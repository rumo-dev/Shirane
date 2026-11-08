# AUDIO_ENGINE.md — Stream Audio Studio 音声エンジン仕様

## 実装状況 — 調音 Shirane / 2026-11-07

Phase 0で48,000 Hzの定数とUnavailable状態を定義。現在も音声スレッド・デバイス制御は未実装だが、Phase 2のRT向けcore/Mixer関数は単体試験から使用できる。
Phase 1では非RTのMMDevice列挙・変更通知登録、OSセッション音量の読取りを追加した。音声clientの開始・パケット取得・OS音量変更は行わない。
デバイス選択は固定endpoint IDの希望設定で、切断時に別endpointへ自動変更しない。RT経路への追加はない。
UIの抜差し検証はendpoint一覧と全体通知件数のみを観測する。音声client・バッファ・再生復旧の検証には代用しない。
既定入力の変更・復帰の検証もUIスレッド上の一覧判定のみで、音声ルーティングの変更やRT処理は行わない。全体通知件数は特定endpointのcallbackの証明ではない。
画面のsend・master値は希望設定であり、DSPや音声信号に適用されない。
Phase 2のpool/SPSC・Mixerに擬似信号・C++割当監視・時間測定を導入済み。TODO: Clock/Meter・汎用DSP APIとPhase 2全体の受入、Phase 3以降の実機受入。
現時点で低遅延、音質、安定性の実機性能を達成済みとしない。

Phase 2の初期部品としてSpscQueue（release/acquire公開、満杯/空でfalse、待機なし）とAudioBufferPool（固定planar float32、mono/stereo、世代付きhandle）を実装した。容量はテンプレートで指定し、起動前に構築・全sample領域へ書込みする。ページ常駐は保証しない。貸出は範囲内sampleをゼロ初期化し、最大2×MaxFrames回に制限する。返却後のspan利用は禁止。世代上限のslotは再利用せず退役させる。プール不足・無効サイズはfalseとし出力handleを保持、無効handleのviewは空、二重返却はfalse。sampleを共有するクロススレッド所有権、packet adapter、high-water再同期は未実装。

設計版1.0 / 2026-11-07。全体設計は `ARCHITECTURE.md`、実装規約はルートの `AGENTS.md` を参照。本書の容量・閾値は初期値であり、実機測定後に変更記録を残して調整する。

## 1. フォーマットと時間単位

| 項目 | 初期仕様 |
|---|---|
| 内部sample rate | 48,000 Hz |
| 内部sample | planar IEEE float32 |
| channel | mono入力、stereo入力/Bus、明示的channel map |
| 内部DSP quantum Q | 128 frames候補、64/256も比較 |
| endpoint形式 | WASAPIで交渉した実形式。内部形式と別管理 |
| endpoint周期 P | 問合せた有効値。128 frames固定としない |
| 通常音声範囲 | ±1.0が0 dBFS、内部加算はfloatのheadroomを保持 |

frameは全channelの同一時刻のsample集合。stereo 128 framesは256 samplesであり、128 samplesではない。48kHzでは64/128/256 framesが約1.333/2.667/5.333ms。これは1ブロックの時間で、入出力の実測遅延ではない。

デバイスが44.1kHzやPCM16/24/32なら境界で変換する。WAVEFORMATEXTENSIBLEのvalid bitsとchannel maskを確認し、unsupported layoutは明示的なmatrixなしに処理しない。mono mic→stereoはL/Rへ同値複製、stereo panはequal-powerとし、中央の減衰規則をUIとテストに固定する。サンプル形式だけをfloatと読み替えない。

### 実装済みMixerの数値・所有権契約（Phase 2）

- 内部48kHz、0〜256frames、最大16source。monoはL/Rに複製してからpanを適用。stereoはクロスフィードなしのbalanceで、`L=cos((pan+1)π/4)`、`R=sin((pan+1)π/4)`、中央左右各−3.0103dB、端の反対側は厳密にゼロ。GUIのpan tooltipにも明記。
- gain/masterは−60〜+6dB、panは−1〜+1。非有限値・範囲外はprepareが拒否し、以前の準備済み係数を保持。soloはBus単位、mute優先、muteされたsoloも他sourceを抑制する。
- 係数変更は240sampleの線形振幅ランプ。同じtargetの再送では再開せず、途中変更は現在値から240sample。ブロックを分割しても結果は一致する。source不在時もランプ時刻は進む。
- 緊急ミュートはprocess境界でlatchを読み、48sampleで両Busをゼロへ減衰。applyでは解除不可、停止後resetのみ解除。出力endpoint滞留を含む実際のミュート時間は未検証。
- 入力NaN/Infはそのsourceの当該block全体を無音化しsource数を返す。subnormal入出力はゼロ。double加算でheadroomを維持し、float範囲超過は該当出力sampleをゼロにして計数（limiterではない）。
- 入力source重複、slot範囲外、channel数/長さ異常、自己出力との重なりはInvalidInputと無音出力。256超のframe数は空出力。固定入力slot方式なので任意Graphの循環検証は対象外。外部spanの実メモリー寿命・非競合は呼出し側の契約。
- Mixerは出力配列を自身で保有し、process中に確保・解放・待機・I/Oしない。下位GainRampは検証済み有限非負係数専用の内部部品。一般DSPのprepare/process/reset/latencyインターフェースへの統合は後続。

