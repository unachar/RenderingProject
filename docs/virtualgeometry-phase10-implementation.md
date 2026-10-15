# フェーズ10: GPU Geometry Compression

2026-10-15。提示されたPhase 10仕様に基づくローカル実装。ユーザー指示により、テスト、C++ビルド、シェーダーコンパイル、アプリ起動、GPU検証、性能測定は一切実施していない。以下はコードの実装内容であり、Acceptance Criteriaの検証済み報告ではない。

## 描画経路

`VirtualGeometryPages::Build` は既存のCluster/Hierarchy生成結果を受け取り、Mesh全体でCodecとPosition gridを選ぶ。その後、既存64 KiB PageへCluster全体を詰める。rootのpin、Page順序、StreamingのCopy Queue、Fence、スロット世代管理、Page Poolのraw SRVは継続使用する。Cluster別GPU resourceは追加していない。

通常のnative経路はencoded PageをそのままUploadし、`VGGeometryDecode.hlsli` を通じてMesh Shader内で復号する。共通MSを含む通常、Velocity、Binned、Binned Velocityの各variantに接続した。Velocityとprimitive cullingはPositionだけを復号し、現在のPBR描画はPosition/Normal/UV0/Colorだけを読む。Material ID、Bin Key、visible recordの意味は変更していない。既存の不透明・片面PBRの対応範囲を継承し、masked等の新しいProgrammable Raster対応を追加する変更ではない。

CPU完全復号はAsset Build時の精度・Bounds検証と、参照Decoder／テストに使用する。RuntimeのPage読み込みは構造・数値表現・Indexを検査するが、復号済みVertexBufferを生成しない。Culling metadataはGeometryとは別の常駐bufferを使う。

## Codecと誤差

- Position: Mesh全体共通のOrigin/StepによるUNorm16x3。階層の全レベルを含む同一Positionを同じ整数へ丸める。ゼロExtentは量子化値0。全頂点のobject-space距離誤差が閾値を超える場合、Mesh全体でFloat32x3へ戻す。
- Normal: Oct SNORM16x2、復号後に正規化。角度誤差超過でFloat32x3。
- Tangent: SNORM10:10:10:2、Wは+1/-1。復号NormalとのGram-Schmidt直交化。角度誤差超過でFloat32x4。Normal圧縮が高精度Tangentの直交性を損なう場合はNormalもFloat32に戻す。
- UV: channelごとのFloat16x2。範囲・距離誤差超過でFloat32x2。24 channelまで記述可能。
- Color: UNorm8x4。HDR／範囲外の有限値はFloat32x4で保持。存在しない場合はStreamを作らない。
- Index: 256頂点以下はUInt8、それより多い場合はUInt16。設定でUInt16を強制できる。CPU Codecは257頂点を扱うが、既存native描画の64頂点／124三角形上限は拡張していない。

設定は`VGCompressionSettings`。初期値はPosition 0.001 object-space units、Normal 0.001 rad、Tangent 0.005 rad、UV距離0.0005。既存assetの大きさに対する自動スケール変更は行わない。Fallback理由を属性名付きで標準ログへ出し、通常準備経路ではgraphics logにも出す。NaN/Infを持つ入力は有効なFloat32 Geometryにもできないため、属性名・頂点番号付きで拒否する。

既存`VirtualGeometryVertex`にはTangentと追加UVの入力フィールドがないため、通常importでそれらを捏造しない。`VirtualGeometryCompression::Input`、`Pages::Build`追加overload、`Serializer::SaveEncoded`の追加引数で、生成済みHierarchyの**全レベルのflattened vertex順**に揃ったTangent／複数UVを渡せる。Normal/UV/Colorは存在フラグで省略できる。新しい属性のimport・LOD補間処理は追加していない。

Cluster sphereは復号Positionで外向きにrefitし、Group sphereとLOD MaxErrorには位置誤差を加える。Positionが動いたMeshでは既存Coneを無効にし、元の三角形に由来するConeで誤って消さない。Cone精度の再構築は今後の最適化余地となる。

## Wire format

旧CPU reference `.vgmesh` v1とPage形式v2の読み取りを維持する。Encoded `.vgmesh` は既存Page container family（magic VGP2）のVersion 3。Phase番号とは独立している。`SaveEncoded`で書き、`LoadEncoded`でmetadata/directoryを読み、`ReadPage`で必要なPageだけを取得する。旧`Load`はCPU reference形式専用のまま。未知Versionは`Unsupported VGMesh format version`で失敗する。

Header、Cluster、Group、Compression metrics、Page directoryはlittle-endianのfield-by-field serialization。構造体のABIやpaddingを保存しない。Pageごとにchecksumを持つ。

Cluster PayloadはVGC1形式。

