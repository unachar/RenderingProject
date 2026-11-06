# Virtual Geometry Phase 1 — 実装・検証レポート

`unachar/RenderingProject` の `dev_main`、調査時HEAD `22696db3cfde776f3135c2a8029333f79a73f90c` にローカル変更として追加。
ローカルのHEADと `origin/dev_main` の参照は一致していた。mainは基準にしていない。コミット・pushは実施していない。
2026-10-12、Windows / MSVC 19.51 / x64で検証した。

## 追加・変更ファイル

追加したCPUパイプライン:

- `source/virtualgeometry/virtualgeometryconfig.h`: 上限・設定と設定検証API。
- `virtualgeometrytypes.h`: portable vertex、cluster、group、bounds、cone、stats。
- `virtualgeometryasset.h`: 所有するflat配列、build結果と明示的error。
- `virtualgeometrybuilder.h/.cpp`: buildの入口、前処理・leaf・hierarchy・validation・統計の実行。
- `virtualgeometrygeometry.cpp`: 入力検査、縮退／重複除去、meshlet、AABB／sphere／cone。
- `virtualgeometryhierarchy.cpp`: position remap、partition、境界lock、simplification、親生成、root。
- `virtualgeometryinternal.h`: 内部関数、32-bit addressingとrange検査。
- `virtualgeometryvalidator.h/.cpp`: geometry／DAG／boundary／statsの検証。
- `virtualgeometryserializer.h/.cpp`: encode／decode、save／load、checksum、読み込み制限。
- `virtualgeometryserializationinternal.h`: little-endian reader／writerとfile layout。
- `virtualgeometrymetadata.cpp`: build config、material対応、debug name、statsの保存。
- `virtualgeometryreport.h/.cpp`: 統計・階層・rootを人間が読めるテキストとしてdump。

上記の省略パスはすべて `source/virtualgeometry/` 内。
検証用に `tests/virtualgeometry_tests.cpp`、`tests/virtualgeometry/CMakeLists.txt`、
`scripts/validate-virtualgeometry.ps1` を追加した。
設計・手順はこの文書と `docs/virtualgeometry-phase1-plan.md` に記録した。

変更した既存ファイル:

- `source/models/staticmodel.h`: `StaticMeshData::CpuIndices` を追加。
- `source/models/staticmodel.cpp`: OBJ／AssimpのLOD0 uploadと同じ最適化後indicesを保持。
- `DirectX12.vcxproj`／`.filters`: 新規ソースと、既存meshoptimizerのclusterizer、indexgenerator、
  meshletutils、partition、spatialorderを登録。coreはPCH不要、Windows adapterだけ既存PCHを使用。

StaticModelResource、従来LOD、MeshShaderPipeline、GPU Driven、Visibility Buffer、Hi-Z、RenderGraphの
描画経路は置換していない。VirtualGeometryを毎フレームbuildしたり、既存描画へ自動接続したりする処理もない。

## Buildの使い方と流れ

```cpp
#include "virtualgeometry/virtualgeometrybuilder.h"
#include "virtualgeometry/virtualgeometryserializer.h"
#include "virtualgeometry/virtualgeometryreport.h"

VirtualGeometryBuildConfig config;
const uint32_t sectionIndex = 0;
auto result = VirtualGeometryBuilder::Build(resource.GetMeshData(sectionIndex), config, sectionIndex);
if (!result)
{
    Debug::Log("Virtual geometry build failed: %s\n", result.Error.c_str());
    return;
}

std::string error;
if (!VirtualGeometrySerializer::Save("model-section0.vgmesh", result.Asset, error))
{
    Debug::Log("Virtual geometry save failed: %s\n", error.c_str());
    return;
}
Debug::Log("%s", VirtualGeometryReport::Text(result.Asset).c_str());

VirtualGeometryAsset reloaded;
if (!VirtualGeometrySerializer::Load("model-section0.vgmesh", reloaded, error))
{
    Debug::Log("Virtual geometry load failed: %s\n", error.c_str());
    return;
}
```

