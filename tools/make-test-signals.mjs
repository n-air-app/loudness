#!/usr/bin/env node
// ラウドネス計測の精度確認用テスト信号を作る
//
// ITU-R BS.1770 / EBU Tech 3341 の基準: 1kHz のサイン波をステレオ両チャンネルに
// 振幅 A (dBFS) で入れると、ラウドネスは A LUFS になる (許容 ±0.1 LU)。
// K-weighting の 1kHz での利得とチャンネル和の補正がちょうど打ち消し合うため。
//
//   node tools/make-test-signals.mjs <ffmpeg.exe のパス> [出力ディレクトリ]
//
// ffmpeg は obs-studio-node に同梱されているもの (node_modules/obs-studio-node/ffmpeg.exe) が使える。
// 生成後、ffmpeg の ebur128 フィルタでも値を測って表示する (プラグインとの比較用)。

import { execFileSync, spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const [ffmpeg, outDirArg] = process.argv.slice(2);
if (!ffmpeg) {
  console.error('usage: node tools/make-test-signals.mjs <ffmpeg.exe> [outDir]');
  process.exit(1);
}

const outDir = outDirArg ?? path.join(path.dirname(fileURLToPath(import.meta.url)), 'test-signals');
mkdirSync(outDir, { recursive: true });

const SAMPLE_RATE = 48000;
const DURATION_SEC = 60;
// -15: ニコ動の基準値 / -23: EBU R 128 の基準値 / -33: 小さすぎる例
const LEVELS_DBFS = [-15, -23, -33];

for (const db of LEVELS_DBFS) {
  const amp = (10 ** (db / 20)).toFixed(8);
  const file = path.join(outDir, `sine-1khz-stereo${db}dbfs.wav`);
  const wave = `${amp}*sin(2*PI*1000*t)`;

  execFileSync(ffmpeg, [
    '-hide_banner', '-loglevel', 'error', '-y',
    '-f', 'lavfi', '-i', `aevalsrc=${wave}|${wave}:s=${SAMPLE_RATE}:d=${DURATION_SEC}`,
    '-c:a', 'pcm_f32le', file,
  ]);

  // ebur128 フィルタの集計結果は stderr に出る
  const report = spawnSync(ffmpeg, ['-hide_banner', '-nostats', '-i', file, '-af', 'ebur128', '-f', 'null', '-'], {
    encoding: 'utf8',
  }).stderr;
  const integrated = report.match(/Summary:[\s\S]*?I:\s+(-?[\d.]+) LUFS/)?.[1] ?? '?';
  console.log(`${path.basename(file)}  expected ${db.toFixed(1)} LUFS, ffmpeg ebur128 I = ${integrated} LUFS`);
}