## 2. WASAPI Shared初期化

物理micとMonitor/Stream render endpointは以下の手順を標準とする。

1. 非RTでMMDeviceからendpointを解決し、COM apartmentを初期化する。
2. IAudioClientをactivateし、実際のmix formatと対応形式を検証する。
3. IAudioClient3を取得可能なら、採用形式でGetSharedModeEnginePeriodを呼ぶ。
4. min/max範囲内かつfundamentalの整数倍となる周期候補を作る。128に近い値を初期候補とし、最短値を無条件採用しない。
5. InitializeSharedAudioStreamでEVENTCALLBACKを指定する。SetEventHandle、GetBufferSize、必要service取得、事前確保を行う。
6. renderを無音でprimeし、event/stop待機を準備してStartする。captureもevent駆動で開始する。
7. 実形式、default/fundamental/min/max、採用P、buffer size、stream latencyを診断へ記録する。

IAudioClient3初期化ではperiodからendpoint bufferが決まるので、内部Qや要求periodを実buffer capacityと同一視しない。有効周期の条件は[InitializeSharedAudioStream公式仕様](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream)に従う。

非対応、形式拒否、周期競合等ではHRESULT別に再試行する。失敗したclientを無理に再利用せず必要に応じて新規activateする。現engine形式/周期を取得して追従するか、従来IAudioClient::InitializeのShared EVENTCALLBACKへフォールバックする。従来SharedではhnsPeriodicityを0とし、採用buffer durationはAPI仕様に従う。低遅延経路に入れなかったことをUIへ表示する。独占モードへ自動変更しない。

RAW modeは既定で要求しない。ドライバー、音声効果、形式による違いを評価し、必要な場合だけ明示的に選ぶ。[Windows低遅延音声の概要](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio)を参照する。

## 3. Application Loopback

`ActivateAudioInterfaceAsync`、`VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`、`AUDIOCLIENT_ACTIVATION_PARAMS`、`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`を使用する。対象PIDとINCLUDE_TARGET_PROCESS_TREEを指定する。除外treeモードは高度な別sourceとしてのみ提供する。

Microsoftの[Application Loopbackサンプル](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)はbuild 20348以降を要件とする。製品対象はWindows 11に限定するが、OS build、必要API、activation HRESULTを実行時に確認する。非対応時は理由を表示し、全デスクトップ取得へ黙って切り替えない。

process loopbackは物理endpoint captureとは別adapterとし、IAudioClient3のperiod設定を当然に使えると仮定しない。公式サンプルのShared loopback初期化方式を基準に、形式とEVENTCALLBACK動作を実機検証する。物理endpoint非依存の取得であり、endpoint clockを持つ前提のコードを共用しない。

activation完了handlerのCOM要件を満たし、必要ならagileな実装を使う。初期化情報・handler・async operationは完了まで生存させる。停止後の完了はgenerationで破棄し、COM最終解放は非RTで行う。

ゲームとDiscordは別source、別SPSC、別同期状態を持つ。Discordの複数プロセス構造、ゲームlauncherと実ゲームのtree、再起動、PID再利用を検証する。音声をrenderしていない対象は無音となり得る。無音を直ちに故障と扱わない。Application Loopbackが元音声を消すと仮定しない。

## 4. パケット転送とQuantum Adapter

Captureはevent後、GetNextPacketSizeで利用可能packetを確認し、GetBuffer→固定メモリーへコピー/変換→ReleaseBufferを同じworkerで行う。OSのbuffer pointerをqueueへ保存しない。停止を適宜確認し、パケット処理の予算を超えたら次のサービスへ回し、遅延とoverflowを検出する。

- SILENT: データpointerを読まず、指定frame数をzeroとして扱う。
- DATA_DISCONTINUITY: 欠落イベントと世代を通知する。同期を再評価し、クリックを抑える。
- TIMESTAMP_ERROR: timestampを同期推定から除外する。到着時刻は低信頼情報としてのみ扱う。
- 0 frames / BUFFER_EMPTY: 通常の「現在データなし」として処理する。

