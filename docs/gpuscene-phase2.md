# GPU Scene Phase 2

ECSを正規データとし、変更通知を受けたEntityだけをRendering用のGPUキャッシュへ反映する。
既存Opaque・Forward・Deferred・Shadow・Velocity・DXR・アップスケーラの描画経路は維持する。
VirtualGeometry Phase 1の変更も維持し、このPhaseで新しいLOD選択・Occlusion・Meshlet・Mesh Shader・Streamingは追加しない。

## データと登録

| データ | GPU形式 | stride | 内容 |
| --- | --- | ---: | --- |
| Instance | StructuredBuffer | 224 bytes | current/previous world、Mesh/Material ID、Entity ID、active/culling flags、world bounds、local mesh bounds、既存LOD設定 |
| Mesh | StructuredBuffer | 64 bytes | 既存VB/IBのGPU VA（low/high）、頂点/インデックス数、開始位置、local bounds |
| Material | ByteAddressBuffer | 2624 bytes | texture descriptor indices、alpha、PBR、shader class/mode、既存15部位のshader constants |
| Visibility | RWStructuredBuffer | 4 bytes | GPU Instance IDごとのFrustum結果。削除slotは0 |

C++のsize/offset assertionsと`shader/hlsl/GpuSceneCommon.hlsli`でレイアウトを合わせる。
MatrixはDirectXMathと同じrow-major。Materialは既存`BuildPBRConstantsFromMaterial`を再利用する。
Shader Model 5.1はStructuredBufferの1要素を2048 bytesまでに制限するため、Materialの全パラメータを残したraw SRVを使用する。

Static/Animated Modelはmesh sectionごとにInstanceを持ち、Primitive Meshと3D Spriteは1つのInstanceを持つ。
2D画面Sprite、Camera、LightだけのEntityは3D culling cacheに登録しない。
`Entity::GetGPUInstanceID()`は最初のIDを返し、全sectionは`GpuScene::GetEntityInstances(entity)`で取得する。
初回のScene同期前・描画geometryなし・削除後は`InvalidGPUInstanceID`。
CPU snapshotを返す参照/pointerは次の同期までに使用する。

Mesh/Materialは内容hashで共有し、hash衝突時はbytesも比較する。
Instanceは共有レコードへのIDだけを持つ。頂点・インデックスを複製しない。
共有レコードはimmutable cacheとしてEngine終了まで保持する。Material編集は変更Entityだけを別の共有レコードへ付け替える。
Mesh buffer内のboundsはgeometry共通、EntityのAABB overrideはInstanceのworld boundsに適用する。

## 更新と履歴

通知元はRegistryのAdd/Remove/Destroy/Reset、TransformSystemのDirty処理、Material setter/texture解決、
PrimitiveGeometryの更新、Timelineのcomponent track、Editorの変更確定/Undo/Redo。
描画時のUnchecked accessorは通知を発生させない。

既存の参照による編集APIを維持するため、checked accessorのrendering component取得は保守的な通知を行う。
同じ内容ならInstanceのbytes比較で転送を抑止する。
新しいRendering/EditorコードでUnchecked accessorや保持済み参照を編集する場合は、ECSを編集後、
`GpuSceneChanges::Notify(entity, flags)`で変更を通知する。これはGPUデータの直接編集ではない。
TransformのPosition/Rotation/Scale変更は従来どおり`IsDirty = true`を設定すれば自動反映される。
通知・同期は既存ECSと同じmain threadで行う。

移動したフレームのprevious worldは直前のキャッシュworld。
次のフレーム開始時に、その移動Instanceだけprevious=currentに揃える。
そのフレームも移動した場合は再度直前のworldをpreviousにする。
最初の静止フレームには履歴を揃える224 bytesの更新が1回必要で、それ以降は転送しない。
初回登録・ID再利用時はprevious=current。削除slotのactive flagを消し、Free Listへ戻す。

## ResourceとRenderGraph

DEFAULT heapのInstance/Mesh/Material bufferは常駐する。
初期capacityは1024で、不足時だけ2倍ずつ拡張する。
既存`GraphicsDevice`の3フレームfence待機後に、そのframeのUpload resourceを再利用する。
新しいfenceや独立したqueueは作らない。
旧resourceは拡張が起きたframeのretirement listに保持し、そのframe slotを再利用できる時点で解放する。
同じdirect queue上で以前のread→次のcopy→次のreadの順に実行するため、ID再利用もGPU使用中のデータを書き潰さない。

`GPU Scene` passをShadow/Opaque/Deferredの依存に追加。
各frameの実resourceをimportする内側のgraphで、更新のあるbufferだけをCOPY_DESTにして差分copyし、
NON_PIXEL_SHADER_RESOURCEへ遷移した後にScene Frustum computeを実行する。
変更なしではCopy passを作らない。拡張時のみ現在のレコード全体を転送する。
連続slotの差分copyはまとめる。SceneのbarrierはRenderGraphが管理する。
成長前のresource pointerをcompiled graphへ固定しない。

GPU Sceneの同期はChanged Entityと履歴settle対象だけを処理する。
GPU Frustum passはresident slotを直接走査し、CPU candidate配列を作らない。
既存modelのGPU indirect cullingもInstance bufferのworld/local mesh boundsを参照する。
その他の既存CPU Draw/Velocity/DXR列挙を全面的に置き換えるPhaseではないため、その処理は維持している。