各StaticMeshDataはmaterial section単位でbuildする。`MaterialId` はresource内のsection index。
浮動小数のshader用 `AppliedMaterialPartId` は `SourceMaterialPartId` として別途保持する。
GPU readbackは使用せず、従来 `LodIndexBuffers` は入力に使用しない。
portable APIは `span<const VirtualGeometryVertex>` と `span<const uint32_t>` を受け取り、GPU／device不要。
Windows adapterはStaticModelVertexの12属性を明示的に変換する。

1. config、index range、全頂点属性の有限性を検査。
2. exact full-attribute remapを使って重複triangleを識別し、zero-area triangleを除去。
3. source LOD0をleaf meshletsへ分割し、local vertex／uint8 triangleをflat配列へ格納。
4. 同じlevelの位置共有関係と近接性でpartitionする。
5. groupの一時meshを組み立て、外部／非多様体境界をlock。
6. 属性付きsimplification、境界検証、parent meshletsの再生成。
7. 誤差を伝播して次のlevelへ反復。削減できないgroupの子はterminal rootsにする。
8. 統計を再集計し、full validatorを実行して成功したassetだけを返す。

## Cluster生成とBounds

同梱meshoptimizer 1.1の実ヘッダーと実装を確認し、`meshopt_buildMeshlets` と
`meshopt_optimizeMeshlet` を使用する。デフォルトは128 triangles／256 vertices、cone weight 0.5。
configで上限を小さくすることは可能だが、この上限を超える設定はerror。
triangle offsetの単位はbyte、triangle 1個は3 bytes。vertexは必ずcluster-local。
Position／Normal／UV／Diffuseのすべてを保存し、seam属性を勝手にweldしない。

AABBは格納頂点から生成。sphereはAABB中心と角への距離から、double中間計算と上向き丸めで作る。
coneはtriangleの幾何法線からdoubleで計算する。axisは平均法線、cutoffは全法線を覆う角度と
axisのfloat丸め誤差から生成する。apexは全triangleの背面側へ押し出す。
広すぎるconeはcutoff=1で無効化する。meshoptimizer同様、cutoffはbackface test用のsin半角表現。

極小座標（cubeを1e-12倍）の追加テストでmeshoptimizerのfloat boundsが半径0になる問題を再現し、
上記のdouble計算へ切り替えた。1e8倍のcubeも検証済み。
group sphereはchildまたはchildの生成groupのsphereを包含するため、descendant geometryも覆う。
`meshopt_computeSphereBounds` で中心を求めた後、doubleで包含半径を再確認して上向きに丸める。

## Hierarchy・Crack防止・LOD Error

`meshopt_generatePositionRemap` と `meshopt_partitionClusters` を使い、デフォルト8 clusters/group。
child setとparent setは `HierarchyIndices` 内の連続rangeで表す。
clusterの `GeneratingGroupIndex` は生成元、`GroupIndex` は消費先。消費先なしがroot。
複数の親クラスタが同じ子setを表すDAGであり、単一parentのtreeは前提にしていない。

境界は無向位置edge keyのhash表で数える。group内の使用回数が2以外、別groupとの共有edge、
level全体の使用回数が2以外のedgeを保護する。
UV／normal seamでも同位置の全vertex copyへ `meshopt_SimplifyVertex_Lock` を設定する。
`meshopt_simplifyWithAttributes` を使い、Normal 3、UV 2、Color 4の計9属性を渡す。
weightはデフォルト1.0／1.0／0.25、target trianglesは約50%、relative error上限は1.0。
topologyまたはlockによりtargetへ到達できなければ実際の結果を採用する。

