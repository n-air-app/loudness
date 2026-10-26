#!/usr/bin/env node
// nair-loudness 動作確認用クライアント
//
// N Air (nair-loudness.dll を読み込んだ状態) を起動しておき、別のターミナルから実行する。
// 依存パッケージなし。Node.js 18 以降。
//
//   node tools/probe.mjs                       # info / listSources を表示して、トラック1 + マイク系ソースを計測
//   node tools/probe.mjs --track 0 --track 1   # 計測するミックストラック (0 始まり) を指定
//   node tools/probe.mjs --source <sourceId>   # 計測するソースを指定 (pre / post の両方を作る)
//   node tools/probe.mjs --interval 1000 --duration 60
//   node tools/probe.mjs --list                # info / listSources だけ表示して終了
//
// Ctrl+C で終了すると、作ったメーターを破棄してから切断する。

import net from 'node:net';
import { parseArgs } from 'node:util';

const PIPE_NAME = '\\\\.\\pipe\\NAirLoudness';
const MIC_SOURCE_TYPES = ['wasapi_input_capture', 'nair-rtvc-source'];

const { values: opts } = parseArgs({
  options: {
    track: { type: 'string', multiple: true },
    source: { type: 'string', multiple: true },
    interval: { type: 'string', default: '500' },
    duration: { type: 'string' },
    list: { type: 'boolean', default: false },
  },
});

class Client {
  #socket;
  #buffer = '';
  #pending = new Map();
  #nextId = 1;

  connect() {
    return new Promise((resolve, reject) => {
      this.#socket = net.createConnection(PIPE_NAME, resolve);
      this.#socket.once('error', reject);
      this.#socket.on('data', (data) => this.#onData(data));
      this.#socket.on('close', () => {
        for (const { reject: r } of this.#pending.values()) r(new Error('connection closed'));
        this.#pending.clear();
      });
    });
  }

  #onData(data) {
    this.#buffer += data.toString('utf8');
    const lines = this.#buffer.split('\n');
    this.#buffer = lines.pop() ?? '';
    for (const line of lines) {
      if (!line.trim()) continue;
      const message = JSON.parse(line);
      const entry = this.#pending.get(message.id);
      if (!entry) continue;
      this.#pending.delete(message.id);
      clearTimeout(entry.timer);
      entry.resolve(message.res);
    }
  }

  call(fn, arg = {}) {
    const id = String(this.#nextId++);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#pending.delete(id);
        reject(new Error(`timeout: ${fn}`));
      }, 1000);
      this.#pending.set(id, { resolve, reject, timer });
      this.#socket.write(JSON.stringify({ id, fn, arg }) + '\n');
    });
  }

  close() {
    this.#socket?.end();
  }
}

function assertOk(res, what) {
  if (res?.error) throw new Error(`${what}: ${res.error}`);
  return res;
}

const fmt = (v, unit = '') => (v === null || v === undefined ? '   ---' : `${v.toFixed(1).padStart(6)}${unit}`);

async function main() {
  const client = new Client();
  try {
    await client.connect();
  } catch (e) {
    console.error(`${PIPE_NAME} に接続できません (${e.code ?? e.message})。`);
    console.error('N Air が起動していて、nair-loudness.dll が読み込まれているか確認してください。');
    process.exit(1);
  }

  const info = assertOk(await client.call('info'), 'info');
  console.log('info:', info);

  const { sources } = assertOk(await client.call('listSources'), 'listSources');
  console.log('audio sources:');
  for (const s of sources) console.log(`  ${s.id.padEnd(28)} ${s.name}`);

  if (opts.list) {
    client.close();
    return;
  }

  // 前回の実行が Ctrl+C 以外で終わってメーターが残っていても、ここで掃除する
  assertOk(await client.call('destroyAll'), 'destroyAll');

  const tracks = (opts.track ?? ['0']).map(Number);
  const sourceNames = opts.source ?? sources.filter((s) => MIC_SOURCE_TYPES.includes(s.id)).map((s) => s.name);

  const meters = [];
  for (const track of tracks) {
    const { meterId } = assertOk(await client.call('create', { type: 'track', track }), `create track ${track}`);
    meters.push({ meterId, label: `track${track + 1}` });
  }
  for (const source of sourceNames) {
    for (const postFader of [false, true]) {
      const res = await client.call('create', { type: 'source', source, postFader });
      if (res.error) {
        console.warn(`create source ${source} (${postFader ? 'post' : 'pre'}): ${res.error}`);
        continue;
      }
      meters.push({ meterId: res.meterId, label: `${source.slice(0, 24)} ${postFader ? 'post' : 'pre '}` });
    }
  }

  if (!meters.length) {
    console.error('作成できたメーターがありません');
    client.close();
    process.exit(1);
  }

  const labelWidth = Math.max(...meters.map((m) => m.label.length));
  const header = `${'meter'.padEnd(labelWidth)}  momentary  short-term  integrated     LRA   seconds`;

  let stopping = false;
  const stop = async () => {
    if (stopping) return;
    stopping = true;
    clearInterval(timer);
    try {
      await client.call('destroyAll');
    } catch {
      // 切断済みなら何もしない
    }
    client.close();
  };
  process.on('SIGINT', stop);

  const startedAt = Date.now();
  const durationMs = opts.duration ? Number(opts.duration) * 1000 : Infinity;

  const poll = async () => {
    if (Date.now() - startedAt >= durationMs) return stop();
    const lines = [new Date().toLocaleTimeString(), header];
    for (const m of meters) {
      const r = await client.call('get', { meterId: m.meterId });
      if (r.error) {
        lines.push(`${m.label.padEnd(labelWidth)}  error: ${r.error}`);
        continue;
      }
      const seconds = r.frames / info.sampleRate;
      lines.push(
        `${m.label.padEnd(labelWidth)}  ${fmt(r.momentary)}     ${fmt(r.shortTerm)}      ${fmt(r.integrated)}  ${fmt(r.range)}  ${seconds.toFixed(1).padStart(8)}`,
      );
    }
    console.log(lines.join('\n') + '\n');
  };

  const timer = setInterval(() => poll().catch((e) => {
    console.error(e.message);
    stop();
  }), Number(opts.interval));
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
