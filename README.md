# 調音 Shirane

Windows 11 x64向け配信ミキサー。Phase 0・1受入済み、現在は **Phase 2（音声コア）** を実装中です。
マイク・ゲーム・Discord・ブラウザー・音楽のMonitor / Stream希望設定を独立編集できます。
**実音声の取得・再生・OBS送出はまだ実装していません。** コアのGain・独立2バスMixerは擬似信号で検証中で、画面の設定やデバイスには未接続です。メーターは架空の信号を表示しません。

「デバイス / DEVICES」で実機の入力・出力一覧、アプリ別OSセッションと音量を確認できます。
パネルが狭い場合は上の境界をドラッグして広げてください。
マイク・Monitor・Streamの選択はendpoint IDで保存し、切断時も保持します。
MonitorとStreamは同じ出力先を選べません。既定デバイスの表示はmultimedia役割です。
OS音量は読取専用で、内部gainとは連動しません。選択だけで録音・再生は開始しません。

### マイクの抜き差し確認

「デバイス / DEVICES」で取り外してよいUSBマイクを選び、「マイクの抜き差し確認」を開きます。
「確認開始」→USBを抜く→「切断を観測」の表示を待つ→同じポートに戻す、の順で操作します。
120秒以内に同じIDで復帰し、切断・復帰の各区間で通知件数が増えた場合に「状態復帰を観測」と表示します。
選択変更・取得失敗・通知不足・時間切れは未合格です。「確認をリセット」で再試行できます。
通知件数は全デバイス合計なので他デバイスの変更を重ねないでください。音声復旧を検証する機能ではありません。
状態遷移は `shirane.log` に数値で記録します（1:切断待ち、2:再接続待ち、3:状態復帰観測、4〜7:未合格、0:未実施）。

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

UIスレッドが希望設定、Win32、ImGui、DX11を単独所有します。
Phase 1のDeviceServiceは専用MTAスレッドでCOMを生成・列挙・解放し、mutex保護したスナップショットのコピーをUIへ渡します。
MMDevice通知はatomicカウンターのみを更新し、workerが最大250ms間隔で確認します。
セッションは2秒ごとの時点一覧で、短命セッションの完全なイベント追跡は未実装です。
初期化失敗は画面にHRESULTと再起動案内を表示し、列挙中の一部失敗は警告付き一覧として公開します。
通知解除APIが失敗した場合はdebug出力に記録し、無効ポインター呼出しを避けるため自己完結したcallbackをプロセス終了まで保持します。
Control worker、音声worker、RT経路はまだありません。音声coreはOS・UIに依存しない空のターゲットです。
COMリソースはComPtr、UIとウィンドウ寿命はAppHostのRAIIで管理します。
GPU初期化に失敗した場合はWARPを試し、描画失敗はHRESULTをログに記録して安全終了します。
GPU device lostからのUI自動復旧は未実装です。後続フェーズでは音声寿命をUIから分離します。
設定ロードは候補オブジェクトを検証した後だけ反映し、保存は同一フォルダーの一時ファイルから置換します。
同一ユーザーの複数プロセスによる同時設定保存は未対応です。
現在の設定はschema 2。schema 1からは既存音量・配置を保ち、全デバイス未選択として移行します。
読込だけでは元ファイルを書き換えず、明示保存でschema 2へ更新します。schema 0 / 将来版は拒否・保持します。

## 検証と次段階

検証記録は [docs/VALIDATION.md](docs/VALIDATION.md)。CIにはDebug/Releaseのビルド・単体テストを定義しています。
CIで音声実機テストを成功扱いしません。
Phase 0の受入はDebug/Release起動、Docking保存復元、CI単体成功です。
2026-11-07、ローカルGUI確認とGitHub CIの両構成10/10成功によりPhase 0を受入済みです。
Phase 1の列挙・通知登録・選択保存を実装しました。2026-11-08にUSB 2.0 Cameraのマイクで抜差し・選択ID維持を確認済みです。

- 2026-11-08にWindows既定入力の変更・復帰とShiraneの選択ID維持も実機確認済みです。
- Phase 1受入済み: 2026-11-08、[GitHub CI](https://github.com/rumo-dev/Shirane/actions/runs/42480242047)のDebug/Releaseビルド・単体各24/24成功と実機確認を完了。他機器の抜差しは未検証。
- 既定入力の実機確認: デバイス画面の「既定の入力変更を確認」で開始し、Windows側で別入力へ変更。変更観測後に元へ戻します（180秒以内）。Shiraneの選択は固定し、結果を画面とログで確認します。このUIはWindows設定を変更しません。
- TODO Phase 2: pool/SPSC、Mixer、DSP基礎。受入: 擬似信号、可変ブロック、RT割当ゼロ、時間測定。
- Phase 2着手: 固定容量pool/SPSCとcoreテスト6件を追加。ローカルDebug/Releaseは各30/30成功。実音声には未接続で、Mixer/DSPなどPhase 2全体の受入はまだです。
- Phase 2追加: GainRamp・16入力のMonitor/Stream Mixerを実装。gain/pan/mute/solo、240sampleの通常ランプ、48sampleの緊急ミュートを検証し、ローカルDebug/Release各40/40成功。pan中央は左右各−3.01dB、左右端では反対側をゼロにするequal-power balanceです。Phase 2全体は未受入です。
- TODO Phase 3以降: 実音声取得と再生。受入は設計文書の各Phaseに従います。
- Phase 2追加: 内部48kHz Clock、50ms peak/RMS Meter、DSP共通APIとGainProcessorを実装。接続テストを含めローカルDebug/Release各51/51成功。Meterは画面未接続。次は長時間転送の検証とPhase 2全体の受入確認です。

[開発規約](AGENTS.md) · [全体設計](docs/ARCHITECTURE.md) · [音声契約](docs/AUDIO_ENGINE.md)

実装時に参照したMicrosoft資料:
[D3D11CreateDeviceAndSwapChain](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-d3d11createdeviceandswapchain)、
[DXGI](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi)、
[MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)。

Phase 1参照:
[MMDevice通知](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient)、
[セッション列挙](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-getsessionenumerator)、
[GetProcessId](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol2-getprocessid)。

読取専用実機試験は `out/build/windows-vs2022-x64/Release/shirane_device_probe.exe`。
ハードウェアなしは終了コード77（skip）、API失敗は非ゼロです。通常CIの単体には含めません。
CTestに登録する場合はconfigure時に `-DSHIRANE_ENABLE_HARDWARE_TESTS=ON` を指定し、`-L hardware` で実行します。

擬似音声コアの60秒連続転送試験は、configure時に `-DSHIRANE_ENABLE_ENDURANCE_TESTS=ON` を指定し、`ctest --preset windows-release -L endurance --output-on-failure` で実行できます。Debugも対応します。通常の単体確認は `-L unit`。CIは両構成で単体と連続転送を別工程として実行します。実音声・1時間/8時間耐久試験の代わりではありません。
