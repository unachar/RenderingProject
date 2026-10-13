# Phase 4: Hi-Z Occlusion Culling & GPU LOD

Phase 3のGPU Scene、Geometry Arena、ExecuteIndirectを拡張した。Frustum → Occlusion → LOD → Compact → Command Generation → Opaqueの順にGPU上で処理する。通常動作ではVisibility、LOD、Hi-ZのCPU Readbackは行わない。

## 実装

- 既存の通常Z（Near=0、Far=1）に合わせ、R32_FLOATのDouble Buffer Hi-ZをMax Reductionで生成する。各MipをCompute Dispatchし、RenderGraphがSubresource Barrierを管理する。奇数解像度の端のTexelも保持する。
- 専用Opaque DepthをExecuteIndirectで生成する。Texture使用・動的ObjectはOccluderから除外し、Alpha TestやTransparentのCPU描画がHi-Zへ混入しないようにする。
- 前FrameのHi-ZへAABBの8頂点を投影し、1 Pixel広げたRectangleを完全に覆うMip Texelを比較する。Near Plane交差、画面端、不確かな投影はVisibleにする。Depth Biasを設定可能。
- First Frame、Resize、Camera Cut、Scene変更、描画経路変更ではHistoryを無効にする。現実装では通常のCamera移動でもOcclusionを一時停止する。LOD Historyは通常のCamera移動では保持する。
- GPU LODはBounding SphereのScreen Height、Asset Threshold、Hysteresis、Global Biasで選択する。Force LOD、Distance Fallback、欠落LODの細かいMeshへのFallbackに対応する。
- 自動生成LOD0〜4と、最大8段階の外部Mesh関連付けに対応する。Model横の`.lod`へThreshold、Hysteresis、Mesh Pathを保存し、Editorから編集・Save・Reloadできる。
- Visible InstanceはInstance ID、Selected Mesh ID、LOD、状態を保持する。Indirect Commandは選択MeshのIndex Count/Offsetを使用する。
- ImGuiのPhase 4 Visibilityに個別Toggle、Bias、Force LOD、統計、Hi-Z Mip Preview、状態/LOD色表示、Bounds/Screen Rectangle表示を追加した。Debug ReadbackはFrame Fence完了後に取得し、Bounds表示は最大4096件。各Compute Pass、Hi-Z、Opaqueを既存Profilerへ登録する。
- Main Cameraのみが対象。ShadowにCamera Depthを使わない。CPU FallbackとPhase 3経路を維持する。

## 検証方法

```powershell
./scripts/validate-gpuvisibility.ps1
./scripts/validate-gpuvisibility.ps1 -GpuValidation
./scripts/validate-gpuvisibility.ps1 -Configuration Release -Rebuild
./scripts/validate-gpudriven.ps1 -SkipBuild
```

CPUテストはMip範囲、Rectangle Coverage、LOD境界/Hysteresis、欠落LOD、Metadata Round Tripを確認する。GPUシーケンスは1000 ObjectのOcclusion OFF/ON画像一致、壁裏10000 Objectの画像一致とCull、高速Camera移動/Teleport、5段階LODの実際のIndirect Command、欠落LOD、Hysteresis、50012/100012 Instance、Resize、801×451の全MipとCPU Max Reductionの一致を検証する。

実行ログと比較CSVは`build/visibility/`へ保存する。ValidationのReadbackは検証専用であり通常の描画経路には含まれない。

確認済み：CPUテスト、Debug C++/HLSLビルド、Debug通常実行、Debug GPU Based Validation（140 Frame、100012 Instance、D3D12エラー0）、Release全体Rebuildと140 Frameの実行検証。ReleaseではDebug Queueが無効なので、Debug Layerの結果はDebug GBVのログを根拠とする。

Phase 3回帰検証も84 Frame、50000 Command、CPU/GPU GBuffer画像一致、D3D12エラー0で合格した。共有Shader出力のRelease Rebuild中に起動した回帰検証はPostProcess初期化で停止したため、Rebuild完了後に単独で再実行し、この結果を採用した。`git diff --check`も合格。

## 性能比較と制約

Release通常実行の合成都市Scene、50012 Instanceでの測定例：

|条件|Final Visible|Triangles|Opaque GPU ms|GPU Render ms|
|---|---:|---:|---:|---:|
|Frustum|50012|400024|1.262|2.681|
|+ Occlusion|35132|280984|0.884|3.278|
|+ Occlusion + LOD|35132|140504|0.883|3.303|

100012 Instanceでは70772 Visible、29240 Occluded、283064 Trianglesだった。これらは単一Frameの測定例で、平均性能を保証しない。CSVのVS/PS Invocationは追加Depth描画や他Passも含むFrame全体のPipeline統計である。

OcclusionでOpaque時間が減り、LODでGeometry量が減った一方、専用Occluder Depthの追加描画によりこのSceneの総Render時間は増加した。現在の実装は描画欠落の回避を優先しており、移動中CameraではOcclusion効果が得られない。Source Depthの再利用やTemporal Reprojectionは後続の性能改善候補である。

自動検証は生成Meshを使う。実際の大規模Import済み都市Asset、Editorの手動操作、Debug色/Boundsの見た目、FSR/DLSS有効時の最終画質は別途実機確認が必要。Texture使用ObjectをOccluderから広く除外するため、Textureが多いSceneではCull効果が小さくなる。

統計のTotalはGPU Driven対応Opaque Instance数であり、CPU FallbackやTransparentを含むScene全体のEntity数ではない。