簡略化は元vertexのindicesを返す方式なのでpositionを変更しない。
protected edgeの位置・使用回数を簡略化前後で完全一致検証し、新しい外部境界も拒否する。
reclusterされた実assetのchild／parent geometryから、full validatorでも境界を再検証する。
ハッシュ表の走査順はlock条件の集合にだけ作用し、assetの生成順は入力・partition ID順で固定。
triangle×triangle、cluster×clusterの総当たりは使用しない。

leaf errorは0。parent errorは仕様どおり
`max(relative result_error * meshopt_simplifyScale(group), maximum child LodError)`。
weighted attribute metricを含む誤差であり、厳密なHausdorff距離や累積幾何誤差の証明ではない。
groupのMinErrorは最大child error、MaxErrorはparent error。
triangle数とcluster数の両方が減るgroupだけを階層へ採用する。
no progress、小さいfrontier、最大32 levelsで停止。失敗partitionの子はrootとして残すため、
rootsは複数で、levelも混在してよい。group temporaryは処理終了時に破棄する。

## .vgmesh file format

raw、little-endian、pointerなし、paddingなしのfield-by-field serialization。圧縮は未実装。
float32／float64はbit representationを保存する。

固定headerは160 bytes:

| Byte offset | 内容 |
|---:|---|
| 0 | uint32 magic: VGM1 |
| 4 | uint32 version: 1 |
| 8 | uint64 total file bytes |
| 16 | uint64 FNV-1a checksum（byte 32以降、directoryも含む） |
| 24 | uint32 section count: 8 |
| 28 | uint32 header bytes: 160 |
| 32 | 8個のdirectory entry: uint64 offset、uint64 element count |

sectionsはVertices、Triangle bytes、Clusters、Groups、Hierarchy indices、Roots、Level stats、Metadata。
record strideは順に48／1／104／44／4／4／20／1 bytes。
Metadataはconfig、material対応、debug nameとbuild statistics。
実測BuildMillisecondsは非決定的telemetryのため保存・hashから除外し、reload後は0にする。
それ以外のgeometry／hierarchy／bounds／error／roots／settings／statsは完全にround-tripする。

loadはfile size、version、連続offset／count／stride、overflow、checksumを確認してから確保する。
既定のfile／decoded payload budgetはそれぞれ1 GiBで、呼び出し側が変更できる。
metadata内部の可変長nameもsection残量を超えるとerror。
最後にfull validatorを通してからcallerのassetへmoveする。失敗時は既存assetを保持する。
FNVは破損検出・content fingerprintであり、暗号学的な認証ではない。

## Validator

Debug／Releaseともbuild完了・save前・load後に実行し、明示的errorを返す。

- cluster上限、empty、local triangle index、vertex／triangle flat range、未参照payload。
- 全vertex属性、bounds、cone、errorのNaN／Inf、AABBの順序、sphere半径とgeometryの包含。
- material section一致、groupのchild／parent rangesと参照、重複member、producer／consumer一致。
- rootsの範囲・重複、全terminalのroot登録、orphanの拒否。
- 厳密なparent level = child level + 1によるcycle拒否。
- parent error >= 最大child error、group error対応、triangle／cluster数の削減。
- descendant sphereの包含、child／parentの外部・非多様体edge multisetの保持。
- source = removed degenerate + removed duplicate + final leaf triangle count。
- per-level counts、全体counts、max／average、memory size統計。

## 検証結果

再実行は `scripts/validate-virtualgeometry.ps1`。Debugは標準suite（10万triangleを含む）、
Releaseは標準suiteと100万triangle stress suiteを実行する。
`-SkipMillion` で100万triangleを省略可能。
CPUテスト用CMakeはD3DやGPUを必要としない。

実行済みのRelease計測（build時間にはfull validationを含む。save／load時間は含まない）:

