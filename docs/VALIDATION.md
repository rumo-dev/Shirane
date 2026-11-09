# Shirane 検証記録

## Phase 2コア受入 — 2026-11-09

- 対象コミット90177a0。[GitHub CI](https://github.com/rumo-dev/Shirane/actions/runs/42620187895)のDebug/Releaseビルド、単体各51/51、擬似連続転送各1/1成功（Debug60.01秒、Release60.00秒）。ローカルも同じ試験と両構成の起動終了smoke成功。
- pool/SPSC、独立Mixer/Bus、内部Clock、Meter、Gain/DSP APIについて、擬似信号・可変ブロック・所有権/境界・C++割当監視・静的RT確認・処理時間測定・60秒並行転送が完了。ARCHITECTUREのPhase 2コア範囲を受入済みとする。
- 次はPhase 3のWASAPI Shared render/capture、IAudioClient3、MMCSS、mic Monitor。開始前に安全なmute/停止/失敗時動作を実装し、実機で周期・切断復旧・遅延・1時間glitchを評価する。
- UIは引き続き希望設定のみ。実音声・device clock写像・異クロックSRC・Graph切替・1時間/8時間実音声耐久を達成済みとはしない。コアの受入はこれらの代用ではない。
- 以下の「Phase 2未受入」「CI未確認」は当時の記録で、この受入結果により更新する。

## Phase 2連続転送 — 2026-11-09

- 2スレッドの擬似信号を実時間60秒連続で転送。固定容量64packetのSPSC、consumer所有pool2×stereo256frames、Gain、独立2Bus Mixer、50ms Meter、内部Clockを通す。frame数1/64/128/256を循環し、開始時に満杯を意図的に発生させる。
- 全packetのsequence/frame位置、全sampleの左右/Bus値、Meter peak/RMS/sequence、Clock積算、pool返却、停止後のqueue排出を検証。両構成成功、producer/consumer監視区間のC++割当・解放とも0。

| 構成 | 実時間ms | block数 | frame数 | 満杯再試行数 | Meter窓数 |
|---|---:|---:|---:|---:|---:|
| Debug | 60,007 | 478,651 | 53,728,431 | 400,051,338 | 22,386 |
| Release | 60,000 | 6,856,751 | 769,670,156 | 512,434,815 | 320,695 |

- 再試行/yieldは試験driverのみ。満杯回数は音声glitch数ではない。実時間ペーシングせず可能な速度で計算し、frame数から実音声の試験時間を推定しない。欠落/重複/信号不一致0、Meter drop0、残存貸出0をassert。
- 条件: Intel Core i7-14700、Windows build26200.9711、高パフォーマンス電源、MSVC14.44/SDK26100。48kHz想定、source L=(frame mod4)×0.125/R=−2L、Gain1、Monitor左端/Stream右端・両gain0dB。device/driver/実周期なし、キュー滞留は0〜64packet、追加DSPなし。steady_clockで60秒、結果XMLはout/endurance-Debug.xmlとout/endurance-Release.xml。
- 通常単体は両構成51/51成功。任意CTest登録はenduranceラベル/90秒timeout。初回のGoogleTest起動フラグ誤記でusageが出た実行は未実施として扱い、正しいフラグで両60秒の完了とXMLを確認した。
- CIにもunitとenduranceを別工程で追加。今回のCI結果確認後、Phase 2コア受入を判定する。
- これは60秒の擬似転送試験であり、1時間/8時間の実音声耐久・実機低遅延・異クロックSRCの合格ではない。C/OS割当の動的フックは未実装で、対象経路は静的に呼出しなしを確認。

## Phase 2 Clock・Meter・DSP共通API — 2026-11-09

- コミット368ccc2の[GitHub CI](https://github.com/rumo-dev/Shirane/actions/runs/42618651104)もDebug/Releaseビルド・各51/51テスト成功。
- AudioBlockのmutable/const view、内部AudioClock、IAudioProcessor/ProcessSpec/Status、GainProcessor、Meterを追加。製品のデバイス・GUIへは未接続。
- Debug/Releaseビルド・単体各51/51成功。初回のテスト内空span初期化のコンパイルエラーは修正済み。
- 追加11テスト: Clockの8時間相当frame計算・100ns絶対変換・epoch・overflow拒否、共通API経由のGainランプ/latency/安全処理、Meterの無音/impulse/sine、block分割一致、異常値/満杯drop、無効入力、1000窓の別スレッド読取り、Gain→Mixer→Meter→Clock接続。
- 8時間相当は擬似frame計算であり、8時間の実行・音声耐久ではない。Meterの並行転送はsnapshot全体のpeak/RMSとsequence整合を検証し、欠落を許すUI用経路として扱う。
- 接続処理を128frames×1000回実行し、C++割当・解放0。RT経路の静的確認でもOS/COM/UI/ログ/待機/確保・解放なし。直接C/OS割当の動的フックは未実装。
- 測定条件: Intel Core i7-14700、Windows build26200.9711、電源「高パフォーマンス」、MSVC14.44/SDK26100。1stereo source（L=0.1/R=0.2）、48kHz、Gain振幅1、Mixer両Bus pan左端/gain0dB、Monitor Meter50ms、Clock advance128。device/driver/実周期/滞留未使用、その他DSPなし。
- steady_clockでGain process＋Mixer process＋Meter process＋Clock advanceを1000回測定（初期ランプも含む、Meter読取りは測定外）。nearest-rank p50/p99/max: Debug132.8/253.7/337.1µs、Release7.0/11.6/22.5µs。out/processor-debug.xml、out/processor-release.xmlに記録。単発環境のコア計測で、最悪時間や実音声低遅延を保証しない。
- 残る確認: 長時間転送、Phase 2全体の受入。device同期・±ppm補正・SRC、Control接続と実音声は後続。

## Phase 2 Gain・独立Mixer — 2026-11-08

- コミット32088adの[GitHub CI](https://github.com/rumo-dev/Shirane/actions/runs/42481696956)もDebug/Releaseビルド・各40/40テスト成功。
- GainRamp、非RTパラメーター準備、固定16入力/2stereo BusのMixerを追加。UI/実音声には未接続。pan規則はAUDIO_ENGINE.mdとGUI tooltipへ明記。
- Debug/Releaseビルド・単体各40/40成功、両構成の20frame起動終了smoke成功。
- 新規10テスト: 無音/mono impulse、stereo sine/pan/headroom、Bus別solo/mute/master、途中変更を含む線形ランプ、1/63/64/128frames分割一致、48sample緊急ミュートとlatch、NaN/Inf/source無音化・subnormal・overflow、無効設定の保持、入力形状/重複/自己feedback拒否、割当監視・時間測定。
- 16stereo source×128frames×2Busを1,000block処理しC++割当・解放は0。直接C/OS割当の動的監視は未実装。RT呼出し経路を静的確認し、固定配列/有界ループのみ、COM/UI/ログ/待機/確保・解放なし。
- 測定条件: Intel Core i7-14700、Windows build26200.9711、電源「高パフォーマンス」、MSVC14.44/SDK26100。全source入力0.01、gain0dB、pan中央、mute/soloなし、master0dB、48kHz想定。デバイス/driver/実周期/音声滞留は未使用。追加DSPなし。
- 1block warmup後、steady_clockでapply+processを1,000回測定。入力有限チェック、出力クリア、全gainランプ・Bus加算を含む。nearest-rank p50/p99/max: Debug 158.5/187.8/230.0µs、Release 8.4/12.0/29.0µs。XMLはout/mixer-debug.xmlとout/mixer-release.xml。単発環境の測定値であり最悪時間保証や実機低遅延の達成ではない。
- Phase 2全体は未受入。Clock/Meter・汎用DSP API・長時間転送、Control接続、異クロック/実音声検証は残る。

## Phase 2基礎バッファ — 2026-11-08

- コミットfee3886の[GitHub CI](https://github.com/rumo-dev/Shirane/actions/runs/42480928920)もDebug/Releaseビルド・各30/30テスト成功。Phase 2全体の受入ではなく、この基礎部品の検証結果。
- 固定容量SpscQueueと単一所有スレッドのAudioBufferPoolを追加。音声エンジンには未接続。
- Debug/Releaseビルド成功、単体各30/30成功（既存24件＋core6件）。初回のCTest複数ラベル引数による登録失敗は単一unitラベルへ修正。
- キュー: 容量1/3/127、空・満杯、1万回の折返し、2スレッド100万payloadの順序・sample整合を確認。1時間/8時間耐久の代替ではない。
- プール: 容量上限、1〜256frames、mono/stereo再利用、ゼロ初期化、他pool・古いhandle・二重返却の拒否を確認。
- 独立テスト実行ファイルで通常/整列付きC++ new/deleteを監視。フック自身の検出を確認し、事前構築後10万回のacquire/view/push/pop/releaseで割当0・解放0。直接malloc/freeやOS割当の動的フックは未実装だが、対象ヘッダーを静的確認し、これらの呼出しはない。
- 測定: Intel Core i7-14700、Windows build 26200.9711、電源「高パフォーマンス」、MSVC14.44、SDK26100。steady_clockで10万回の総時間を1回測定、Debug 65,693µs、Release 1,015µs。最適化・ループ・検証分岐を含む合計で、各操作の最悪時間や実音声性能を保証しない。
- 測定構成: pool4×stereo256frames、使用frames1〜256、queue容量4で1件push後pop。デバイス/driver/実周期/DSPは未使用、音声滞留なし。XMLはローカルout/core-debug.xmlとout/core-release.xml。
- [整列割当のMicrosoft資料](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/aligned-malloc)に従い、テストフックのみで_aligned_malloc/_aligned_freeを対にして使用。製品coreは標準C++のみ。
- Phase 2全体は未受入。Mixer/Bus、Gain/DSP API、Clock/Meter、音声信号テスト、長時間転送、処理時間分布の検証が残る。

## Phase 1受入 — 2026-11-08

- 対象コミット: `d2bcfc48926edb734f0ca8982e54b4db707d14a1`。
- [GitHub Actions実行結果](https://github.com/rumo-dev/Shirane/actions/runs/42480242047): Windows Debug/Releaseのビルド成功、単体テスト各24/24成功。
- CIテスト実行時間: Debug 0.24秒、Release 0.12秒。音声処理時間の測定ではない。
- ローカルの設定保存・復元、USBマイク抜差し、Windows既定入力変更・復帰、固定選択IDの維持と合わせ、Phase 1の読取専用デバイス機能を受入済みとする。
- OS音量操作は任意機能として未実装。短命セッションの完全追跡、他機器の抜差し、音声処理・音声復旧・性能は未検証。
- CIはハードウェア試験を含まない。checkout@v4のNode.js 20非推奨警告あり（ジョブは成功）。
- 次段階: Phase 2のpool/SPSC、Mixer/Bus、基礎DSPを擬似信号・割当監視・時間測定で検証する。

以下のCI未確認記述は当時の記録で、本結果により更新する。

## 実機既定入力変更結果 — 2026-11-08

ユーザーがWindows既定入力を別のマイクへ変更し、元へ戻した。Release画面とログで往復完了を確認。
元のmultimedia既定入力「SteelSeries Sonar - Microphone」への復帰と、Shiraneの「マイク (USB 2.0 Camera)」の選択ID維持を確認した。

| 日本時間 | 状態 | snapshot世代 | 全体通知件数 |
|---|---|---:|---:|
| 21:24:13.778 | 確認開始 / 変更待ち | 22 | 0 |
| 21:24:22.571 | 既定変更を観測 / 復帰待ち | 28 | 2 |
| 21:24:32.983 | 元の既定IDへの復帰・選択ID維持を観測 | 34 | 4 |

今回の操作について既定入力の一覧追従と固定選択の維持を確認済みとする。
全体通知件数は特定endpointのcallback発生元を証明せず、音声処理・復旧・遅延も検証しない。
Phase 1の残る確認は新変更のCI。以下の既定変更未実施記述は当時の記録で、本結果により更新する。

## 既定入力の検証補助 — 2026-11-08

- Windowsのmultimedia役割の既定入力について、変更→元のIDへの復帰を180秒以内に観測するUIを追加。
- Shiraneの選択ID維持、両区間での全体通知件数増加、正常な列挙を要求する。選択変更・通知不足・取得失敗・時間切れは合格にしない。
- 判定器はUIスレッドが所有し、Windows設定を変更しない。RT経路への変更なし。
- Debug/Releaseの単体テストは各24/24成功。Releaseを再ビルドし、既定入力名と確認UI、保存済みマイク選択の復元を画面で確認。
- 擬似snapshotのテストと実機結果を区別する。既定入力の実機往復はユーザー操作待ちで、現時点では未合格。

## 実機抜差し結果 — 2026-11-08

ユーザー操作による「マイク (USB 2.0 Camera)」の抜差しをRelease版で確認。
同一選択IDを維持し、切断→再接続を観測した。画面の状態復帰表示とログの両方を確認。

| 日本時間 | 状態 | snapshot世代 | 全体通知件数 |
|---|---|---:|---:|
| 14:07:40.733 | 確認開始 / 切断待ち | 94 | 144 |
| 14:07:45.889 | 切断観測 / 再接続待ち | 98 | 147 |
| 14:07:58.795 | 同一IDで状態復帰を観測 | 105 | 182 |

この機器・今回の操作について、抜差し後の一覧追従と選択ID保持を確認済みとする。
通知件数は全デバイス合計であり、個々のcallback発生元の証明や通知遅延測定ではない。
音声clientは未実装のため音声復旧・音切れ・遅延の合格を意味しない。
残る確認: Windows既定デバイス変更、新変更のCI。Phase 1全体の受入完了とはまだ判定しない。
以下の抜差し未実施記述は当時の記録で、今回の機器については本結果で更新する。

## 抜差し検証補助 — 2026-11-08

- DeviceCheckとGUI手順を追加。固定IDを保持した有効→消失/未接続→有効の往復を120秒で観測する。
- 取得失敗・選択変更・通知不足・時間切れ・古いgenerationを成功扱いにしない。
- 全体通知が一覧の状態変化に先行する場合も区間内の証拠として保持する。通知元endpointの同一性まで保証しない。
- 状態遷移をログへ記録。endpoint IDやプロセスパスはこの検証ログへ出さない。
- Debug/Releaseビルド成功、単体各21/21成功。Release最終起動終了成功、検証UIの表示を確認。
- 擬似snapshotの7テストは判定器の検証であり、実機抜差しの合格ではない。
- 実機確認用Releaseウィンドウを開いて操作手順を案内。現時点では確認開始・往復完了の記録なし。
- Phase 1の実機抜差し・既定変更・新しいCI結果は引き続き未受入。

## Phase 1実装・検証 — 2026-11-08

対象: 非RTのDeviceService、MMDevice通知登録、実機endpoint/OSセッション読取り、固定ID選択、schema 1→2移行。
RT経路・音声取得再生は追加していない。

- 最終 `tools/verify.ps1 -Smoke` 成功。VS2022 Debug/Releaseビルド、各14/14単体、20フレーム起動終了が成功。
- 単体追加: 旧schemaの値・元ファイル保持、欠損endpoint ID保存復元、同一出力拒否、不正ID拒否、
  擬似一覧での削除・再接続・順序変更・同名デバイス・改名・方向検査。
- 実機読取専用probe: Debug/Releaseとも終了0。endpoint 64件、有効21件、session 44件、通知登録成功、2世代の一覧更新を確認。
- GUI: Release起動、旧設定読込み、デバイス選択候補、無効/切断済み候補の選択不可表示、OS音量表、通知登録表示を確認。
- GUIで発見したシステム音の間接リソース文字列表示を日本語ラベルに修正し、再ビルドと起動確認。
- 実機probeは通常CIに含めない。任意の `SHIRANE_ENABLE_HARDWARE_TESTS=ON` で `hardware` ラベル、30秒timeout、終了77でskip。

未実施: 物理デバイス抜差し、既定デバイス変更による通知発火・追従、OS Audio Service停止復帰、長時間試験、新変更のGitHub CI。
通知登録成功と実際の抜差し追従成功は区別する。Phase 1受入は未完了、Phase 2へは未移行。
セッションは2秒ごとの時点一覧で短命セッションを見逃し得る。OS音量操作、既定出力の変更、音声client開始は未実装。
初期化失敗後の再試行はアプリ再起動で行う。通知解除失敗時のcallback保持は既知の異常系制限。
性能・音質・レイテンシーの達成を示す検証ではない。実機環境は初回記録と同じPC。

## Phase 0受入完了 — 2026-11-07

GitHub `rumo-dev/Shirane` のコミット `87e9985b3cee92bed50253fabff39532f23ac8e6` に対する
[Windows Phase 0 CI](https://github.com/rumo-dev/Shirane/actions/runs/42371057370) が成功。
Debug/Releaseともconfigure・build・単体10/10成功をログで確認（日本時間21:10頃）。
先行するローカルGUI起動・Docking保存復元の実測結果と合わせ、Phase 0の受入条件を満たした。
Phase 1のデバイス機能へ移行可能。音声機能の実機受入を意味しない。
以下の「CI未実行」「確認待ち」は当時の記録であり、この結果により解消した。

## 追加検証 — 2026-11-07

- `tools/verify.ps1 -Smoke` を追加し、Debug/Release構成・ビルド・単体・各20フレーム起動終了を一括実行して成功。
- 起動テストを `out/smoke/` へ分離し、通常プロファイル・ログを使用しないよう変更。自動試験中のエラーダイアログ待ちも抑止。
- 単体一時データをビルドフォルダー内へ移し、サンドボックス内でも保存・置換試験が通ることを確認。
- 不正フィールド7種類のロード時に既存設定を部分更新しない検証、1 MiB超過ファイル拒否、日本語ディレクトリー往復を追加。
- 最終単体結果: Debug **10/10**、Release **10/10** 成功。両ビルド成功。
- vcpkg展開には通常環境での実行が必要だった。単体テスト自体はサンドボックス内で成功。
- GitHub CIは未実行のまま。Phase 1への移行はCI実行先または暫定受入方針の確認待ち。

以下は初回実装時の検証記録。

実施日: 2026-11-07。対象: UI・設定・ビルド基盤。音声実機の性能試験ではない。

## 環境

- Windows build 26200.9711 / 25H2 / x64
- Intel Core i7-14700
- Visual Studio 2022 / MSBuild 17.14.23 / MSVC 19.44.35219 / v143
- Windows SDK 10.0.26100.0
- CMake 3.31（VS同梱）
- vcpkg baseline `6fdd23fa0a9479a9f9a1c48178696f60cf39255d`、tool 2026-10-29
- Dear ImGui v1.91.9b-docking、Win32、DX11、Meiryo 18px

## 実施結果

| 項目 | 結果 |
|---|---|
| CMake preset構成 | 成功 |
| Debug / Releaseビルド | 成功。自作コードの警告なし |
| Debug単体 | 7 / 7 成功 |
| Release単体 | 7 / 7 成功 |
| Debug / Release GUI起動 | 両方の実ウィンドウを確認 |
| 最終バイナリ smoke test | 両構成で20フレーム描画・終了コード0・正常終了ログを確認 |
| 日本語・記号 | フォント範囲を補正後、表示を確認 |
| 5入力・両バス操作欄 | 初期サイズ1440×940で全操作欄が表示されることを確認 |
| Monitor / Stream設定独立 | マイクMonitorを-15.9 dBに変更、Stream 0.0 dB維持 |
| 明示保存・再読込 | 保存後の-0.3 dB変更から-15.9 dBへ復元 |
| Dockingサイズ変更・永続化 | 出力パネル境界を移動して保存、別構成の再起動後も復元 |
| ログ | 開始・保存成功・正常終了を確認 |

単体の対象: 安全な初期値、空のEngine状態、独立send/日本語/layout round-trip、既存ファイル置換、
壊れたJSON、欠損ファイル、schema 0/2拒否、NaN/Inf/範囲外拒否、ID/pan/容量境界、置換失敗時の既存データ保持。

初回はVS同梱vcpkgと新しいbaselineの非互換で失敗。作業フォルダー内の対応版vcpkgで解消した。
サンドボックス内の一時ディレクトリーではWindowsファイル置換テスト2件が失敗したため、
通常権限の実行環境で再実行し、Debug/Releaseの全件成功を確認した。

## 未実施・既知制限・次の段階

- GitHub CIは定義済みだが未実行。現在の作業フォルダーはGitリポジトリではなくremoteもない。
  TODO: リポジトリに登録後、Windows workflowのDebug/Release単体成功を確認してPhase 0受入を完了する。
- UIのDPI変更時にフォントサイズを動的再構築する処理、GPU device lost自動復旧は未実装。
  描画失敗時はログ・通知後に終了する。
- 非表示タブを含む通常Dockingは有効。複数OSウィンドウへのviewport分離は無効。
- 複数プロセス同時の設定保存、電源断耐性の実機試験は未実施。
- 音声デバイス、driver、電源設定、実周期、FIFO、DSP、測定遅延の値は未測定。
- RT処理が存在しないためRT割当監視・処理時間測定は未実施。Phase 2の受入に含める。
- WASAPI、Application Loopback、MMCSS、SRC、DSP、OBS、1時間/8時間耐久試験は未実装・未実施。
- Phase 1以降への移行はPhase 0の残る受入後。今回、実音声ミキサー完成とは判定しない。