| Byte offset | 内容 |
| --- | --- |
| 0 / 4 | Magic / aligned payload bytes |
| 8 / 12 | VertexCount / TriangleCount |
| 16 / 20 | Attribute mask / Index width (1または2) |
| 24 / 28 | Index offset / Stream count |
| 32 / 44 | Mesh-global Origin float3 / Step float3 |
| 56以降 | Stream descriptor列。各16 bytes: semantic、Codec、relative offset、stride |
| 各Stream後 | 0～3 bytesのゼロpadding |
| Index stream | i0/i1/i2の連続値、末尾を4-byte境界までゼロpadding |

Stream開始とCluster開始は4-byte境界。Positionの6-byte stride、Indexの1/2-byte要素はaligned DWORDからbyteを抽出して読む。GPU helperは非aligned `Load`を行わない。Origin/StepとdescriptorはCluster header内に重複保存するため、理論上の18 bytes/vertexに追加headerとpaddingがかかる。

RuntimeロードはPage checksumに加え、Codec、semantic順序と重複、mask、stride、Stream範囲、Index幅と値、有限Float32/Half、quantization gridのoverflowを検査する。不正PageをGPUへ転送しない。

## Cache・統計・比較

通常準備経路は`<従来CachePath>.<SettingsKey>.encoded.vgmesh`を再利用／生成する。SettingsKeyは明示的に直列化した設定とEncoder revisionから決め、実行順のAsset IDに依存しない。従来描画、既存Inspector、CPU互換性のため、元のraw CPU Cacheとsource geometryは保持する。**Cache全体のディスク容量・CPU RAMがこの変更だけで減るとは主張しない。** 読み込み後の圧縮Geometryを通常頂点配列へ展開する処理は追加していない。

Debug UIはEncoded/Raw payload bytes、比率、Fallback数、Quantized mesh数、Half UV stream数、Index8/16 cluster数、Resident Page allocation、累計Upload bytesを表示する。Payload bytesと64 KiB単位の実Page allocationを区別する。別採取していないGPU Decode時間を0 msとして表示しない。現在はnative raster内に含まれる。

比較用に`VGCompressionSettings::Enabled=false`でPhase 9の48-byte raw vertex／UInt8 index Page layoutを出力できる。通常importの別起動で`DX12_VG_COMPRESSION_RAW=1`を指定するとraw Pageを使い、別名Cacheへ保存する。事前構築Manifestが指定された経路ではManifest自体の形式を使用する。

## 追加した検証コード（すべて未実行）

- CPU Codec: 通常／巨大／微小／Flat／ゼロExtent／負座標、Cluster境界の同位置の完全一致、Oct両半球、Tangent handedness、複数UV、Half範囲Fallback、Color欠如、255/256/257頂点、破損offset・Index・truncation。
- Page tests: 復号Position誤差、保守的sphere内包含、Index一致、ファイル全体の決定性、Version拒否、checksum／directory破損。
- GPU parity: `virtualgeometry_decoder_parity_tests.cpp`と専用CS。Encoded raw SRVからPosition/Normal/Tangent/UV0/Color/IndexをGPU復号・ReadbackしてCPU参照と比較する。Debug LayerとGPU Based Validationを有効にし、error/corruptionメッセージを失敗扱いにする。UInt8/UInt16およびFloat32 variantsを含む。アプリのScene描画を検証するものではない。
- opt-in CMake target `virtualgeometry_decoder_parity_tests`は専用CSのコンパイル依存を持つ。通常のCPU targetへD3D12 parityの実行は追加しない。
- `virtualgeometry_tests --compression-stress <output.vgmesh>`は約100万source三角形を構築し、Encoded出力と全Pageのロードを検査する。画面検査やGPU Stressの代用ではない。
- `virtualgeometry_tests --compression-benchmark <legacy-native64.vgmesh> <output-directory>`は同じHierarchyからraw／encodedファイルを作り、Payload／Page allocation／file bytes／Fallback数をCSVで出す。

将来のGPU parity実行入口:

```powershell
cmake --build build/virtualgeometry --config Release --target virtualgeometry_decoder_parity_tests
build/virtualgeometry/Release/virtualgeometry_decoder_parity_tests.exe build/virtualgeometry/VirtualGeometryDecoderParity.cso
```

実Sceneの比較は同一Scene・Camera・解像度・材質・Cluster設定でraw/encodedを別起動し、cold/warm Streamingを分けて行う。既存Profilerのnative raster、frame time、FPS、Page residency、Upload、I/O error/stallを取得する。別測定のDecode microbenchmarkとraster内の総コストは混同しない。

## 未確認の完了条件

C++/HLSLコンパイル、CPU test、GPU parity、すべての描画variant、Phase 8 temporal/2-pass culling、Phase 9 binning、Page Evict中の描画、画面・depth・velocity、境界の亀裂、D3D12/GBV error 0、GPU stress、実測Resident削減、fileサイズ削減、+5% frame time目標は未確認。圧縮実装を追加したこととPhase 10のAcceptance全項目を満たしたことは区別する。Archive compression（10.7）は未実装で対象外。

既存のローカル変更を保持し、commit/push、別checkout、追加agentは使用していない。
