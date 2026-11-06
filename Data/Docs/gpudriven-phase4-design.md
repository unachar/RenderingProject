# GPU Driven Rendering Phase 4 設計

ユーザー提供のPhase 4仕様を受け入れ条件とする。Phase 3のGPU Scene、共有Geometry、24-byte Indirect Command、ExecuteIndirect、CPU fallbackを維持し、Main CameraのOpaque Static MeshへHi-Z OcclusionとGPU LODを追加する。既存の未コミット変更は保持する。

## 確認した既存構造

- Main CameraのDepthは通常Z。Depth clearは1、Opaque PSO比較はLESS。Hi-ZはR32_FLOAT、Max reductionとする。
- `occlusionculling.cpp`には二重バッファ、Mip別Dispatch、従来の2-phase経路がある。Phase 3のScene全体のGPU Driven RendererはこのOcclusion判定を利用していない。
- `GpuScene::Execute`のRenderGraphがScene更新、Frustum判定、Compact、Command生成を実行する。Opaqueは別Graphで実resourceをimportしている。
- Visible Bufferは現在Instance IDのみ。Command生成はInstanceのMeshIndexから共有Geometryのoffsetを取得する。
- StaticModelはLOD0〜LOD2のIndex Bufferを生成するが、CPU側の保持データはLOD0のみ。LOD有効Entityは現在GPU Driven対象外。
- Frame別Command Bufferと、既存GpuSceneBufferのfenceに基づくretirementがある。新Resourceも同じ寿命管理を利用する。

## 採用方針と代替案

既存Hi-Z管理を補強してPhase 3へ接続する。Occlusion、LOD、Compact、Command生成を初期実装では分離し、GPU profilerで各段階を確認できるようにする。GPU Driven Pathを作り直さない。

別のHi-Z subsystemを並設する案はDepth lifetimeとdescriptor管理が重複するため採用しない。Frustum／Occlusion／LODの単一Dispatch化は初期検証後の最適化候補とする。Async ComputeとSingle Dispatch Multi-Mipは本Phaseに追加しない。

## 描画順序とHi-Z履歴

Frame Nでは、Frame N-1のOpaque Depthから生成したPrevious Hi-Zを読み、`GPUSceneUpdate → Frustum → Occlusion → LOD → CompactVisibleLOD → GenerateIndirectCommands → OpaqueGPUDriven`を実行する。Opaque Depthが完成した後にCurrent Hi-Zを生成し、次Frameへ引き継ぐ。Hi-Z生成がFrame末尾になる点は、仕様のPrevious Frame利用に合わせた具体化である。

Hi-Z sourceはFSR等のupscale前のScene内部解像度のOpaque Depth。TransparentとAlpha TestはOccluderに含めない。混在Depthから安全に分離できない経路ではOcclusionを無効化し、必要ならOpaque専用Depth passを追加する。ShadowのvisibilityへCamera Hi-Zを適用しない。

従来2-phase経路とPhase 4の履歴生成を同じFrameで重複実行しない。既存CPU／特殊描画の経路を保ちながら、各モードでHi-Zの生成箇所・利用者・swap箇所を一意にする。

二重バッファのtextureとdescriptorをGPU使用中に上書きしない。既存queueの順序とframe fenceでread/writeの順序を保証し、resizeはGPU idleまたはretirement後に行う。複数queueへ拡張する場合に備えてresource dependencyを明示する。

## Pyramid生成とResource State

Depth Resource／SRVの実形式を既存Depth生成箇所で確認し、Depth SRVからComputeでR32_FLOAT Mip0へコピーする。続いて1MipずつMax reductionする。異なるformat間のCopyTextureRegion互換性に依存しない。

Mip countはScene解像度から算出する。D3D12のfloor縮小に合わせ、奇数幅・高さの端のtexelも次Mipへ含めるReduction footprintを使用する。単純な2×2のみで末尾を捨てない。texture accessは各Mipの実寸でclampする。

RenderGraphへDepth SRV、Hi-Z各subresourceのSRV／UAV依存を宣言する。Mip間のUAV→SRV transition、Visibility／LOD／VisibleのUAV→SRV、Command／CountのUAV→INDIRECT_ARGUMENTをGraphで処理する。外部所有resourceの最終stateを呼び出し側へ返し、二重のstate管理を避ける。

初回、resize、Scene reset、Camera切り替え、teleport、FOV／position／rotationの大きな変化ではPrevious Hi-Zを無効化する。Camera Cut flagと明示的な履歴reset APIを用意する。Cameraのview／projectionが変化したFrameでは、初期実装はOcclusionを保守的に無効化する。これにより微小移動を含むdisocclusionで古いDepthが現在の可視性を保証しない問題を回避する。履歴生成は継続し、安定したCameraで再開する。

## Conservative Occlusion

GPU SceneのWorld AABBをPrevious View Projectionへ投影し、8頂点からScreen Rectangleとnearest depthを求める。Near plane交差、非有限値、無効なbounds、画面外へはみ出す不確かな投影はVisibleへ倒す。

Rectangleをpixel単位で外側へ拡張する。Mipの選択はRectangleを覆うtexel数を基準にし、選択Mipで最大2×2のtexelに収まるまで上げる。全被覆texelをpoint Loadし、そのMax depthとObject nearest depthを比較する。Corner／Centerのみを粗くsampleして内部の穴を見落とす設計にしない。通常Zでは`nearestDepth > maximumOccluderDepth + bias`の場合のみOccludedとする。

