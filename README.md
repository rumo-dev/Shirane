# 調音 Shirane

Windows 11 x64向け配信ミキサー。現在は **Phase 0（基盤）** の実装です。
マイク・ゲーム・Discord・ブラウザー・音楽のMonitor / Stream希望設定を独立編集できます。
**音声の取得・再生・DSP・OBS送出はまだ実装していません。** メーターは架空の信号を表示しません。

## 起動とビルド

Visual Studio 2022の「C++によるデスクトップ開発」、MSVC v143、Windows SDK 10.0.26100.0、CMake 3.25以上、Git、vcpkgが必要です。
初回構成はネットワーク接続が必要です。vcpkgの位置は `VCPKG_ROOT` で指定します。
vcpkg本体もbaselineと対応した版を使用してください。VS同梱の古いvcpkgでは依存構築に失敗します。
この作業環境では `.tools/vcpkg` にbaseline版とvcpkg-tool 2026-10-29を用意しています。
別環境では次のように準備できます（既存の `.tools/vcpkg` がない場合）。

```powershell
git clone https://github.com/microsoft/vcpkg.git .tools/vcpkg
git -C .tools/vcpkg checkout 6fdd23fa0a9479a9f9a1c48178696f60cf39255d
./.tools/vcpkg/bootstrap-vcpkg.bat -disableMetrics
$env:VCPKG_ROOT = "$PWD/.tools/vcpkg"
```

```powershell
./tools/build.ps1 -Configuration Debug
./tools/build.ps1 -Configuration Release
./out/build/windows-vs2022-x64/Release/Shirane.exe
```

両構成のビルド・単体・実ウィンドウ起動検証をまとめる場合:

```powershell
./tools/verify.ps1 -Smoke
```

単体テストの一時データはビルドフォルダーの `test-data/` に作成します。
起動検証は20フレームで終了し、通常のユーザー設定を読まず、ログを `out/smoke/` に分離します。
描画環境のないCIでは `-Smoke` を付けず、音声やGUIの実機成功とは扱いません。
vcpkgのダウンロードとバイナリキャッシュは既定で `out/downloads/` と `out/cache/` に保存します。

標準コマンド（CMakeがPATHにある環境）:

```powershell
cmake --preset windows-vs2022-x64
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure
cmake --build --preset windows-release
ctest --preset windows-release --output-on-failure
```

## 使い方

- 各入力のMONITOR / STREAMでgain、pan、Mute、Soloを設定します。音声には未適用です。
- 「設定を保存」で設定とDocking配置を `%LOCALAPPDATA%\Shirane\profile.json` へ保存します。
- 起動時に保存設定を復元。「再読込」で最後に保存した状態へ戻します。
- パネルのタブをドラッグして配置を変更。「配置を戻す」で初期配置に戻ります。
- 終了時の自動保存はありません。壊れた設定、未対応schemaを勝手に上書きしません。
- ログは同フォルダーの `shirane.log`。1 MiB × 3世代でローテーションします。
- 日本語フォントはWindowsのMeiryoを利用します。フォントの再配布はしません。

## 固定依存

| 項目 | 固定値 |
|---|---|
| C++ / toolset | C++20 / MSVC v143 |
| Windows SDK | 10.0.26100.0 |
| vcpkg baseline | `6fdd23fa0a9479a9f9a1c48178696f60cf39255d` |
| nlohmann/json | 3.12.0 port 2 |
| spdlog | 1.17.0 port 1 |
| GoogleTest | 1.18.0 |
| Dear ImGui | v1.91.9b-docking / `52fe0a05a7b1aa180a202bb24f0f2a049a9c1b7d` |

ImGuiはDocking commitをFetchContentで固定し、`IMGUI_HAS_DOCK` をコンパイル時に検査します。
非Docking版への暗黙置換はできません。vcpkg依存はstatic tripletでリンクします。

## 所有権・失敗時動作

Phase 0ではUIスレッドが希望設定、Win32、ImGui、DX11を単独所有します。
Control worker、音声worker、RT経路はまだありません。音声coreはOS・UIに依存しない空のターゲットです。
COMリソースはComPtr、UIとウィンドウ寿命はAppHostのRAIIで管理します。
GPU初期化に失敗した場合はWARPを試し、描画失敗はHRESULTをログに記録して安全終了します。
GPU device lostからのUI自動復旧は未実装です。後続フェーズでは音声寿命をUIから分離します。
設定ロードは候補オブジェクトを検証した後だけ反映し、保存は同一フォルダーの一時ファイルから置換します。
同一ユーザーの複数プロセスによる同時設定保存は未対応です。
schema 1が初版です。schema 0 / 将来版は拒否・保持し、移行は実在する旧schema導入時に追加します。

## 検証と次段階

検証記録は [docs/VALIDATION.md](docs/VALIDATION.md)。CIにはDebug/Releaseのビルド・単体テストを定義しています。
CIで音声実機テストを成功扱いしません。
Phase 0の受入はDebug/Release起動、Docking保存復元、CI単体成功です。
受入が揃うまではPhase 1へ進めません。

- TODO Phase 1: デバイス列挙・通知。受入: 追加削除追従、選択維持、OS音量と内部gainの区別。
- TODO Phase 2: pool/SPSC、Mixer、DSP基礎。受入: 擬似信号、可変ブロック、RT割当ゼロ、時間測定。
- TODO Phase 3以降: 実音声取得と再生。受入は設計文書の各Phaseに従います。

[開発規約](AGENTS.md) · [全体設計](docs/ARCHITECTURE.md) · [音声契約](docs/AUDIO_ENGINE.md)

実装時に参照したMicrosoft資料:
[D3D11CreateDeviceAndSwapChain](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-d3d11createdeviceandswapchain)、
[DXGI](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi)、
[MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)。
