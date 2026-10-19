RenderingProject — 10機能統合拡張設計書

対象: unachar/RenderingProject / dev_main
作成日: 2026-10-19
文書種別: 実装指向・段階導入仕様 / Codex引き渡し用
状態: 提案。未実装部分の有無は実装開始時に再確認すること。

0. 目的と現状

既存の DirectX 12 / C++ / HLSL レンダリングエンジンに、以下の10機能を段階導入する。既存の Virtual Geometry (VG)、GPU Driven Rendering、MegaLights、TSR、ReSTIR GI、Radiance Cache、DXR、NRD/OptiX、RenderGraph、VSM、Reserved Resource Texture Streaming、OIT、RT屈折、エディター、アセット管理を維持する。既存機能を再実装することは本計画の目的ではない。

根拠: source/virtualgeometry/, source/rendering/megalights.h, source/rendering/rayreflections.cpp, source/rendering/rendergraph.h, source/assets/texturemanager.cpp, source/assets/materialasset.h, source/scene/scenefile.h, docs/virtualgeometry-phase11-implementation.md。現状のソース探索による設計案であり、Windows GPU実行や全機能のAcceptance合格を保証しない。

全体設計原則

1. 各機能は FeatureCaps による機能検出、プロジェクト設定、ランタイムON/OFF、診断表示を持つ。
2. DXR / Mesh Shader / Sampler Feedback等の非対応環境では安全に既存経路へフォールバックする。
3. RenderGraphへ明示的な読み書きリソースとQueue/Fence依存を登録する。CPU/GPUの暗黙同期を増やさない。
4. Camera Cut、Resize、動的解像度変更、Scene Load/Unload、Device Lost、Material Hot Reloadで履歴と所有権を正しく扱う。
5. TSRのVelocity/Depth/Reactive情報を共有し、透明物やLOD切替時のゴーストを防止する。
6. リソースの容量上限、時間予算、オーバーフロー、VRAM不足時の縮退動作を定義する。
7. 実装ごとにCPU単体試験、GPU Validation、A/B画質比較、負荷計測を行う。既存機能の回帰を許さない。

1. 機能A: VG専用シャドウ描画

優先度: P0 / 依存: VG、VSM、Shadow Pipeline。

現状と課題: docs/virtualgeometry-phase11-implementation.md はVGのShadow経路にLegacy GeometryまたはRoot Proxyを使い、専用VG Shadow Traversalを持たないと記載している。大規模シーンで影の詳細と負荷が一致しない可能性がある。

設計: Shadow viewごとにCluster階層走査→可視Clusterリスト→Material Bin→Mesh Shader Shadow Rasterを実行する。Directional VSM Clipmap、Spot、Point Cube faceをそれぞれView IDで識別。Shadow側のScreen-Space Errorに相当するTexel ErrorでLOD選択する。影のCaster/Receiver Bias、Alpha Masked、両面マテリアルは既存規則を引き継ぐ。ページ未常駐時は祖先Cluster、失敗時は既存Shadow fallbackへ戻す。Shadow pageキャッシュのDirty判定はTransform、Material、Residency、Light移動に連動。

候補接続: source/virtualgeometry/virtualgeometrytraversal.cpp, virtualgeometrygpu.cpp, source/rendering/lightingresources.cpp, source/rendering/shadowpageregions.h, shader/hlsl/VirtualGeometryTraversalCS.hlsl, VirtualGeometryMS.hlsl。

受け入れ: Legacy参照との影輪郭比較、移動体/LOD遷移/欠落ページ/Clipmap境界、GPU Validation error 0。性能はVG OFF/ONの同一シーンでShadow GPU msとVRAMを測定。改善率は事前に断定しない。

2. 機能B: Sampler Feedback Virtual Texturing

優先度: P1 / 依存: TextureManager、D3D12 Reserved Resources。

現状と課題: texturemanager.cpp はMip単位のResidency/段階的Uploadを持つ。タイル単位の可視需要に応じた選択的な入れ替えを追加する。

設計: Sampler Feedback Tierを検出し、Feedback Mapの解決→非同期Readback→Texture Tile Request→優先度付きIO/Upload→Tile Mapping更新→Fence完了後公開。Page TableとResident Bitsetを保持。最低Mip tailはPinし、LRU + ヒステリシス + カメラ速度に応じたPrefetchを採用。画面面積、Material重要度、TSR出力解像度を優先度に反映。Feedback未対応時は現行Mip Streamingへ戻す。

