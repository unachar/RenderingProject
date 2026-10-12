# GPU Driven Rendering Phase 3 設計案

## 目的と範囲

ユーザー提供のPhase 3仕様を受け入れ条件とする。Opaque Static Mesh / Indexed Geometry / Standard PBRをGPU SceneからGPU上で選別・Command生成し、Pass単位のExecuteIndirectで描画する。既存Phase 2、CPU fallback、Shadow、Velocity、特殊描画を維持する。Hi-Z、LOD、Mesh Shader、Transparent、Skinnedへの拡張は含めない。

## 調査で確認した現状

- `source/rendering/gpuscene/`にInstance・Mesh・Materialの常駐buffer、差分更新、frame fenceに基づくretirementがある。
- `GpuSceneCulling`はInstance slotごとのVisibility flagを出力する。CompactなVisible ID listとGPU countは追加が必要。
- Mesh recordは個別VB/IBのGPU addressを持つ。既存`GpuDrivenIndirectDrawCache`はObject・Texture groupごとの処理で、Scene全体のCPU submission削減を満たさない。
- `FramePipeline`はGPU Scene更新の後にOpaqueを実行する。GPU Scene内部のRenderGraphが実resource stateを管理する。
- Materialは2624-byte raw bufferで、既存PBR constantsを保持する。既存Shader Modelは5.1。Texture indexが実際のheap offsetに対応するかは実装前にDescriptor管理で確認する。
- 作業ツリーにはPhase 2とVirtualGeometryの未コミット変更がある。これらを維持する。

## 採用する接続

### Geometry

既存Static Mesh資産を登録時にOpaque用のGlobal VB/IBへ追加する。元のGeometryを保持し、CPU fallback・Shadowなどは引き続き使用する。Scene Mesh recordに共有Geometryのoffsetと対象可否を付加し、C++/HLSL layout assertionsを更新する。Mesh sectionごとに既存Instanceを利用し、1 section = 1 commandとする。Geometry変更時だけ更新し、capacityは必要時に幾何級数で拡張、旧resourceは既存frame fenceで退役させる。

個別VB/IB viewをIndirect commandに含める案は移行を小さくできるが、commandサイズを増やし、仕様の共有Geometry方式から離れるため採用しない。

### GPU処理

既存Frustum flagを維持し、対象InstanceだけをGPUでVisible ID listへ圧縮する。GPU counterをGPU reset passで初期化する。GenerateIndirectCommands CSは最大capacityでdispatchし、GPU visible countによるbounds check後にMesh recordからcommandを生成する。

Commandは`uint instanceID`と`D3D12_DRAW_INDEXED_ARGUMENTS`の24 bytes。Root constant 1 DWORD + DRAW_INDEXEDのCommand Signatureを作成し、C++のsize/offsetとHLSLの書込みoffsetを検証する。CountはGPU上のcompact visible countを再利用できる条件で再利用する。対象判定は圧縮時に完了し、無効commandを残さない。

### OpaqueとShader

Opaque用root signatureにInstance ID、Scene Instance/Mesh/Material SRV、frame共通定数、既存Texture heapの配列を接続する。SM5.1で利用可能なDescriptor table配列参照を優先し、ResourceDescriptorHeap構文のためだけにEngine全体をSM6.6へ移行しない。

VSはSceneのWorldを取得し、PSへnointerpolation Instance IDを渡す。PSはScene raw Material bufferから既存PBRパラメータとTexture indexを読み、既存Shading処理とGBuffer出力を共有する。PreviousWorldを保持し、既存Velocity経路との履歴一致を検証する。

Pass開始時にheap・PSO・Global VB/IB・共通定数を設定し、ExecuteIndirectを1回、必要な既存PSO区分がある場合は少数回実行する。GPU対象のObjectはCPU opaque loopから除外し、対象外のみ既存経路で描く。GPU pathの初期化失敗は明示的にCPU fallbackへ戻す。

### RenderGraphとLifetime

`GPUSceneUpdate → GPUCulling → CompactVisibleInstances → GenerateIndirectCommands → OpaqueGPUDriven`の実resource dependencyを宣言する。Visibility/Visible IDのUAV→SRV、Command/CountのUAV→INDIRECT_ARGUMENT、必要なUAV同期をRenderGraphで管理する。Opaque実行時のgraphにも実buffer使用を接続し、手動barrierを描画箇所へ分散しない。

Command・Count・Visible IDは既存frame resource設計に合わせて管理し、frame slot再利用は既存fence待機後に限る。Command capacityはInstance capacity以上。毎frameのresource作成を避け、拡張だけで確保する。

## Debugと検証

CPU/GPU toggle、対象・fallback数、capacity、memory、CPU submission時間、GPU Culling/Command Generation/ExecuteIndirect/Opaque時間を既存ImGuiとProfilerへ追加する。GPU countやcommand詳細の表示は明示的なdebug readbackに限定し、通常描画はreadbackに依存させない。最大command数を実行command数として表示しない。

既存validation fixtureを拡張し、100/1000/10000件でCPU/GPU GBuffer一致、可視数とcommand数の一致、CPU API呼出し数、submission時間を測る。高速camera移動、1000生成→500削除→500生成、空Scene、capacity成長も検証する。50000件は既存ECS上限16384のため、上限・関連bufferの安全性を確認したうえで対応可能な環境で実行する。

Debug/Release build、shader compile、CPU layout/capacity tests、D3D12 Debug Layerと可能ならGPU Based Validationを実行する。実測していない描画一致、性能改善、validation成功は完了扱いにしない。

## 実装順

1. Descriptor・PBR・Static Geometry登録点を追加調査し、対象条件と共通Shader接続を確定する。
2. Global Geometry登録、Scene layout、24-byte commandとsignatureを追加する。
3. GPU counter reset、Visible ID圧縮、Command生成、frame bufferとRenderGraphを接続する。
4. Opaque ShaderとExecuteIndirect、CPU対象除外、fallback toggleを接続する。
5. Debug UI・Profiler・stress fixtureを追加し、描画と性能を比較する。

この文書は調査に基づく設計案であり、Phase 3の実装・検証完了を意味しない。
