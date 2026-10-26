# N Air Loudness

## 概要

N Air Loudness は、N Air の配信音声のラウドネスを計測する OBS プラグインです。
EBU R 128 / ITU-R BS.1770 に従い、[libebur128](https://github.com/jiixyj/libebur128) で計算します。
N Air アプリケーションとは名前付きパイプ上の JSON RPC で通信します (n-air-app/substream と同じ方式)。

関連 issue: [n-air-app/n-air-app#1517](https://github.com/n-air-app/n-air-app/issues/1517)

### 計測できるもの

- Momentary (400ms) / Short-term (3s) / Integrated / Loudness Range (LRA)

### 計測対象 (タップ)

| type | 取得位置 | 用途 |
|---|---|---|
| `track` | 出力ミックストラック (`obs_add_raw_audio_callback`) | 配信・録画に乗る音そのもの |
| `source` (`postFader: false`) | ソースのフィルタ適用後・ミキサー音量適用前 (`obs_source_add_audio_capture_callback`) | マイク入力そのものの大きさ |
| `source` (`postFader: true`) | 上記にミキサー音量・ミュートを反映 | ミキサー上のバランス |

メーターは複数同時に作れます。

## 互換性

| Component | Version |
|---|---|
| Target obs-studio-node | 0.25.56 |
| Build SDK LibOBS | 30.2.4sl41 |
| Architecture | Windows x64 |

LibOBS のヘッダと import library は `thirdparty/obs-libs/` にあります。
obs-studio-node を更新するときは、対応するヘッダ・`obs.lib`・`nair-loudness/version.h` をまとめて更新してください。

## ビルド

### 必要なもの

- Visual Studio 2022 以上 (C++ によるデスクトップ開発)

### ビルド手順

```powershell
# Developer PowerShell for VS で実行するか、MSBuild をフルパスで実行
msbuild nair-loudness.sln /p:Configuration=Release /p:Platform=x64
```

成果物は `x64\Release\nair-loudness.dll` です。N Air で試すときは以下に置きます。

```powershell
copy .\x64\Release\nair-loudness.dll <n-air-app>\node_modules\obs-studio-node\obs-plugins\64bit\
```

配布時の DLL 署名は N Air のパッケージング時に行うため、このリポジトリでは署名しません。

## 動作確認

DLL を置いた N Air を起動しておき、別のターミナルから `tools/probe.mjs` を実行します (Node.js 18 以降、依存パッケージなし)。

```powershell
node tools/probe.mjs --list                 # プラグイン情報と音声ソース一覧だけ表示
node tools/probe.mjs                        # トラック1 + マイク系ソース (pre / post) を 500ms ごとに表示
node tools/probe.mjs --track 0 --source <sourceId> --interval 1000 --duration 60
```

`Ctrl+C` で終了すると、作成したメーターを破棄してから切断します。
OBS のログに `[nair-loudness] plugin ... loaded` が出ていればプラグインは読み込まれています。

## 通信プロトコル

名前付きパイプ `\\.\pipe\NAirLoudness` に接続し、改行区切りの JSON を送受信します。
同時に接続できるクライアントは 1 つで、切断後は再接続できます。

リクエスト:

```json
{"id": "一意なID", "fn": "関数名", "arg": {}}
```

レスポンス:

```json
{"id": "リクエストと同じID", "res": {}}
```

失敗時は `res` が `{"error": "メッセージ"}` になります。

### 関数

#### `info`

プラグインと OBS の音声設定を返します。

```json
{"version": "0.1.0", "targetLibobs": "30.2.4sl41", "targetObsStudioNode": "0.25.56",
 "runtimeLibobs": "30.2.4", "sampleRate": 48000, "channels": 2}
```

#### `listSources`

音声出力を持つ OBS ソースの一覧を返します。`create` の `source` に渡す名前の確認用です。

```json
{"sources": [{"name": "wasapi_input_capture_xxxx", "id": "wasapi_input_capture"}]}
```

N Air では OBS 上のソース名は表示名ではなく N Air の `sourceId` です。

#### `create`

メーターを作成して計測を開始します。

```json
{"type": "track", "track": 0}
{"type": "source", "source": "マイク", "postFader": false}
```

- `track`: 0 始まりのミックストラック番号。配信に乗るのはシンプルモードでは 0、詳細モードでは `AdvOut.TrackIndex - 1`
- `source`: OBS のソース名 (N Air の `sourceId`)

レスポンス: `{"meterId": 1}`

#### `get`

`{"meterId": 1}` → 現在の値

```json
{"momentary": -18.2, "shortTerm": -17.9, "integrated": -18.5, "range": 4.1, "frames": 480000}
```

値が確定していない項目 (計測開始直後や無音) は `null` です。単位は `range` のみ LU、他は LUFS です。

#### `reset`

`{"meterId": 1}` → Integrated / LRA を含めて計測をやり直します。

#### `destroy`

`{"meterId": 1}` → メーターを破棄します。

#### `destroyAll`

すべてのメーターを破棄します。クライアントの再接続時などに使います。

## ライセンス

GPL-2.0-or-later。詳細は [LICENSE](LICENSE) を参照してください。

同梱しているサードパーティのライセンスは [thirdparty/README.md](thirdparty/README.md) を参照してください。
