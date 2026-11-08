# AGENTS.md — Stream Audio Studio 開発規約

製品名は「調音 Shirane」。2026-11-07にPhase 0実装を開始。2026-11-08にPhase 1の非RTデバイス列挙・通知登録・選択保存まで受入済み。次はPhase 2で、音声処理は未実装。実装状況と受入結果は `README.md` と `docs/VALIDATION.md` を参照する。本書の将来フェーズの契約は継続する。

本書はリポジトリルートに配置する。全体設計は `docs/ARCHITECTURE.md`、音声処理の契約は `docs/AUDIO_ENGINE.md` を参照する。配布時は3ファイルを同じフォルダーに置いているため、導入時に後者2つを `docs/` へ移動する。

## 1. 製品と優先順位

Windows 11 x64向け配信者用ミキサー。マイク、ゲーム、Discord、ブラウザー、音楽を個別に取得し、Monitor BusとStream Busを独立制御する。優先順位は、音切れ防止・長時間安定性、低遅延、音質、拡張性、UI装飾の順とする。文書は設計仕様であり、機能が実装済みであることを意味しない。

## 2. 固定技術方針

| 分野 | 採用方針 |
|---|---|
| OS / CPU | Windows 11 / x64 |
| 言語 / IDE | C++20 / Visual Studio 2022 / MSVC v143 |
| ビルド | CMake、CMakePresets.json、vcpkg manifest |
| UI | Win32 + Dear ImGui Docking + DirectX 11 |
| 音声 | WASAPI Shared、IAudioClient3による低遅延対応、Application Loopback |
| データ | 内部48,000 Hz、planar float32、mono/stereo |
| 補助 | nlohmann/json、spdlog、GoogleTest |

依存バージョン、vcpkg baseline、ImGui Dockingのcommit、Windows SDKの検証バージョンを固定しREADMEへ記録する。ImGuiの非Docking版を誤採用しない。Rust併用、ASIO、カーネルドライバーは初期スコープに含めない。Exclusiveは将来の任意出力経路とし、Application Loopbackへ流用しない。

## 3. 作業手順

1. 既存コード、関連文書、作業中の差分を読む。ユーザーの未保存・未コミット変更を保持する。
2. 対象フェーズと受入条件を絞り、変更するモジュールとRT経路への影響を明示する。
3. インターフェース、所有権、スレッド、失敗時動作を決めてから実装する。
4. 最小の垂直断面をビルド・実行し、機能に対応するテストを追加する。
5. ビルド、単体、必要な実機検証を行い、失敗を修正する。
6. 変更、検証条件、結果、未実施項目、既知制限、次の段階を報告する。

依存APIはMicrosoft公式資料で確認する。推測でAPIやフラグを作らない。仕様変更は3文書へ整合して反映する。未実装はTODOと受入条件を付け、モックと実機結果を区別する。破壊的操作や製品方針を変える変更はユーザーに確認する。ビルド・音声実機を使えない場合は未検証と明記し、成功したと報告しない。

## 4. リアルタイム契約

RT範囲はCaptureのパケット転送、Render、ミキシング、DSP、同期用SRC処理とする。

- RT処理中のnew/delete、malloc/free、コンテナ拡張、文字列整形、例外送出を禁止する。
- mutex、condition_variable、future待機、スピン待機、ファイル・ネットワークI/O、同期ログ、GUI操作を禁止する。
- デバイス列挙・再初期化、COMの最終解放、DSPグラフ破棄、プラグインロードは非RT側で行う。
- WASAPIイベントと停止イベントの待機はサービス処理の外側で行う。イベント駆動の待機自体を禁止しない。
- OS音声バッファ取得・解放など必要なAPIだけをRT経路で使用する。WindowsはハードリアルタイムOSではなく、上限時間を保証したと主張しない。
- メモリー、SRC状態、DSP、キュー、ブロックプールを開始前に確保し、ページに触れて初期化する。
- ループ回数、キュー処理数、ブロック数に上限を設ける。過負荷で無制限に追いつこうとしない。
- HRESULTは全て確認し、RT側は数値エラーとカウンターだけを通知する。復旧判断はControl側が行う。

各音声スレッドでMMCSS登録を行い、Audioを基準にPro Audioを比較測定する。終了時は登録を解除する。失敗は診断へ表示する。REALTIME_PRIORITY_CLASS、全コア固定、システム設定変更を低遅延の前提にしない。

## 5. C++20規約と所有権