Captureのframe positionとQPC timestampはpacket先頭に対応する。QPCPositionは100ns単位で、QueryPerformanceCounterの生tickではない。[GetBuffer公式仕様](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer)を参照。

可変packetを固定slotへ分割し、元timestampにframe offsetを加えて保存する。内部Quantum Adapterは不足frameを保持し、QごとにDSPへ渡す。Renderが求めるframe数がQの倍数でないときは、完成blockの残りを次回へ保持する。要求数に合わせてbufferを伸縮しない。

RenderはGetCurrentPaddingと実buffer sizeから書込み可能frame数を計算する。必要量だけGetBufferして、準備済みscratch/output FIFOから充填し、ReleaseBufferする。無音全体なら適切なSILENT flagを使用する。OSが要求する大きさとQを混同せず、catch-up回数は事前計算した上限を持つ。

## 5. バッファ容量・滞留量・所有権

初期上限は入力16source、stereo。設定上限を超える追加はControlで拒否する。pool sizeはsource数、最大packet、SRC履歴、graph二重保持、sink数から算出する。

| バッファ | 初期容量例 | 通常の目標滞留 | 満杯 / 不足 |
|---|---|---|---|
| 入力SPSC | sourceごと8192 native frames以上、最大packetを考慮 | mic: max(Q相当, 通常packet 1個)、app: 安定packet 1〜2個 | producerは新規packetをdropしてdiscontinuity通知 / consumerは無音 |
| Stream SPSC | 48kHz stereo 8192 frames | sink period相当以上、測定で調整 | producerは新規blockをdrop / sink不足は無音 |
| Control command | 256 fixed commands | ほぼ0 | Controlで統合・再送、構成変更はACK管理 |
| RT診断 | producerごと1024 events | ほぼ0 | drop数だけ加算、音声を待たせない |
| 録音 | 別途秒単位で確保 | storage性能による | 欠落範囲を記録、Monitorを停止しない |

8192 framesは48kHz換算約170.7msの最大保管容量であり、通常170.7ms待つ設計ではない。native rateごとに時間を計算する。目標滞留はpacket cadence、起床jitter、P、SRC履歴から決め、GUIへmsとframesで表示する。

新規dropだけで古い音声が残り続けないよう、consumerがhigh-waterを超えたら自分のread indexだけを進め、target付近の新しいデータへ再同期する。producerがconsumerのread indexを上書きしてはならない。再同期はカウントし、短いfadeを使う。packet metadataとsampleは同一slotの所有権で渡し、別キューの順序ずれを避ける。

SPSC indexの公開にはrelease/acquireを使い、cache line競合を避ける。poolの貸出・返却も所有スレッドを固定する。複数consumerへ同一mutable blockを渡さず、Busごとの固定bufferへ書く。

## 6. マスタークロックと同期

通常はMonitor render deviceをマスターとする。IAudioClockのpositionはfrequencyと合わせて時間へ変換し、frame数だと決めつけない。QPCとの対応点、render padding、送出済みframe数を使って内部48kHz timelineへ写像する。アプリ入力に利用可能なdevice clockがなくても、timestampとFIFO情報で同期できる構成にする。

入力packetごとにsource frame positionとtimestampからsource時刻を推定する。timestampが欠損・不連続なら推定器をresetし、直前値から異常な巨大SRC補正を出さない。QPCの生tickはQueryPerformanceFrequencyで秒へ、WASAPIの100nsは10,000,000で秒へ変換する。計算途中の単位を型や変数名に残す。

独立デバイスは公称48kHzでも周波数が異なる。固定比SRCだけでは長時間のFIFO増減を防げないため、各sourceと独立sinkへ状態付きadaptive SRCを用意する。

初期制御案:

```text
occupancyError = (queuedFrames - targetFrames) / targetFrames
ratio = inputFramesConsumed / outputFramesProduced
nominalRatio = inputRate / outputRate
ratio = nominalRatio * (1 + driftEstimate + boundedPI(occupancyError))
```

入力FIFOが多いときratioを上げ、output 1 frameあたり多くの入力を消費する。driftEstimateは入力clock/マスターclockから推定した公称比との差。FIFO PIとは役割を分け、過補正しない。PIは数十ms単位で更新し、観測の平滑化、anti-windup、補正rate制限を持たせる。