候補接続: source/assets/texturemanager.cpp/.h, source/rendering/renderersettings.h, Material binding / RenderGraph。Texture Sample側のFeedback書き込みが必要な経路を棚卸しする。

受け入れ: 8K/16K相当の多数TextureでMip欠損・チラツキ・VRAM推移を記録。Tile mappingのFence競合・破棄済みTextureへのReadback適用を防ぐ。容量超過時は低Mip維持。

3. 機能C: PCGグラフ

優先度: P1 / 依存: Procedural、Asset Registry、GPU Scene。

設計: 型付きDAG（Height/Noise/Mask/Slope/Scatter/Transform/Filter/Output）を定義。Cycle禁止、Seed固定、ノード別Cache Key、部分再評価、Undo/Redo、JSON保存。Scatterは密度、最小間隔、法線整列、傾斜、高度、Collision Rejectをサポート。配置結果はGPU Scene Instanceとして登録し、可能なモデルはVGを共有する。編集時プレビューと確定Bakeを分離し、負荷上限を設定する。

候補接続: source/procedural/proceduralcomponents.h, source/editor/editorprocedural.cpp, source/rendering/gpuscene/, source/assets/assetregistry.cpp。

受け入れ: 同一Seed/設定で結果一致、変更したサブグラフだけ再計算、1万/10万InstanceのCPU時間・GPU時間・保存再読込の整合性を測定。

4. 機能D: World Partition / Scene Streaming

優先度: P1 / 依存: SceneFile、ECS、Asset Registry、VG、PCG。

設計: WorldをCell IDと階層LOD (HLOD) に分割。Cell ManifestはBounds、Entity参照、Asset依存、Priority、Versionを保持。カメラ近傍を非同期ロード、遠方を段階解放。ロード完了まではProxy/HLODを表示。Entity IDとGUIDの安定性、跨Cell参照、Streaming中のEditor選択、Undo/Redoを設計。GPUリソースの解放はFence後。PCG生成物をCell単位でBake可能にする。

候補接続: source/scene/scenefile.h, source/core/world.cpp, source/ecs/, source/editor/editorscene.cpp, source/virtualgeometry/virtualgeometrystreaming.cpp。

受け入れ: 高速カメラ移動/テレポート/逆走/連続ロード中断、参照切れ0、可視欠落・二重表示0、メモリ上限とフレームスパイク計測。

5. 機能E: GPU Particle / VFX

優先度: P2 / 依存: RenderGraph、MegaLights、OIT、TSR。

設計: Particle PoolをGPU StructuredBufferで管理。Emit/Simulate/Compact/Sort(optional)/DrawIndirectをComputeで処理。Spawn上限、寿命、重力、風、Drag、Noise、Depth Collision、Mesh Collision(optional)、Ribbonを段階導入。Opaque DepthとのSoft Particle Fade、OIT合成、MegaLights受光、Reactive Mask出力を統合。CPU粒子は小規模互換経路として残す。

候補接続: 新規 source/vfx/, shader/hlsl/Particle*.hlsl, source/rendering/framepipeline.cpp。

受け入れ: 1万/10万/100万粒子でGPU ms、メモリ、Overflow、TSRゴーストを測定。数値は目標負荷でありFPS保証ではない。

6. 機能F: Material / Shader Graph

優先度: P2 / 依存: Material Asset、DXC、PSO Cache、Editor。

設計: 型付きノード（Constant/TextureSample/UV/Math/Noise/NormalBlend/PBR Output）をDAGで接続。中間IR生成→型検査/定数畳み込み/Dead Node除去→HLSL出力→DXC非同期Compile→PSO Cache。Opaque/Masked/Forward/Shadow/Velocity/RT Hitの必要Variantを明示。コンパイル失敗時は直前の正常PSOを保持し、エラーをノードに表示。VGとの互換性は生成Shaderの要件で判定。

候補接続: source/assets/materialasset.h, source/editor/editormaterialassets.cpp, source/rendering/shaderpipelines.cpp, source/rendering/persistentpsocache.h。

受け入れ: 同一Graphから全Passで同一Material表現、Hot Reload中のクラッシュなし、キャッシュ命中時の再コンパイル抑制。

7. 機能G: DBuffer Surface Decals