## DebugとProfiler

描画デバッガーの`GPU Scene` treeで、slot/active/capacity、共有Mesh/Material数、Dirty数、
処理Entity数、総転送量、Instance転送量、buffer memory、CPU更新時間、ID対応表を確認できる。
memoryはresident、frame Upload、retirement resourceの合計。既存VB/IBの資産全体のmemoryは含めない。
RenderProfilerは`GPU Scene`、`GPU Scene Update`、`GPU Scene Culling`のCPU/GPU時間を測る。
Copyがないframeには`GPU Scene Update` eventを発行しない。

## 10,000件に必要な既存接続点の修正

- ECS上限を1024から16384に拡大。既存Instancing upload上限は32768にし、ECS上限の16倍への不要な膨張を避けた。
- Velocity/Outlineの一時CB slotsをEntity上限に対応させた。以前の2048 slotsでは10,000件でnull descriptorを渡していた。
- 既存PBR constantsは2048 bytesを超えていたため、frame slot strideを2560 bytesへ変更し、隣のMaterial slotへの上書きを防止した。
- DXRのTLAS拡張/BLAS削除は、同じcommand listの先行描画が参照した旧resourceをframe fenceまで保持する。
  10,000件への拡張時にDebug Layerが検出した旧TLASの早期解放を修正した。

## 検証

再現コマンド（Visual Studio C++ toolsが必要）:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate-gpuscene.ps1 -Configuration Debug
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate-gpuscene.ps1 -Configuration Release
```

既に同じsourceをbuild済みなら`-SkipBuild`を付ける。
Debug/Releaseのshader出力は共有されるため、buildと描画検証は順番に実行する。
検証は`DX12_VALIDATION_MODE`を利用し、通常のEditor設定を保存しない。
ストレスfixtureは`DX12_GPU_SCENE_STRESS_TEST=1`の場合だけ作成する。

CPU testは10,000 slots、Dirty重複排除、静止時0更新、previous settle、削除/再登録、
ECS通知の合成、同じEntity IDの削除/再生成、Scene resetを検証する。
実機testは1→1000→10000 Entityの登録・buffer拡張、全Instance/MaterialのGPU bytes読み戻し、
10,000件すべてのGPU Frustum結果とCPU参照との比較、共有VB参照、1件移動、ID再利用、
Material変更、Mesh交換、回転/負スケール/AABB overrideを検証する。
Fixtureは共有geometryを使い、既存描画経路でも実行する。

画像比較は同じprojectionの固定Sceneでresident/legacy cullingを切り替え、
OpaqueのBase Color/Normal/Material GBufferをpixel bytes単位で比較する。
比較時だけMesh ShaderとHi-Zを外し、既存Opaque indirect cullingの接続を直接検証する。
Temporal DLSS/NRDの履歴が異なる最終画像を完全一致の証拠として扱わない。
その他のDXR、DLSS、HDR、resize、empty scene、deformation、既存Mesh Shader/fallbackは84-frame smokeを使用する。

## 最終検証結果（2026-10-12）

Debug Build、Release Rebuildとも成功。Releaseは既存PCH/PDBのlink不整合を避けるため、
`/Z7`を指定して全体をRebuildした。両構成でGPU Scene CPU tests、既存simulation 2801 checks、
procedural editor 976 checksが成功した。

| 構成 | 通常機能 | fallback | GPU Scene stress | GBuffer比較 |
|---|---|---|---|---|
| Debug | 84 frames PASS | 84 frames PASS | 両経路PASS | 両経路で完全一致 |
| Release | 84 frames PASS | 84 frames PASS | 両経路PASS | 両経路で完全一致 |

Debug Layerが有効なDebug検証では、通常機能/fallbackともD3D12 errors=0。
10,000 fixture Instanceと既存3 Instanceの合計10,003件、capacity 16,384で以下を確認した。

| 状態 | Dirty Instance | Instance upload | 全upload |
|---|---:|---:|---:|
| 静止（frame 65/68） | 0 | 0 bytes | 0 bytes |
| 1 Instance移動（frame 66） | 1 | 224 bytes | 224 bytes |
| previous matrixのsettle（frame 67） | 1 | 224 bytes | 224 bytes |

Release通常機能の単発測定では、frame 68のCPU更新処理は0.031 ms、1件移動は0.041 ms。
既存ProfilerのGPU Scene全体は0.0215–0.0219 ms、Cullingは0.0180–0.0194 msだった。
これらはこの実行環境の測定値であり、導入前後の統計的なperformance比較ではない。

全GPU Instance/Material bytes、全10,000件のFrustum結果、ID再利用、共有Mesh/Material、
Mesh交換、負スケール/回転/Bounds override、Opaque indirect culling接続を確認した。
描画一致の直接比較は固定SceneのBase Color/Normal/Material GBufferが対象。
任意のSceneや全Shadow/DXR出力のpixel一致まで保証するものではなく、その他の経路はsmokeで検証した。

最終ログは`build/gpuscene/debug-acceptance-console.log`、`release-acceptance-console.log`と
`build/gpuscene/{Debug,Release}-stress-{features,fallback}/init_log.txt`に保存した。
