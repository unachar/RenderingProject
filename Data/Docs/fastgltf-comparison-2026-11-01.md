# fastgltfモデル読込の実装・比較

> 中断時点の最新実装・共有CRTへの変更・再計測結果は[引継ぎ記録](library-work-handoff-2026-11-01.md)を参照。以下のCRT構成と一部の数値は最適化前の記録。

## 今回の判断

利用可能なSDKがある場合、glTF/GLB/VRMはfastgltf backendを標準の先行経路とし、未対応機能はAssimpへ戻す。`DX12_USE_FASTGLTF=1`はfastgltf優先、`0`は既存のみ。SDK未導入時は既存方式を使う。初期の既存優先方針は、有効な省略bind形式でも既存Assimpがクラッシュする追加証拠とDraco比較を受けて変更した。[Draco比較記録](draco-comparison-2026-11-01.md)に判断と試験を記録する。

JSON解析だけでなく、ファイル読込、属性展開、所有権のあるaiSceneへの変換、既存の座標変換・法線生成・頂点結合・cache locality処理、解放までを測った。最適化Release、各方式20回×交互順5passの1入力あたり中央値:

|入力|Assimp|fastgltf＋既存後処理|
|---|---:|---:|
|Box.glb（External内の実サンプル）|0.670515ms|0.538570ms|
|CesiumMan.glb（External内の実スキン・アニメーション資産）|4.701400ms|5.419040ms|
|材質・埋込画像の生成fixture|0.661180ms|0.543300ms|
|skin・morph・線形animationの生成fixture|0.770150ms|0.610750ms|

Draco対応を含む最終の専用DLL構成で再測定した。小入力ではfastgltfが速い一方、実アニメーション資産では約15%遅い。変換中に一時シーンとAssimp側コピーを保持するため、メモリ削減とも扱わない。新経路を先行させる判断は、既存で誤読・クラッシュする有効な入力を正しく扱うことを優先したもの。すべての入力で速度が優位とは扱わず、変換中のコピー等の最適化を残す。4入力でのwarm反復結果であり、cold I/O、一般の大規模シーン、FPS、GPU品質の優劣は未証明。生の5pass結果は`bin/validation/fastgltf/report-with-draco.json`。以前の測定は`report.json`と`report-dll.json`に残すが、上表の根拠には使わない。

## 実装と出力確認

`source/models/fastgltfimport.cpp`で実parserとaccessor decoderを使用する。階層・TRS/行列、三角形、インデックスなし入力、法線、接線、UV、正規化頂点色、PBR基本材質、埋込画像・外部画像参照、skin/bind行列/重み、静的morph、線形TRS animationをaiSceneへ変換する。異なるskinやnode側morph weightのインスタンスを分離し、同一条件のメッシュは共有する。

Assimpのcustom importerとして登録し、既存の後処理をそのまま実行する。アプリ側の一時配列をDLLで解放させず、`SceneCombiner::CopyScene`でDLL側へ深いコピーを作る。返したシーンはImporterと一緒に所有し、`ReleaseModelScene`から正しい所有者を解放する。FBX/PMX/VMD等の既存経路は維持する。

adapterとRelease parser SDKは専用の`FastGltfAdapter.dll`へまとめ、Release Assimp DLLと同じSTL構成をDLL内で使用する。公開境界にはSTLオブジェクトを渡さず、生のシーンポインターと固定バッファだけを使う。製品Debugの翻訳単位は通常のDebug STLを使い続ける。SDKとadapterはDLL内のRelease `/MT`でメモリを確保・解放する。

最初のstatic統合では、異なるiterator設定を同じ実行ファイルへ混在させていた。CPU試験の呼出側も同じ設定にしていたため問題を検出できず、製品の起動試験でアクセス違反が出た。専用DLLへの分離に変更し、不一致の強制許可設定を撤去した。現在のDebug試験は通常Debug STLの呼出側からDLLを呼び、実際の境界を確認する。

Box、インデックスなし三角形、生成材質/skin/morph/animation、CesiumManについて、無変換・StaticModel実際のflags・AnimationModel実際のflagsで比較する。頂点/法線/接線/色/UV/面、階層と行列、画像全バイト、基本色・texture role/path、bone/bind/weights、morph、animation target/keyを照合した。チャンネルは対象ノード名で対応させ、値は1e-5、時刻は0.0001msの許容差を用いる。シーン名や任意metadata全件の一致を保証する試験ではない。

疎なpositionのfixtureでは、この同梱Assimpはstrided base＋sparse値の組合せから不正な頂点値を返した。fastgltf側は既知のdense相当データと照合して確認する。日本語名とpercent escapeを含む外部buffer URIも、既存読込が失敗し新backendで回復した。これらを既存出力への一致のために壊さない。

外部bufferは読込前に宣言サイズを制限し、必要なバイトだけを読む。buffer/view/accessor/sparseの範囲とindex順、頂点index、有限値、階層cycle/共有、変換予算を確認する。破損・過大accessor・範囲外sparse・cycle・未対応extensionは失敗として戻す。

## ビルドと残る範囲

`scripts/build-fastgltf-dx12.ps1`に続けて`scripts/build-fastgltf-adapter.ps1`で構築する。製品のDebug/Releaseとも、このRelease DLLを使用する。同梱upstreamのCMakeは1.0.0、manifestラベルは0.9.0/mainで一致していないため、ラベルだけをSDK同一性の根拠にしない。依存はsimdjson 5.0.4、simdutf 9.2.1の固定hash付き。SDK、依存のライセンスを製品へ配置する。既存Assimpヘッダー2箇所の壊れたinclude終端も、custom importerをコンパイルするため修正した。

Release SDK、adapter DLL、DLLを使う製品Debug全体ビルドは成功。Release/通常Debugの呼出側とSDK無効構成も試験合格。通常PDB生成の環境問題を避ける`_CL_=/Z7`はビルドプロセスのみへ指定した。制限環境の最初の製品ビルドは待機したため、親子関係を確認して終了させ、デスクトップ環境のビルドで成功を確認した。

Box/CesiumManを含む隔離シーンをfastgltf優先で起動し、3フレーム描画と正常終了を確認した（`bin/validation/startup/fastgltf-dll-models-desktop-Debug-1.log`、runner exit0）。生成シーンと元設定を分け、終了後にユーザーのシーン設定を復元した。このsmokeは画素比較やアニメーション全時間の視覚検証ではない。

DLL分離後の既存Assimp経路も同じ隔離シーンで3フレーム正常終了した（`bin/validation/startup/fastgltf-dll-native-desktop-Debug-1.log`、runner exit0）。両起動の時間はcache状態が異なるため、性能比較には使用しない。

主な証拠: `bin/fastgltf-sdk-release-final-build.log`、`bin/fastgltf-adapter-dll-final-build.log`、`bin/fastgltf-product-dll-build.log`、`bin/fastgltf-import-dll-{Release,Debug,disabled}-final.log`、`bin/fastgltf-import-dll-benchmark.log`。既存animation preview/lifetime/pose-instance/scene settingsの回帰試験も合格（`bin/fastgltf-animation-regression.log`）。

STEP/CUBICSPLINE、morph weight animation、非三角形topology、camera/light、instancing、material/texture extensionsは未実装であり、明示的fallbackの対象。Dracoのmesh inputは追加実装済み。残る機能の実装と比較、実画面でのquality検証、全体レビューは引き続き必要。fastgltf全仕様やExternal全ライブラリの完了報告には使わない。