優先度: P1 / 依存: GBuffer、Material、Deferred Lighting。

注意: 既存のLightRenderMode::Decalはライト側の機能。Surface Decalとは別機能として扱う。

設計: Box Projector + Decal Material。GBuffer Lighting前にDBuffer (Albedo/Normal/Roughness/Mask)へ描画し、既存GBufferへ合成。Depth Reconstruction、角度Fade、Priority/Layer、透明材質への適用範囲、VGと通常Meshの共通受光を定義。GPU DrivenのTile Cullingを将来追加可能にする。

候補接続: source/rendering/scenepasses.cpp, shader/hlsl/DeferredPS.hlsl, source/assets/materialasset.h, Editor Inspector。

受け入れ: 法線/粗さ/影の整合性、重なり順、壁面・床面の投影歪み、GPU時間、TSR安定性を検証。

8. 機能H: Offline Reference Path Tracer

優先度: P1（検証基盤） / 依存: DXR、Material、Lighting、Capture。

設計: 通常Realtime Renderingと独立した静止画Reference Mode。Progressive Accumulation、MIS、Russian Roulette、複数Diffuse/Specular Bounce、環境光、Area Light、Material Emission、透明材質は対応範囲を明示。Seed固定、Sample Count、Max Depth、HDR EXR出力、AOV (Albedo/Normal/Depth/Direct/Indirect) を提供。Realtime GI / MegaLightsとのRMSE・SSIM等を同一カメラ/露光で比較する。

候補接続: source/rendering/raytracingscene.cpp, source/rendering/rayreflections.cpp, source/rendering/rendercapture.cpp。Realtime用Historyとは分離する。

受け入れ: Cornell Box、面光源、金属、ガラス、Emissionで既知Referenceと比較。Sample増加による分散低下、Seed再現性、CPU/GPU停止・中断の安全性を検証。

9. 機能I: FFT Ocean / Water

優先度: P2 / 依存: RT反射・屈折、Atmosphere、Weather。

設計: Ocean Spectrum (Phillips/JONSWAP選択)、IFFTでDisplacement/Normal/Foamを生成。Near/Far LOD、岸辺Depth Fade、Absorption/Scattering、Fresnel、SSR/RT/Probe Reflection Fallbackを統合。Water MaterialはOpaque/Transparentとの描画順を明示。天候のWindと連携。Causticsと浅瀬は第2段階。

候補接続: source/rendering/rayreflections.cpp, source/rendering/transparencyeffects.h, source/rendering/renderquality.h, 新規 source/water/。

受け入れ: カメラ移動/波位相の連続性、海面境界、透明合成、TSR、風変更時の過渡状態、GPU時間を検証。

10. 機能J: Local Reflection Probes

優先度: P1 / 依存: Material、Reflection、Asset Registry。

設計: Sphere/Box Probe Component、Cubemap Capture、GGX Roughness Prefilter、Parallax Box Projection、複数Probe Blend、更新モード(Static/OnDemand/Realtime)。DXR反射が無効/予算不足/粗面でのFallbackに使用。Capture中のProbe自己参照を防止し、Bake結果をAssetとして保存。

候補接続: source/rendering/rayreflections.cpp, source/rendering/materialbindings.cpp, source/assets/assetregistry.cpp, source/editor/editorinspectors.cpp。

受け入れ: Box内の視差、隣接Probe境界、粗さ別応答、DXR切替時の不連続、Captureコストを測定。

11. 推奨実装順と依存関係

|Phase|対象                                           |完了条件                                   |
|-----|---------------------------------------------|---------------------------------------|
|0    |共通基盤・計測                                      |FeatureCaps、予算、ログ、A/B Capture、既存回帰試験を固定|
|1    |A VG Shadow + G Decals                       |Shadow/GBuffer整合性、実機GPU Validation合格   |
|2    |H Reference Path Tracer + J Reflection Probes|Realtimeとの画質比較基盤、Probe Fallback確立      |
|3    |B Virtual Texturing                          |Residency・Feedback・Fence・VRAM予算を検証     |
|4    |C PCG                                        |Deterministic Graph、GPU Instance連携、Bake|
|5    |D World Partition                            |Cell Streaming、HLOD、参照・メモリ安全性          |
|6    |E GPU VFX                                    |Particle/OIT/TSR/Lighting連携            |
|7    |F Shader Graph                               |HLSL生成、PSO Variant、Hot Reload安全性       |
|8    |I FFT Ocean                                  |Weather/Reflection/Transparency統合      |
|9    |統合Acceptance                                 |10機能同時有効化、縮退動作、長時間安定性                  |