| Model | Source triangles | Leaf clusters | Total clusters | Groups | Levels | Roots | Build ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| Cube | 12 | 1 | 1 | 0 | 1 | 1 | 0.012 |
| 波状grid 80×80 | 12,800 | 100 | 206 | 26 | 7 | 3 | 54.31 |
| UV／Normal seam付きgrid | 12,800 | 100 | 206 | 26 | 7 | 3 | 50.80 |
| 波状grid 224×224 | 100,352 | 784 | 1,644 | 213 | 9 | 7 | 490.77 |
| 波状grid 710×710 | 1,008,200 | 7,877 | 16,423 | 2,054 | 12 | 24 | 5,858.56 |

モデルはテスト内で決定論的に生成したCPU mesh。既存のimported art assetの品質比較ではない。
100万triangle assetのhashは `4836873047369230373`。
Cube／中密度／10万triangle／seamモデルはDebugとReleaseのhashも一致した。
各モデルでbuild 2回の完全byte一致、save、元asset unload、load、再encode byte一致を検証した。
全geometry、cluster、group、hierarchy、bounds、error、rootの一致をこのbyte比較に含む。

追加でempty、1 triangle、zero-area、duplicate triangle／vertex、unused vertex、NaN、Inf、
極小／大座標、UV／normal seam、重なった非多様体面、20 disconnected cubes、複数root、
最大depth停止、config不正、local index／material／group／root／error／sphere破損、
version／checksum／offset／count破損、truncation、file budgetを検証した。

| 実行 | 結果 |
|---|---|
| CPU Debug CTest | 1/1 passed（標準suite） |
| CPU Release CTest | 2/2 passed（標準＋100万stress） |
| dev_main Debug C++／HLSL build | 成功 |
| dev_main Release C++／HLSL rebuild | 成功 |
| 既存Procedural editor checks | 976 checks、0 failures（両構成） |
| 既存Simulation math checks | 2,801 checks、成功（両構成） |
| 既存Debug描画features／fallback | 各84 frames、0 smoke failures |
| 既存Release描画features／fallback | 各84 frames、0 smoke failures |
| project XML／登録重複／git diff whitespace | 問題なし |

CPUの最終ビルドログにwarning／errorなし。
既存全体ビルドにはSkyAtmosphereのX3556、NISのX4000、検証スクリプトによるdebug info overrideの
D9025が残る。これらの既存shaderやbuild方針は本タスクで変更していない。
Releaseの最初のincremental buildは既存PCHのdebug info形式と `/Z7` が不一致で失敗した。
同じ `/Z7` 設定でRebuild後、Releaseの機能／fallback検証は成功した。

詳細ログは `bin/validation/virtualgeometry/`、全体rebuildログは
`build/virtualgeometry/release-rebuild.log`、描画ログ・画像は
`bin/validation/{Debug,Release}-{features,fallback}/`。

## 制約とPhase 2への引き継ぎ

- source componentの絶対値は1e8以下に限定し、float内部計算の極端なoverflow入力を明示的に拒否する。
- exact positionで境界判定する。近接しているだけの位置をepsilon weldする処理はない。
- open／non-manifold／seamのlockで削減が止まるのは正常。全モデルを1 rootにはしない。
- corpusとして使用したのは生成モデル。複雑な実制作assetのLOD品質比較や表示比較は今後必要。
- 確保budgetはdecoded payloadの制限。validatorのhash表などのworking memoryも別途必要になる。
- BuildMillisecondsはreloadで復元しない。時間を含むreportを必要なら別途保存する。
- GPU専用描画、streaming、compression、GPU traversal、culling、screen-space error selectionはPhase 2以降。

Phase 2へはlocal vertex／uint8 triangles、offset／count、material section対応、level、
group child／parent ranges、producer／consumer、複数roots、sphere／AABB／cone、LOD error、
configとversionを渡せる。rootのboundsは生成groupのdescendant boundsも考慮すること。
parent setはgroupの子set全体を置き換えるため、parentを独立に選択して子setと重複描画しないこと。
GPU LOD cut／group選択の整合性とscreen-space errorの校正はPhase 2で検証する。
