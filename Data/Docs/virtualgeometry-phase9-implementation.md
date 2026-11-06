# フェーズ9: Material Binning

2026-10-15。ユーザーの指示により、テスト・ビルド・shader compile・runtime・追加agentを使わず実装した。コード読取り以外の検証は行っておらず、動作・コンパイル成功・性能改善は未確認。

## 実装範囲

既存のnative対応材質（不透明・片面PBR）の可視clusterをMaterial IDで集約する。現在の対応範囲を広げず、masked、両面、透過、Toon/Unlit等は既存の適格性判定と描画経路を維持する。異なるshader/raster classごとのPSO分類やdeferred material resolveは追加していない。

First、Second、Combined/Velocityそれぞれの最終可視集合へ、次のGPU処理を追加した。

1. Binとcounterをreset。
2. Material IDごとのcluster数を集計。
3. prefix sumで範囲を割当て、空でないBinのIndirect commandを連続生成。
4. clusterを対応範囲へscatter。
5. GPUのactive-bin数をCountBufferとして、1回のExecuteIndirectで各BinをMesh Shader描画。

Indirect recordは20 bytes（FirstCluster、ClusterCount、DispatchMesh XYZ）。native graphics rootの末尾へb7用2 DWORDを追加し、既存root indexを維持した。Binned MSだけがこの範囲情報を使い、従来の非Binned MS/Velocity variantも残す。Binごとに専用材質PSOを生成せず、既存の共通PBR Pixel Shaderとtexture heapを使用する。

## 上限と寿命

- 初期exact Material Bin上限4096、設定範囲1〜65535。Material IDが範囲外のものは追加の共通Binへまとめ、元のrecordとMaterial IDを維持する。材質数超過を理由にgeometryを捨てない。
- sorted listは既存の完全な常駐cutと同じ容量（上限400万cluster）。各phaseはその部分集合であり、Bin数によるper-bin固定cluster枠は設けない。
- Bin/range、sorted list、Indirect command、counterをswap-chain frameごとに保持。First→Second→Velocityは同一graphics queueの順でbufferを再利用し、RenderGraphのstate transition/UAV barrierで接続する。
- Pageの常駐snapshot、group単位のLOD置換、native成功フラグ、legacy/traditional排他は変更しない。並べ替えるのは可視recordだけ。
- Binning無効・shader/resource準備失敗時は元の非Binned native dispatchを使う構成。Binned raster variantの読み込み失敗で既存native PSOを無効化しない。

## UI / Profiler

native material binningの有効化と上限、phase別active bins、cluster数、共通Binへ入ったcluster数、不正Material ID数、追加buffer bytesを表示する。統計は各phase32 bytesのreadbackを3 frame保持し、完了Fenceだけpollする。通常描画でGPU待ちやCPUでのBin一覧回収を行わない。

GPU timingには既存Profilerの`Native material bin count/prefix/scatter`イベントを追加した。UI値は過去の完了frameのsampleであり、現frameの同期値ではない。

## 主な変更箇所

- `source/virtualgeometry/virtualgeometrybinning.h/.cpp`: Bin資源、compute pass、Indirect情報、非同期統計。
- `shader/hlsl/VirtualGeometryBinningCS.hlsl`: reset/count/prefix/scatter。
- `VirtualGeometryBinnedMS.hlsl`、`VirtualGeometryBinnedVelocityMS.hlsl`、`VirtualGeometryMS.hlsl`: Bin範囲を使うopaque/velocity variants。
- `source/rendering/meshshader/meshshaderrenderer.cpp`: PSO、command signature、phase別BinningとIndirect描画。
- `virtualgeometrydebug.cpp`、`renderprofiler.h`: 設定と統計。
- project/filter、`Directory.Build.targets`: 新規C++と6つのshader出力を登録。shader依存検査scriptの対象数も追従したが実行していない。

## 未確認事項

C++/HLSLコンパイル、root binding、Indirect record layout、GPU resource state、全phaseの集合一致、複数材質での画面・depth・velocity、上限超過、無効化/再有効化、Page Evict中の挙動、D3D12 debug errors、追加VRAMと速度は未検証。Phase 8も未検証の状態を引き継ぐ。

Bin内scatterはatomicで行うため、同一材質内のcluster順序は固定しない。共通overflow Binは複数材質が混在し、その範囲で材質ごとの連続配置は保証しない。速度向上や完全な画面一致は、測定・比較が許可された段階で確認する。

ユーザー設定・既存変更を保持し、commit/pushや別checkoutの作成は行っていない。
