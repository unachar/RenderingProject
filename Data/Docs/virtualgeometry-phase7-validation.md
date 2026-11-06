# Phase 7 検証記録

2026-10-15。Windows / D3D12、NVIDIA GeForce RTX 3050 Laptop GPU（4 GB）。実装は既存checkoutへ統合し、コミット・pushは行っていない。

## 実装と確認範囲

| 要件 | 実装 | 検証 |
|---|---|---|
| 固定Page、Hierarchy常駐 | `virtualgeometrypages` / `virtualgeometrypagecache`、64 KiB、VGP2、64-bitファイルoffset | 全Page往復、checksum、破損・切断・offset overflow拒否、生成失敗時の旧cache保持 |
| GPU常駐判定・Request | `VirtualGeometryTraversalCS.hlsl`、frame別Residency、dedup bitset、上限65536件 | production GPU traversal/reference 46ケース。部分到着、共有親、欠落root、非ゼロPageBase、overflow |
| SSD→RAM→VRAM | `virtualgeometrypageio` / `virtualgeometrystreaming`、4 worker、128件まで待機、staging上限64 MiB | 実SSD read、I/O遅延・失敗・retry、in-flight/完了済みI/O cancel |
| Copy非同期転送・寿命 | `virtualgeometrypagepool`、独立COPY Queue、世代付きticket、graphics/copy両Fence | 64 KiB exact readback、Copy前publish拒否、graphics未完了中のslot保持、解放後のreplacement bytes、古い世代の完了拒否、debug error 0 |
| Budget・Evict | 上限1 GiB、root固定、LRU、物理確保と予約を別計上 | 予算縮小と復元、Evict、再要求。32回の登録拒否後もasset/cluster/Page数不変 |
| 親Fallback・高LOD復帰 | 全親activeかつ全子resident時だけgroup置換、instance単位で出力予約 | CPU residency組合せ、GPU cut比較、native/traditional raster比較 |
| Mesh Shader / velocity | Page descriptor配列、非ゼロbyte offset | paged/contiguous Level 3 raster、移動transformのvelocity両経路 |
| UI / Profiler | 4状態Page数、request、SSD/Copy bytes、実使用/予約/予算、metadata/staging、CPU/I/O/Copy完了時間 | Debug/Releaseビルドとruntime。Copy時間はCPU観測の完了待ち時間であり、GPU timestampではない |
| 高速移動の穴・重複 | teleport、距離変更、native切替、I/O障害、低予算 | sampled coverage、cluster重複・祖先同時選択・native/legacy/fallback排他検査 |

通常frameの同期counter readbackは無効。画面・routingのサンプル時だけ同期captureする。通常frameでgraphicsの未完了Fenceが存在することをassertし、毎frame同期による偽の安全性を避けた。

## 最終実行結果

- `scripts/validate-virtualgeometry.ps1`: Debug 1/1、Release 2/2合格。Release million-triangle stressを含む。
- `scripts/test-shader-build-targets.ps1`: 合格。既存ShadowCacheの二出力targetを個別扱いし、22個の単一出力shader targetを検証。
- `scripts/validate-virtualgeometry-phase6.ps1 -SkipCpu`: GPU 46ケース、paged/contiguous rasterとvelocityすべて合格。
- `scripts/validate-virtualgeometry-phase7.ps1 -Configuration Release -Gpu -Runtime -StressTriangles 100000000 -KeepStressFiles`: 合格。
- `scripts/validate-virtualgeometry-phase7.ps1 -Configuration Debug -Runtime -StressTriangles 30000000`: 合格、D3D12 debug errors 0。
- `git diff --check`: 合格。

### 1億triangle Release

生成した別々の62 asset、source 101,580,800 triangles。各assetに32,768 leaf clusters、各50 triangles。単一assetのinstance数を増やした換算値ではない。親LOD込みのcacheは1 asset 76,375,968 bytes。ページpayloadは生成時も64 KiB単位で扱い、全量をRAMへ展開しない。

240 frames、画面比較18回、routing検査18回、失敗0。GPU requests 24,733、SSD 20,905,984 bytes、Copy完了20,774,912 bytes、Evict 194回。注入I/O失敗1回から復旧し、追加の読取失敗なし。終了時pool 8,257,536 bytes、目標8 MiB以内。

root warmup後209 samplesの平均CPU render 8.390 ms、GPU render 4.175 ms。process working set 1,902,080,000 bytes、private bytes 3,531,587,584。初期metadata登録の最大CPU 14,397 msはsteady平均から除外。検査時の最大selected 1,715 clusters、leaf 0で、この低予算・短い移動試験では親LODを主体に描画した。全source triangle同時描画や全leaf到着の性能試験ではない。leafへの昇格は小規模CPU/GPU cut試験で確認している。

ログ: `bin/validation/virtualgeometry-phase7/runtime-Release-100000000.log`。

### 約3000万triangle Debug

別々の19 asset、source 31,129,600 triangles。240 frames、画面比較15回、routing検査15回、失敗0、D3D12 errors 0。GPU requests 24,931、SSD/Copy完了とも18,481,152 bytes、Evict 190回、注入I/O失敗1回から復旧。終了時pool 6,094,848 bytes、目標6 MiB以内。

root warmup後181 samplesの平均CPU render 10.574 ms、GPU render 3.953 ms。process working set 1,370,800,128 bytes、private bytes 2,786,373,632。最大selected 2,075 clusters、leaf 0。初期metadata登録を含む最大CPU 15,484 ms。通常frameに未完了graphics workが存在することも確認した。

ログ: `bin/validation/virtualgeometry-phase7/runtime-Debug-30000000.log`。

両stress実行後に再生成可能なcacheとfallback OBJ（合計約6.2 GB）を削除し、検証ログは保持した。

### 10億triangle

1億試験の合格後、実機の収容判定を実行。metadata buffer見積3,623,931,904 bytes、frame/pool予備を含む必要量4,469,280,768 bytesに対し、DXGI VRAM budgetは3,533,963,264 bytes。exit 78 / SKIPで停止し、巨大cache生成と描画は実施していない。Hierarchy常駐の設計上、このGPUでは収容できないため、10億triangleは描画未検証。

再開: 十分なVRAMとSSD空き容量のある環境で `scripts/validate-virtualgeometry-phase7.ps1 -Configuration Release -Runtime -StressTriangles 1000000000`。生成cacheは概算47 GBに加え、scriptが要求する8 GiBの空きを確保する。

ログ: `bin/validation/virtualgeometry-phase7/admission-1000000000.log`。

## レビューと残る制約

独立したread-only reviewを実施。重要指摘（metadata append前の予算判定、満杯時の反復走査、毎frame同期が検証を弱める問題）を修正し、登録拒否・Fence未完了中の再利用・非同期runtimeの検証を追加した。

通常import経路は既存CPU hierarchy/geometryを互換性のため保持する。巨大sceneにはmanifest経路を使う。現状は非圧縮Pageであり、DirectStorageや圧縮は未導入。常駐metadataはgeometry budgetと別に必要で、Hierarchyそのもののout-of-core化は対象外。

stress入力は決定的な平面群であり、任意の制作asset、全material、透過・特殊描画の全組合せを保証するものではない。既存Phase 6 Debug acceptanceではmaterial fallbackとnative/traditional GBuffer一致も確認済み。
