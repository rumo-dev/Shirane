# ARCHITECTURE.md — Stream Audio Studio 全体設計

## 実装状況 — 調音 Shirane / 2026-11-07

Phase 0の `Shirane`、`sas_ui`、`sas_config`、`sas_audio_core`、`sas_tests` を実装。
UIスレッドが希望設定と描画資源を所有し、保存は明示操作で行う。現時点でControl・音声スレッドはない。
Phase 1では `sas_platform` のDeviceServiceが専用MTAスレッドでデバイス・セッションを読取専用列挙する。
MMDevice callbackは自己完結したatomic通知カウンターのみを更新。workerが250ms間隔で確認し、2秒ごとの一覧再取得も行う。
UIはmutex保護した所有スナップショットをコピーし、COMポインターを受け取らない。
セッションは時点一覧であり、短命セッションや生成イベントの完全追跡は未実装。
schema 2にマイク・Monitor・Streamの固定endpoint IDを追加。schema 1は未選択へ移行し、欠損デバイスIDは保持する。
既定変更は表示へ反映するが選択先は自動変更しない。同一Monitor/Stream出力は禁止。
Phase 1の実機抜差しはUSB 2.0 Cameraのマイクで同一選択IDの復帰を確認。Windows既定入力の変更・復帰時も一覧追従と固定選択ID維持を確認済み。2026-11-08にCIのDebug/Release各24テスト成功と合わせてPhase 1を受入済み。OS音量変更は任意機能として未実装。
既定入力の検証補助DefaultDeviceCheckもUIが所有する。開始時のmultimedia既定IDとShiraneの選択IDを保持し、180秒以内の既定変更・復帰と各区間の全体通知増加を判定する。Windows設定の操作はユーザーが行い、判定器はOS設定を変更しない。
Phase 1検証補助としてUI所有のDeviceCheckを追加。選択マイクの有効→消失/未接続→有効を、
固定ID・新しいsnapshot generation・区間ごとの全体通知件数増加とともに120秒以内で観測する。
一覧不完全、選択変更、通知不足、時間切れは未合格。擬似snapshotの単体試験と実機受入を区別する。
Monitor/Stream操作は設定編集のみ。初期sendは全てmute、音声開始は無効。
Phase 0・1はローカルGUI・実機確認とGitHub CIのDebug/Release単体成功により受入完了。次はPhase 2。受入記録は `VALIDATION.md` を参照。
以下は将来の音声実装を含む設計仕様であり、実装済みを示さない。

設計版1.0 / 2026-11-07。開発規約はリポジトリルートの `AGENTS.md`、音声詳細は同階層の `AUDIO_ENGINE.md` を参照。本文の設定値は初期設計で、性能保証ではない。

## 1. 目的とスコープ

Windows 11 x64、VS2022、C++20、CMake上で、マイク・ゲーム・Discordを低遅延に処理する。Win32、Dear ImGui Docking、DirectX 11を採用し、Monitor/Streamの独立フェーダー、mute、solo、send、DSPを提供する。

初期リリースはShared Mode、mono/stereo、内部48kHz float32に集中する。仮想デバイスドライバー、VST3、ノイズ抑制、ボイスチェンジャー、多チャンネル、Exclusiveは後段とする。アプリキャプチャは音を複製する機能であり、元アプリの再生先を自動的に差し替えるものではない。

## 2. 論理構成

```text
UI / Hotkey / OBS control
             │
          Control ── Config / DeviceManager / ProcessCatalog
             │ prepared graph + commands
             ▼
Mic WASAPI ── input SPSC ── clock adapter / SRC ─┐
Game process loopback ── SPSC ── SRC ───────────┼─ channel common DSP
Discord process loopback ── SPSC ── SRC ────────┘       │
                                            independent sends
                                                ┌────┴────┐
                                           Monitor      Stream
                                          bus DSP       bus DSP
                                                │          │
                                         output adapter   SPSC
                                                │          │
                                          headphones    output worker
                                                           │
                                                  OBS audio transport
```