補正幅は初期±500ppm、変更slewは10ppm/更新以下を候補とし、擬似clockと実機で調整する。これは対応保証値ではない。補正限界を継続して超えたら無理に追従せずdiscontinuityとして再primeする。大きな欠落やtimestamp jumpに通常のPIで追いつこうとしない。

SRCはanti-alias filterと状態・phase continuityを持つ実装を選び、filter delayを報告する。線形補間だけを最終音質仕様にしない。blockごとにSRC状態をresetしない。

Stream sinkが別device clockならStream worker側のSRCで合わせる。MonitorとStreamのFIFOを1つにしない。MonitorがないHeadless時はQPC基準の48kHz frame timelineを用い、累積deadlineでevent/timer駆動する。連続sleepで周期誤差を累積させない。Windows timerによるjitterはFIFOで吸収し、実測する。

clock master切替えでは旧driver停止→ACK→clock epoch更新→SRC/対応時刻再初期化→新driver開始をControlが直列化する。二重DSP実行を禁止する。

## 7. ミックスとDSP

処理順序は、format conversion→timestamp/clock adapter→SRC→channel trim/common DSP→独立send gain/pan/mute/solo→Bus加算→Bus DSP→meter→sink adapter。

DSPの概念APIは以下とする。spanは借用メモリーで、process中の保持や再確保をしない。

```cpp
struct IAudioProcessor {
    virtual Status prepare(const ProcessSpec&) = 0; // 非RT
    virtual void process(AudioBlockView block) noexcept = 0;
    virtual void reset() noexcept = 0; // 有界、停止中または境界
    virtual std::uint32_t latencyFrames() const noexcept = 0;
    virtual ~IAudioProcessor() = default; // 破棄は非RT
};
```

ProcessSpecはrate、maxBlockFrames、channel layoutを含む。大きな状態のresetは非RTの新instanceへ交換する。prepareは最大入力に対応するメモリーを用意し、実行中に処理サイズへ追従して確保しない。

gain変更は初期5ms線形amp rampを候補とし、muteも短くrampする。緊急ミュートは最大1ms候補の短いrampで即応し、実際の発効は現在のservice境界とendpoint滞留に依存する。0ms保証はしない。NaN/InfをBusへ伝播させない。入力異常時は該当sourceを無音化しカウントする。

初期DSPはgain、meter、EQ、gate、compressor、limiter、mic sidechain ducking。gate/compのattack/releaseはrate依存で計算する。sidechainの参照時刻とBusを明示する。

Monitorはlookahead、FFT、ノイズ抑制等の高遅延処理を既定で迂回する。Stream専用branchにそれらを置き、各processorのlatencyを加算する。Stream内で必要な他channelへ固定delayを追加する。plugin latency変更は非RTでgraphを再構築する。

Stream limiterのlookaheadは0〜1msから評価し、追加遅延を表示する。sample peak制限をtrue peak保証と呼ばない。Monitorへの強いclipping防止とStream limiterは別設定とする。

## 8. イベント、MMCSS、処理予算

各音声workerは音声eventとstop eventを待つ。timeoutは故障監視用であり定常pollingにしない。制御・UI・ログの応答を音声workerが待たない。

各workerでAvSetMmThreadCharacteristicsWによるAudio登録を基準とし、Pro Audioを測定比較する。必要ならAvSetMmThreadPriorityを適用し、終了時はAvRevertMmThreadCharacteristicsを呼ぶ。登録失敗を表示するが、busy loopやプロセス全体のrealtime優先度で代替しない。[MMCSS仕様](https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service)を参照。

Q=128/48kHzのブロック時間は2.667msなので、DSP 25%目標は約0.667ms。これはDSPだけの計算予算。capture変換、SRC、Bus両方、copy、WASAPI呼出しを含む全serviceを別計測する。1回のrender eventで複数Q処理する場合は合計時間をdeadlineと比較する。

event起床遅れ、service時間p50/p95/p99/max、deadline超過を記録する。QPC読み取りと固定サイズevent記録以外の集計・percentile計算は非RTで行う。

## 9. 障害と安定モード

| 障害 | RT側 | 非RT側 |
|---|---|---|
| 入力不足 | 無音補完、短いfade、source別counter | targetとpacket cadenceを調査 |
| FIFO過多 | consumerで古いframeを破棄、再同期 | source timestamp/SRC/負荷を調査 |
| endpoint無効 | 処理を抜け数値error通知 | 再列挙、backoff付き再activate |
| Audio Service停止 | 安全停止 | service復帰後に新generationで再開 |
| app終了 | sourceを無音化 | 再binding候補を表示 |
| sink停止 | Stream欠落を記録、Monitor継続 | sink再接続、backlog破棄、再prime |
| DSP過負荷 | deadline counter、既定の安全処理 | 高負荷DSP bypassや設定変更を提案 |