仕様の「大Objectは低Mip、小Objectは高Mip」という例より、全領域を保守的に覆うことを優先する。通常のPyramidでは大Rectangleほど粗いMipを必要とする。選択MipとRectangleはDebugで確認可能にする。

Dynamic／transform変更ObjectはOcclusionを無効化する。Occluderの移動・削除・geometry／material変更時は履歴を無効化する。Instance slot再利用はvisibility／LOD historyを初期化する。Visible猶予は補助的な安定化として利用できるが、Camera／Scene変更時の無効化の代わりにはしない。

## LODデータとGeometry

GPU LOD Groupはoffset／count、Levelはselected Mesh ID／screen-size threshold／hysteresisを保持する。InstanceへInvalidLODGroupを既定とするGroup IDを関連付ける。C++／HLSLのsize・offsetをassertし、既存serialized dataの読み込み互換性を保つ。

Import時に生成した低LODのCPU index列を保持し、Phase 3の共有IBへ登録する。頂点を共有できる既存簡略化LODは同じBaseVertexを利用する。独立MeshのLODもGPU Mesh IDで関連付けられる構造とする。Groupは最大8Levelを扱い、既存3Level資産もそのまま利用する。Stress用にはLOD0〜LOD4を用意する。

LOD thresholdとhysteresisは既存Asset／Scene metadataの保存経路へ追加する。EditorでCount／Mesh／Threshold／Hysteresisを編集できるようにする。旧距離設定は互換用に残し、screen-size評価ができない場合のdistance fallbackへ利用する。

LOD Meshが未登録、空、またはGPU非対応なら、要求Levelから詳細側へ探して有効なMeshへfallbackする。LOD0も無効ならEntityをCPU経路へ戻す。異なるmaterial／section構成のLODは無理に同一Commandへ束ねず、対応できるsection mappingを検証し、不一致はCPU fallbackとする。

## GPU LOD SelectionとIndirect

Occlusion Visible Instanceに対し、Current CameraのView-space Bounds sphereから画面高さ占有率を計算する。Near plane交差／近距離はLOD0。Global LOD Biasは画面占有率を2のべき乗で補正し、正のbiasで低詳細側へ寄せる。Force LODはAuto／0〜7を扱い、欠落Levelのfallbackを適用する。

GPU historyに前回Levelを保持し、詳細→粗と粗→詳細で異なるthresholdを用いる。Camera Cut、Group変更、slot再利用でhistoryをresetする。GPU LOD OFFは元Meshを利用する。

Compact結果は`{instanceID, selectedMeshID, selectedLOD, padding}`の16-byte recordとする。GenerateIndirectCommandsはselectedMeshIDを参照し、従来24-byte Command契約を維持する。CPUでInstanceごとのLOD判定やVisibility結果の通常readbackを行わない。

## Debug・統計・Profiling

Frustum／Occlusion／GPU LODを個別toggleできる。Hi-Z有効性、Mip count／選択Mip、Camera Cut理由、Depth Bias、LOD Bias、Force LODを表示する。Hi-Z grayscale preview、visibility色分け、LOD色分け、bounds／screen rectangle表示を追加する。Cull済みObjectのDebug表示は別Debug描画経路とし、最終Visible Listだけでは欠落しないようにする。

GPU counterはTotal／Frustum Culled／Frustum Visible／Occlusion Tested／Occlusion Culled／Final Visible／LOD0〜7／selected vertices・trianglesを記録する。UI／validationを有効にした場合のみ遅延readbackし、通常経路では結果をCPUへ戻さない。GPU時間はBuild Hi-Z／Frustum／Occlusion／LOD／Compact／Command Generation／Opaqueを分ける。Pixel Shader CostはtimestampによるOpaque時間のほか、利用可能なpipeline statistics等を使い、未測定値を実測として報告しない。

## 検証と完了判定

CPU側の純粋計算では奇数解像度のReduction coverage、screen projection／near crossing、LOD threshold／hysteresis／missing fallback、layout／capacity／slot resetを検証する。C++とHLSLをDebug／Releaseでbuildする。

GPU validationは既存`validate-gpudriven.ps1`とvalidation hookを拡張し、A: 1000 ObjectのOFF／ON画像一致、B: 壁背後10000 Object、C: 都市50000以上、D: 高速rotation、E: teleport、F: LOD0〜4往復移動、G: 境界微小移動、H: 可能なら100000 Instanceを実行する。画像比較は同じLOD設定でOcclusionだけを切り替え、LODの画像差と混同しない。

Frustum Only／Frustum+Occlusion／Frustum+Occlusion+LODの3条件を同一CameraとSceneで比較し、Frame／各Pass時間、geometry count、visible count、測定できたPixel Shader指標を記録する。Shader／Resource State／descriptor／bounds違反がDirectX12 Debug LayerとGPU Based Validationに増えないことを確認する。

実行できないSceneや検証は未実行と記録する。仕様の必須条件が残る間はPhase 4完了としない。Meshlet／Mesh Shader／Streaming／Shadow LOD等は本Phaseへ含めない。