※ Phase 1–3は依存が少ない部分を並行開発できる。ただしRenderGraph/Descriptor管理を変更するPRは順次マージ。

12. 共通API・データ契約（提案）

struct FeatureCaps {
    bool MeshShader = false;
    bool Raytracing = false;
    bool SamplerFeedback = false;
    bool ReservedResources = false;
};
struct FeatureBudget {
    uint64_t GpuMemoryBytes = 0;
    uint64_t UploadBytesPerFrame = 0;
    float GpuTimeBudgetMs = 0.0f;
};
enum class FeatureFallbackReason {
    None, UnsupportedHardware, MissingDependency, BudgetExceeded,
    CompilationFailed, InvalidResource, ValidationFailure
};

実装時は既存 RendererSettings / RenderQuality / RenderProfiler / GraphicsDevice の責務と重複しないよう既存APIを優先し、上記は設計上の概念型とする。

13. 検証マトリクス

• 機能: Debug/Releaseビルド、HLSLコンパイル、Editor保存/再読込、Hot Reload、ON/OFF、非対応GPU。
• GPU安全性: D3D12 Debug Layer、GPU Based Validation、Device Removed、Descriptor Lifetime、Queue Fence。
• 画質: HDR比較、Depth/Normal/Velocity/Shadow/AO/Material一致、Camera Cut、動的解像度、TSR履歴。
• 性能: CPU Frame、GPU Frame、各Pass GPU ms、VRAM peak/steady、Upload bandwidth、1% Low、GPU/CPU同期回数。
• ストレス: 1.3M / 100M / 1B三角形相当は段階的に検証。巨大シーンの実ジオメトリ数・可視数・Instance換算数を区別。
• 安定性: Resize、Alt-Tab、連続Scene切替、テレポート、負荷変動、長時間実行、VRAM逼迫。
• 記録: GPU/Driver/Resolution/Render Scale/Build SHA/Feature flags/Scene ID/測定区間をCSVまたはJSONに記録。

14. Codexへの実装指示テンプレート

1. 最初に dev_main の対象ファイルと既存のAPI/テストを再探索し、設計書との乖離を一覧化する。
2. 対象Phaseのスコープ、依存、データ所有権、RenderGraph Read/Write、Shader Variant、Fallbackを短い実装計画に落とす。
3. 1機能を小さなPR単位で実装する。関係のない既存コードを改変しない。
4. 新しいリソースはFrame Fenceを守って解放し、Resize/Camera Cut/Device Lostを考慮する。
5. 単体テスト→Shader Compile→Debug/Release Build→GPU実機Validation→A/B Benchmarkの順に実行し、未実行項目は明示する。
6. 既存テスト失敗・品質退行・性能退行があれば原因を特定し、未解決のまま成功と報告しない。
7. Phaseごとに変更ファイル、検証コマンド、ログ、スクリーンショット、測定値、残課題を記録する。
8. コミットはユーザーが明示的に指示した場合のみ実施する。

15. 完了定義

10機能のソースが存在するだけでは完了としない。各機能がEditorから操作可能で、保存/再読込、機能切替、非対応環境Fallback、GPU安全性、描画品質、性能診断、回帰テストを満たし、同時有効化時にもResource競合・履歴破損がないことを統合Acceptanceで確認して完了とする。

参照ソース

• https://github.com/unachar/RenderingProject/tree/dev_main
• https://github.com/unachar/RenderingProject/blob/dev_main/docs/virtualgeometry-phase11-implementation.md
• https://github.com/unachar/RenderingProject/blob/dev_main/source/assets/texturemanager.cpp
• https://github.com/unachar/RenderingProject/blob/dev_main/source/rendering/renderquality.h
• https://github.com/unachar/RenderingProject/blob/dev_main/source/rendering/rayreflections.cpp
• https://github.com/unachar/RenderingProject/blob/dev_main/source/assets/materialasset.h
• https://github.com/unachar/RenderingProject/blob/dev_main/source/scene/scenefile.h 
https://github.com/unachar/RenderingProject 
フェーズ0から順に最後まで実装してください