EngineはMonitorのrender eventをマスターとして1回だけ両Busを計算する。Stream workerはDSPグラフを再実行せず、完成済みStreamフレームを消費する。Monitor停止中はQPCベースのHeadless Engine Driverへ明示的に所有権を移す。2つのdriverが同時にEngineを実行しない。

## 3. モジュールと主要クラス

| モジュール | 主な型 | 責務 / 依存 |
|---|---|---|
| app | AppHost、ControlService | 起動、状態遷移、全体寿命管理 |
| audio/core | AudioBlockView、AudioBufferPool、SpscQueue、AudioClock | OS非依存のデータと基本演算 |
| audio/wasapi | WasapiCapture、WasapiRender、ProcessLoopbackCapture | COM、形式交渉、イベント、パケット転送 |
| audio/capture | InputSource、ProcessBinding | 入力ID、プロセス世代、取得状態 |
| audio/mixer | Engine、ChannelStrip、BusMixer | 1マスター時刻上のミックス |
| audio/routing | RoutingGraph、GraphCompiler | DAG検証、send、遅延とメモリー予算 |
| audio/dsp | IAudioProcessor、Gain、Eq、Gate、Compressor、Limiter | prepare/process/reset/latency |
| audio/sync | ClockEstimator、AdaptiveResampler、QuantumAdapter | 異クロック、パケットとブロックの変換 |
| audio/diagnostics | RtCounters、TraceEvent | 固定サイズの観測データ |
| platform | DeviceManager、ProcessCatalog、MmcssRegistration | デバイス通知、プロセス列挙、Windows資源 |
| integrations/obs | ObsControlClient、IStreamSink | 制御と音声転送を分離 |
| config | ProfileStore、SchemaMigration | JSON検証、保存、復元 |
| ui | Mixer/Routes/Effects/Devices/Diagnostics panels | ImGui描画のみ |

依存方向はUI→Control API、Control→audioとplatform、WASAPI adapter→audio core。audio coreからUI/Win32/DX11/JSON/spdlogを参照しない。依存ライブラリの型をDSP公開APIへ漏らさない。

## 4. 配置とビルドターゲット

```text
StreamAudioStudio/
  AGENTS.md
  CMakeLists.txt
  CMakePresets.json
  vcpkg.json
  README.md
  docs/ARCHITECTURE.md
  docs/AUDIO_ENGINE.md
  src/app/
  src/audio/{core,wasapi,capture,mixer,routing,dsp,sync,diagnostics}/
  src/platform/
  src/ui/{panels,widgets}/
  src/integrations/obs/
  src/config/
  include/
  tests/{unit,integration}/
  assets/
  tools/
```

`sas_audio_core`、`sas_wasapi`、`sas_platform`、`sas_config`、`sas_obs`、`sas_ui`、`StreamAudioStudio`、`sas_tests`に分ける。必要なWindowsライブラリはole32、uuid、avrt、mmdevapi、d3d11、dxgi等を使用APIに応じて各targetへ限定してリンクする。非同期音声activationにはMmdevapi.libを使う。DirectX11のdevice lostやUI停止はaudio coreの再起動条件にしない。

## 5. スレッドと所有権

| 役割 | 所有状態 | 入出力 / 優先度 |
|---|---|---|
| UI Thread | Win32、ImGui、DX11 | 通常優先度、スナップショット表示 |
| Control Thread | 希望構成、graph slot、開始停止 | UI要求を直列化、RT向けSPSCの唯一producer |
| Device Management | MMDevice、通知、再接続候補 | 通知callbackは最小イベントを送る |
| Mic Capture Thread | mic client、capture packet | MMCSS、mic SPSCのproducer |
| Application Capture Workers | appごとのclient | 各入力SPSCのproducer、入力数上限あり |
| Monitor Render / Engine | 現行graph、DSP、同期状態 | MMCSS、全入力consumer、両Bus生成 |
| Stream Output Worker | sink、送出SRC | Stream SPSCのconsumer、音声render時はMMCSS |
| Diagnostics / Recorder | 集計、ログ、録音ファイル | 非RT、producerごとのqueueを消費 |

