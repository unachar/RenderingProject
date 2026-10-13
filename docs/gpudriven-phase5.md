# Phase 5 — Meshlet / Mesh Shader / Cluster Rendering

## 実装

Static OpaqueのGPU Driven経路へMesh Shaderを追加した。GPU Scene、Object Cull、Hi-Z、Discrete LOD、既存PBR/GBuffer、Geometry ArenaとDXR用Index Dataを共有する。

`GPU選択LOD → Cluster判定 → Meshlet候補 → Frustum/Cone/Hi-Z → Visible List → GPU Dispatch Argument → ExecuteIndirect Mesh Shader` をGPU上で処理する。通常描画では可視数・Dispatch情報をCPUへ戻さない。

| 項目 | 実装 |
|---|---|
| 機能検出 | OPTIONS7 Mesh Shader TierとShader Model 6.5。非対応・初期化失敗はGPU TraditionalへFallback |
| Asset | SubMesh/LOD0〜4のMeshlet、uint32 remap、8bit local triangle×3、Sphere、Cone、Cluster。バイナリv2、Source/Config keyとPayload checksum |
| 生成 | DevelopmentのModel Import時に生成・保存。Releaseでは保存済みAssetを読み、欠落・破損はTraditionalへFallback |
| 設定 | 初期64頂点/126三角形、32 Meshlets/Cluster。小さい上限へ変更可能。Shader出力上限を超える設定は拒否 |
| Global Data | Meshlet/Remap/Primitive/Bounds/Cluster/LOD Rangeを容量方式で管理 |
| Instance | Instance IDとMeshlet IDを組にし、同一Assetの複数Instanceを共有 |
| Bounds | World Sphereの半径をFrobenius normで安全側へ拡張。近接・不確かな投影はOcclusionしない |
| Cone | Double-sided、Mirror、非一様Scale/Shearなど不確かな変換は無効化。Cluster Coneは無効値を保存 |
| Cluster | 2階層。1Objectを64スレッドで処理。Cullされた子はBounds判定せず候補へ理由を記録 |
| Capacity | Object全Meshletの予約に失敗したらObject全体をTraditionalへ戻し、部分欠落を防止 |
| 描画 | MS+既存PBR PSのDXIL版。OpaqueとVelocity。両面Fallbackと単面の裏面除外を共通化 |
| Velocity | GPU SceneのCurrent/Previous WorldとPrevious camera VP。既存RGBA16F形式を維持 |
| Frame/State | 3 Frame Resource、既存Fence寿命管理。RenderGraphでSRV/UAV/Indirectと同期を宣言 |
| Debug | Path、Tier、Cull Toggle、Bias、Capacity、色、Sphere/ConeのPreview、Cull理由、Geometry/Memory統計 |
| Profiler | Cluster Culling、Meshlet Culling、Mesh Dispatch Generation、Mesh Shader Opaqueを既存GPU Profilerへ追加 |

GPU Traditional用の元Indirect CommandsはHi-Zに残し、Opaque用に別のFallback Commandsを作る。Mesh Shaderへ渡したObjectのFallback index countだけを0にする。

## 有効化

Deferred RenderingでCompute GBufferを無効にし、ImGuiの `Rendering Geometry Path` を `GPU Driven Mesh Shader` へ変更する。起動時は `DX12_PHASE5_MESH_SHADER=1` でも選択できる。対応状態・失敗理由はMesh Shader Debug UIへ表示する。

DebugではModel Import後に `asset/meshlet-cache/` へ生成する。Release配布にはこの生成済みデータを含める。通常のFrame処理ではMeshlet Buildを呼ばない。GPU使用中のDynamic GeometryはMeshlet Rangeを無効化しTraditionalで描く。

Debug ReadbackをONにするとGPU完了を待つ。性能比較ではOFFにする。Bounds/Cone Previewは最初の4096候補を表示する。

## 検証手順

```powershell
./scripts/validate-meshlets.ps1 -CpuOnly
./scripts/validate-meshlets.ps1
./scripts/validate-meshlets.ps1 -SkipBuild -ForceFallback
./scripts/validate-gpuvisibility.ps1 -SkipBuild
./scripts/validate-meshlets.ps1 -SkipBuild -GpuValidation
./scripts/validate-meshlets.ps1 -Configuration Release -Rebuild
```

ビルドとGPU実行は順に行う。Shader出力と実行ログを共有するため同時実行しない。実行結果は `build/meshlets/` に保存する。既存PCHを異なるデバッグ情報設定で作成済みの場合は `-Rebuild` で揃える。

自動SceneはCube/Sphere、1,048,352 TriangleのMesh、1000共有Instance、5 Material Sections×10000 Entities、内部Camera、高速移動、Negative Scale、Double-sided、LOD0〜4、容量不足、Asset欠落、描画中に追加するMeshlet無しAsset、部分遮蔽、非一様Scaleを含む。Traditionalと5 GBuffer/DepthおよびRaw UV Velocityを比較する。Camera/Transform変更直後でHistory条件が異なる組はGeometryのみ比較し、移動Velocityは別途投影期待値で検証する。

## 検証記録

最終実行の記録は [実装・検証台帳](gpudriven-phase5-progress.md) に集約する。

## 計測の範囲

GPU timestampの `Mesh Shader Opaque` はMesh Shader、Rasterizer、Pixel ShaderとTraditional Fallbackを含むPass時間である。Shader Stage単独の時間として扱わない。Pixel Shader単独の時間分離はPIXなどの専用Profilerで別途確認する。

DLSS/FSRを有効にした画質比較、Shadow Mesh Shader、Skinned/Transparent等のMesh Shader化、Cluster Cone最適化は今回の自動検証に含めない。通常のDXR/Shadow Geometryは従来経路を維持する。自動試験はDXR/DLSSを無効にしてGeometry経路を比較する。