安全モードはQ=256、より長い有効endpoint period、適度なtarget増加を別々の設定として扱う。Qだけ大きくしてdevice event期限が改善するとは限らない。実行中の再確保をせず、事前準備した構成へ切替えるか安全停止して再構築する。頻繁な自動往復を防ぐhysteresisを持ち、適用値と理由を表示する。

欠落0という表示は「検出可能なAPI flag、FIFO欠落、deadline超過が0」を意味する。ドライバー内部で起きた全glitchを観測できると主張しない。

## 10. レイテンシーの定義と測定

遅延予算は概ねcapture packet/OS滞留＋入力FIFO滞留＋SRC filter＋quantum adapter＋DSP＋sink FIFO＋render/driver/hardware。実周期とqueue滞留の測定点が重なる場合は二重計上しない。GetStreamLatencyは一部の報告値で、実測end-to-end全体ではない。

UIでは以下を分ける。

- 理論block時間: Q / 48,000。
- 内部推定: 保有frame数と既知のDSP/SRC遅延から計算し、範囲と測定点を表示。
- 実測: 信号経路と機材を添えた測定値。未測定なら未測定と表示。

mic Monitor目標10ms以下は、入力基準点→SAS→ヘッドホン出力の測定として定義する。分岐したテスト信号をmic/line入力と基準録音へ入れ、SAS出力と基準を同一clockの測定装置で収録し、相互相関で差を求める。出力→入力のケーブルloopbackだけならround-trip測定であり、単純に2で割ってone-wayとしない。実際のmicやpreampを通さない測定にはその制約を付記する。

アプリ追加遅延は同一テストアプリの直接経路とSAS経由経路を同時収録して差を比較する。source側bufferingとSAS追加分を分ける。実ゲーム/Discordでは受信codec、network、相手環境をSAS遅延へ混ぜない。

各条件で複数回測定しmedian/p95/maxと欠落数を記録する。音響測定では距離、反射、機器遅延も記録する。OBSは録画ファイルで二重音声、lip sync、Stream timestamp連続性を確認する。

## 11. テスト計画

### 自動テスト

- Buffer: 1/63/64/127/128/129/480/任意packetを組み合わせ、全frameの順序と欠落flagを照合する。
- SPSC: wrap、満杯、空、単一producer/consumer並行、長時間sequence検証。overflow時も競合しないことを確認する。
- Quantum: endpoint PとQが非整数比でも、toneのphaseとframe総数が連続することを確認する。
- DSP: impulse、sine、無音、step gain、mono/stereo、NaN、異常parameter、latencyFramesとの整合。
- Routing: Monitor/Stream独立、Bus別solo、mute、重複source、cycle拒否、latency補償。
- Clock: ±100/500ppm、44.1/48kHz、packet jitter、timestamp error、clock epoch切替え、欠落回復を擬似時刻で再現する。
- Drift: 8時間相当の擬似入力でFIFOが単調増減せず、定常滞留が設定範囲内に保たれることを確認する。
- Graph: generation/ACK前に旧slotを再利用・破棄しない。command満杯時も緊急muteが適用される。
- RT監視: 開始後にallocation/freeが発生したら検出するテストフックを使用する。ホットパスの静的レビューも行う。

### Windows実機マトリクス

内蔵、USB mic、USB interface、44.1kHz endpoint、異なるUSB clock、通常Shared fallbackを含める。Bluetoothはcodec等による遅延が大きいため別クラスで結果を記録し、有線10ms目標の達成例に混ぜない。

ゲーム＋Discord＋micの同時取得、GUIドラッグ、DSP切替、GPU負荷、録音、device抜差し、既定変更、Audio Service停止復帰、sleep/resume、process再起動、OBS再接続を実施する。通常負荷1時間はunderrun/overrunゼロを目標とし、8時間ではFIFOの持続的増減、メモリー増加、timestamp drift、音切れを確認する。

## 12. 診断レポートと合否

レポート必須項目はOS build、CPU、driver、endpoint IDの匿名化値、native/内部format、P/Q、buffer capacity/target/実滞留、MMCSS、全DSPと遅延、負荷条件、試験時間、欠落/overflow/discontinuity、処理時間percentile、clock補正ppm、測定器と測定点。

受入は機能ごとに判定する。目標未達をバッファ数だけで隠さず、低遅延設定と安定設定の両方の結果を残す。未対応機能、未実施テスト、外部仮想ケーブル等の前提を明記する。この文書の作成だけで実機性能を検証したとは扱わない。