Application Loopbackの非同期activation callbackはControlへ結果を返す。callbackの寿命をキャンセル後も完了まで保持する。COM apartment、インターフェースのagility/marshaling、GetBuffer/ReleaseBufferの同一スレッド要件を守り、ComPtrを別スレッドへ移すだけで安全としない。

## 6. グラフ・パラメーター更新

ControlがGraphCompilerで接続、チャンネル数、DSP遅延、CPU/メモリー予算、循環と重複を検証する。prepare済みGraphを固定slotへ置き、generation付きコマンドで公開。Engineはブロック境界で切替え、利用終了generationをACK。ControlがACK後に旧DSPとメモリーを破棄する。プール枯渇時は編集を保留する。

gain等の頻繁な更新はControlで統合し、Engineがランプとして適用する。キュー満杯はUIへ保留状態を返す。緊急ミュートのみ独立atomic latchを使う。通常のshared_ptr交換、seqlock上の非atomic構造体、単純double bufferによるデータ競合を避ける。

## 7. ChannelとBusの契約

Channelはsource ID、共通trim/DSP、Monitor send、Stream sendを持つ。sendはgain、pan、mute、solo対象を独立に保持する。soloはBusローカルとし、他Busを変えない。初期仕様ではsendは共通DSP後から分岐する。Monitorで高遅延DSPを避けたい場合は、そのDSPをStream専用branchへ配置する。

Monitor Busはヘッドホン出力と低遅延重視。マイクMonitorは初期OFF、Streamは安全な起動ミュートから明示解除する。Stream Busは独立master、peak監視、limiterを持つ。録音はBusまたは個別channelから専用queueへ送り、録音遅延やディスク停止がMonitorを止めない。

Stream内部の並列信号は必要に応じて最大DSP遅延へ補償する。MonitorまでStreamに合わせて遅らせない。異なるBus間の時間差は診断に表示し、映像同期はOBS側の音声同期設定と併せて検証する。

## 8. 二重再生・フィードバック防止

初期のゲーム/Discordモニターは「元アプリの直接出力を聞く、SASのMonitor sendをOFF」とする。StreamのみをSASでミックスできる。この方式ではゲーム/DiscordのMonitor音量はSASフェーダーで変えられず、UIに直接出力と表示する。

全音声をSASでMonitor制御する構成は、元アプリを別の物理出力または利用者が導入した仮想ケーブルへルーティングし、SASだけがヘッドホンへ出力する。無音化によってloopback取得も消えないことを実機確認するまで、session muteを自動適用しない。

自プロセス、OBS送出先、同一process treeを含む重複sourceを検出する。親子treeの重複は設定を拒否するか明示的に解消する。endpoint loopbackとapp loopbackの併用は同一音声を重ね得るため既定で禁止。Discordの受信音声をDiscord入力へ返さない。Discord送信用の第三Busが必要なら将来のmix-minus経路として追加し、Stream Busをそのまま流用しない。

## 9. OBSへの音声送出

WebSocketはシーン検知、配信状態、プリセット切替等の制御専用。PCM転送に使用しない。

最初の実音声接続は利用者が導入した外部仮想ケーブルを用いる。SASのStream sinkがケーブルのrender endpointへWASAPI出力し、OBSが対になるcapture endpointを音声入力ソースとして取得する。Stream endpointはMonitor endpointと別IDであることを検証する。仮想ケーブルがなければ、別の物理デバイスのライン接続か、将来のOBS専用source pluginが必要で、Stream BusだけではOBS入力デバイスは生成されない。

OBS側では重複するDesktop Audio、Mic、Application Audio Captureを無効にし、Stream入力のモニタリングをOFFにする。SASから見えるデバイス名は製品ごとに異なるため、render/captureの方向とendpoint IDで案内する。

将来の自前transportは共有メモリーPCMリング＋OBS source pluginを候補とする。timestamp、format、再接続、backpressure、バージョン互換を別ADRで決める。自前仮想ドライバーより先に評価する。

## 10. ライフサイクルと復旧