- RAIIを使い、HANDLE、COM、MMCSS登録を専用ラッパーで管理する。COMはWRL::ComPtr等を使用する。
- Windows境界のHRESULTを内部の型付きStatusへ変換する。C++23のstd::expectedを前提にしない。
- audio coreはWin32、ImGui、DX11へ依存させない。フレーム数、Hz、100ns、QPC ticksを別型または明示的な名前で表す。
- 非所有ビューはstd::span等、所有は明示的なオブジェクトにする。RTのshared_ptr交換に依存しない。参照カウントの最終解放がRTで発生し得るためである。
- 公開RT関数はnoexceptとし、その内部から呼ぶ全処理も例外非送出にする。noexceptを付けるだけでは適合しない。
- audio mutable stateは音声所有スレッドだけが変更する。UIは直接参照しない。
- atomic型のlock-free性を対象x64で確認する。atomicなら何でもRT安全と扱わない。
- SPSCは本当に単一producer/single consumerに限定する。複数入力を1キューへ同時pushしない。
- 命名は型PascalCase、関数camelCase、定数kPascalCaseを標準とし、既存規約があれば統一する。
- MSVC /W4、/permissive-、/utf-8を採用する。自作コードの警告を解消し、外部依存の警告は分離する。
- DSPはNaN/Inf、denormal、異常パラメーターを考慮する。浮動小数点の最適化オプションは音質・再現性を検証して選ぶ。

## 6. 通信・設定・安全な変更

UI→Controlの通常キューは非RTなのでロック可。Control→Engineは固定容量SPSC、Engine→DiagnosticsもproducerごとのSPSCとする。音声入力は入力ごとに独立したSPSCを持つ。メーターは原子的な値または所有権付きスナップショットで公開する。通常の構造体を同時読み書きする二重バッファは使用しない。

構成変更は非RTで準備した不変Graphをブロック境界で切り替える。固定スロットとgeneration/ACKを用い、旧Graphは利用終了ACK後に非RTで解放する。ACK未受領時はスロットを再利用しない。パラメーター更新はまとめ、音声側で1境界あたり最大64コマンドまで適用する。キュー満杯時はControlで再送・統合し、重要な構成変更を黙って失わない。

緊急ミュートはキュー混雑に影響されないlock-free atomic latchとし、Engineが各処理境界で確認する。キュー、SRC、ディレイを含む復帰処理はミュート後に非RTで調整する。

JSONはschemaVersionを持ち、検証・移行・一時ファイル書込み後の置換で保存する。デバイスは表示名でなくendpoint ID、アプリはPIDだけでなく実行パス等の再識別条件を保存する。OBS認証情報をログや設定エクスポートへ出さない。

## 7. 標準ビルド・確認

次のpreset名をPhase 0で定義する。configure presetはVisual Studio 17 2022、x64、vcpkg toolchainを選び、build/test presetは対応するDebug/Release構成を指定する。

```powershell
cmake --preset windows-vs2022-x64
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure
cmake --build --preset windows-release
ctest --preset windows-release --output-on-failure
```

これらはリポジトリ実装時の契約であり、この文書配布にCMakeプロジェクトは含まれない。vcpkgの場所は環境変数等から指定し、個人の絶対パスをコミットしない。

## 8. テストと完了定義

| 対象 | 必須の確認 |
|---|---|
| Buffer / Queue | wrap、満杯・空、長時間並行転送、順序、所有権、容量境界 |
| DSP / Mixer | 無音、正弦、インパルス、gain/pan、mute/solo、NaN対策、クリック抑制 |
| Routing | Monitor変更がStreamに波及しない、ループ・重複禁止、緊急ミュート |
| Clock / SRC | ±100/500 ppmの擬似クロック、時刻ジャンプ、欠落、44.1→48 kHz |
| WASAPI | 可変パケット、SILENT、周期拒否、非対応形式、切断、再接続 |
| Application | ゲーム/Discordの子プロセス、終了・再起動、無音、二重取得防止 |
| 耐久 | 通常負荷1時間と8時間連続、UI操作・GPU負荷・録音を重畳 |
| 設定 | 壊れたJSON、旧schema、失われたデバイス、保存・復元 |

単体はGoogleTestと擬似デバイスで自動化する。実機テストは別ラベルとし、ハードウェアのないCIで成功扱いしない。Debug/Releaseビルド、関連テスト、RT割当監視、処理時間測定、文書更新が対象機能の完了条件。性能報告にはOS build、CPU、デバイス・ドライバー、電源設定、実周期、滞留量、DSP構成、測定方法を添付する。

## 9. 段階的実装

`ARCHITECTURE.md` のPhase 0〜8を順に進める。最初の実装はWin32/ImGui Docking/DX11画面、CMake、依存固定、ログ、設定、空のaudio core、GoogleTestまで。デバイス制御、音声処理、OBS連携を一度に実装しない。各フェーズの受入条件を満たしてから次へ進む。