状態はStopped→Preparing→Priming→Running→Recovering→Running、またはFailed。停止はRunning→Stopping→Stopped。開始時に設定検証、入力・出力準備、メモリー確保、キュー初期化、短いpriming、gain rampの順に行う。

切断通知やDEVICE_INVALIDATED時は、音声側が失敗番号を通知し、そのsourceだけ無音化する。Controlがbackoff付き再試行を行い、endpoint IDで再識別して別generationとして差し替える。特定ID固定か既定デバイス追従かをプロファイルに保存し、勝手にスピーカーへ切替えない。

Monitor喪失時は旧driver停止と所有権ACKを待ってHeadlessへ移行し、Streamを継続する。再接続では新しいclock epochを開始し、入力・出力の同期状態をresetして短くcrossfadeする。安定性確認まで安全なミュート状態を維持する。

停止では新規要求を止め、停止eventを通知、worker終了を非RTでjoinし、未完了activationを完了処理、COM/HANDLE/DSPを破棄する。停止・再接続のgenerationで古いcallbackを無効化する。

## 11. UIと永続化

Mixer、Routing、Effects、Devices、DiagnosticsをDocking panel化する。UIは希望設定と適用済み設定を区別し、未対応・復旧中・直接モニター・音声未接続を表示する。Diagnosticsは実際の形式/周期、FIFOの容量と滞留量、欠落、SRC補正ppm、処理時間、測定レイテンシーを表示する。

JSONはschemaVersion付き。endpoint ID、既定追従設定、process再識別条件、channel UUID、Bus、DSP順序、ImGui layoutを保存する。PID再利用を避けるため実行パスとプロセス開始時刻を検証する。自動再bindingは候補を提示し、別アプリを無条件に取得しない。

## 12. 開発フェーズと受入条件

| Phase | 実装内容 | 次段階への受入条件 |
|---|---|---|
| 0 基盤 | CMake/vcpkg、Win32、ImGui Docking、DX11、ログ、JSON、テスト | VS2022 Debug/Release起動、Dockingと保存復元、CI単体成功 |
| 1 デバイス | MMDevice通知、session列挙、任意のOS音量操作 | 追加削除追従、選択維持、OS音量と内部gainの区別 |
| 2 Core | pool/SPSC、Mixer、Bus、Clock、Meter、Gain、DSP API | 擬似信号と可変ブロック、割当ゼロ、時間計測 |
| 3 WASAPI | Shared render/capture、IAudioClient3、MMCSS、mic Monitor | 実周期表示、切断復旧、実測遅延、1時間glitch評価 |
| 4 アプリ | process loopback、ゲーム/Discord、SRC/drift | 独立取得、重複回避、再起動、8時間FIFO安定 |
| 5 Bus | 独立send、mute/solo、複数sink | Monitor操作がStreamへ影響しない、異クロック出力安定 |
| 6 DSP | EQ/Gate/Comp/Limiter/Ducking、遅延補償 | impulseと周波数応答、click、負荷、Bus遅延確認 |
| 7 OBS | 外部ケーブルでPCM送出、WebSocket制御 | OBS録画で二重取得なし、同期、再接続、緊急ミュート |
| 8 拡張 | 録音分離、VST3、抑制、voice、仮想デバイス検討 | 機能ごとに別ADRと負荷・障害隔離試験 |

Phase 1のOS session音量は元アプリとcaptureレベルにも影響し得る。内部チャンネルフェーダーと連動させない。Phase 7完了までOBS実音声送出を達成済みと表示しない。VST3を別プロセス化する場合もIPC buffering遅延を予算化し、単一プロセスの例外捕捉だけでクラッシュ隔離と称しない。

## 13. 品質目標

DSP計算時間は内部ブロック時間の25%未満を目標とし、全render service時間と別に記録する。mic Monitor実測10ms以下、アプリ追加遅延10ms以下は対応デバイスでの開発目標。通常負荷1時間のunderrun/overrunゼロと8時間耐久を評価する。達成条件と測定方法は `AUDIO_ENGINE.md` に従い、未測定値を保証しない。
