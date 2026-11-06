# 描画・アニメーション技術100件の一次調査

調査日: 2026-10-30。対象は goal-objective.md の100 URLすべて。

ユーザー選択: 既存DirectX 12実装へ段階的に追加する。

## 調査の範囲

各リポジトリのメタデータとREADMEを取得し、機能、依存先、既存実装との重複、次に確認する課題を整理した。これは全ソースの監査や採用完了を意味しない。READMEの宣伝値を本エンジンの性能実測として扱わない。ライセンス欄はGitHubの検出結果であり、実際にコード・データ・SDKを取り込む前には対象ファイルと再配布条件を確認する。未検出は利用自由という意味ではない。

facebook/zstd は meta/zstd へ転送されたため移転先を確認。READMEの大小文字が異なる5件はroot一覧から正しいファイルを取得した。全100件でREADME取得済み。参照branch、取得先、文字数、GitHub検出ライセンスは隣接JSONに記録した。branchは可変なので、採用時にはcommitを固定する。

## 既存実装の確認

- C++/DirectX 12/HLSL。Unityプロジェクトではない。
- LTC/DFG: shader/hlsl/SurfaceLighting.hlsli と source/rendering/ltctables.h。
- ReSTIR GI/放射照度cache: shader/hlsl/RayReflectionsCS.hlsl。NRD/DLSSのSDKと接続あり。
- BLAS compaction: source/rendering/raytracing.cpp の ApplyPendingCompaction / EnsureCompactionBuffers。
- transient placed resources: source/rendering/transientarena.h。メモリallocator全体が未実装とは扱わない。
- BC texture cache/MIP streaming: source/assets/texturecompression.* と texturemanager.cpp。
- GPU skinning/DQS/TEO/pose cache: source/models/animationskinning.cpp、animationcache.cpp、shader/hlsl/skinning_cs.hlsl。
- 大気LUT/雲: shader/hlsl/AtmosphereLutCS.hlsl、CloudRaymarchPS.hlsl、source/rendering/cloud/。
- 多灯Toonと画面幅輪郭: ToonLighting.hlsli、ToonOutlineProjection.hlsli、docs/live-toon-lighting.md。
- AA/SR: TemporalUpscaleCS.hlsl、TAA_BlendPS.hlsl、dlssupscaler.cpp。AO/OITも既存経路あり。

## 個別評価

下表の候補は実装済みという意味ではない。効果が見込める候補にも実資産・実機での比較が必要。

|#|参照元|評価と次の作業|
|---|---|---|
|1|[NVIDIA-RTX/RTXDI](https://github.com/NVIDIA-RTX/RTXDI/blob/main/README.md)|ReSTIR DI/GI/PT。既存MegaLights/ReSTIR GIと重複。DIの標本化・履歴重みを比較し、PTは参照モードから段階導入。|
|2|[selfshadow/ltc_code](https://github.com/selfshadow/ltc_code/blob/master/README.md)|LTC面光源。既存SurfaceLightingとLUTあり。再導入せず矩形・線・円盤の対応範囲と地平線境界を比較。|
|3|[NVIDIA-RTX/RTXGI](https://github.com/NVIDIA-RTX/RTXGI/blob/main/Readme.md)|現行はNRC/SHaRC。既存ハッシュ放射照度キャッシュと比較。NRCはSDK・GPU要件、学習時間、履歴破棄を別評価。|
|4|[NVIDIA-RTX/RTXCR](https://github.com/NVIDIA-RTX/RTXCR/blob/main/README.md)|髪・肌、LSS、morph。既存Toon髪へ一律適用せずPBRキャラクター用の別経路候補。DXR機能と資産形式を確認。|
|5|[google/filament](https://github.com/google/filament/blob/main/README.md)|小型PBR実装。幾何スペキュラAAを初回候補に選定。既存DFG・多重散乱補償は維持。|
|6|[NVIDIA-RTX/NRD](https://github.com/NVIDIA-RTX/NRD/blob/master/README.md)|既存External/NRDと連携コードあり。更新前に入出力の法線・粗さ・viewZ・motion契約とバージョン差分を確認。|
|7|[AcademySoftwareFoundation/OpenColorIO](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/main/README.md)|カラー管理。AgX/ACES近似の追加とは別に入出力色空間・OCIO設定を扱う。色変換の二重適用を防ぐ。|
|8|[NVIDIA-RTX/STBN](https://github.com/NVIDIA-RTX/STBN/blob/main/README.md)|[追加source audit](rendering-technology-stbn-source-audit-2026-10-31.md)。既存R2/R4あり、baseline試験PASS。初回候補はAO低SPP。型/PDF/8bit export/非商用・商用両licenseを確認。RG8なら候補本体2MiB、実data未取得・効果未測定。4D/影8phaseを一括置換しない。|
|9|[NVIDIAGameWorks/Falcor](https://github.com/NVIDIAGameWorks/Falcor/blob/master/README.md)|DX12/Vulkan研究基盤。RenderGraphとReSTIR PTの設計参考。エンジン全置換は選択しない。|
|10|[GPUOpen-Effects/FidelityFX-CACAO](https://github.com/GPUOpen-Effects/FidelityFX-CACAO/blob/master/readme.md)|CACAO。既存visibility-bitmask AOと同半径・画質で比較する代替候補。AOの二重乗算を避ける。|
|11|[pboechat/PCSS](https://github.com/pboechat/PCSS/blob/master/README.md)|OpenGLのPCSS例。既存影フィルターのblocker searchとpenumbra計算を確認して不足のみ追加。利用条件は本文確認。|
|12|[NVIDIA-RTX/RTX-Kit](https://github.com/NVIDIA-RTX/RTX-Kit/blob/main/README.md)|SDK群の入口。単一ライブラリとして追加しない。RTXDI/RTXGI/RTXCR/RTXTF等を個別判定。|
|13|[HaringPro/Revelation](https://github.com/HaringPro/Revelation/blob/dev/README.md)|Minecraft/Iris GLSLパック。大気・雲・品質切替の参考。Iris固有バッファ契約をD3D12へ直接移植しない。|
|14|[Ads9115/Atmospheric-Scattering](https://github.com/Ads9115/Atmospheric-Scattering/blob/master/README.md)|単一散乱のOpenGL実装。既存大気LUTを単純方式へ置換せず、Rayleigh/Mie/透過の比較参照に使用。|
|15|[NVIDIA-RTX/RTXTF](https://github.com/NVIDIA-RTX/RTXTF/blob/main/README.md)|確率的テクスチャフィルタリング。TSR/DLSSの収束と併せて評価。1標本のノイズ増大とalpha-tested材質に注意。|
|16|[NVIDIA-RTX/OMM](https://github.com/NVIDIA-RTX/OMM/blob/main/README.md)|Opacity Micromaps。alpha-tested DXRのany-hit負荷削減候補。D3D12/API・GPU対応とbake容量を先に確認。|
|17|[Unity-Technologies/Graphics](https://github.com/Unity-Technologies/Graphics/blob/master/README.md)|SRP全体の参考。C#/Unityパッケージは本C++エンジンへ導入しない。AO・影・材質の関連HLSLを個別比較。|
|18|[wjakob/layerlab](https://github.com/wjakob/layerlab/blob/master/README.md)|層状BSDFのオフライン計算。LUT生成や参照レンダリング用。リアルタイム全ピクセル評価は予算を先に確認。|
|19|[mitsuba-renderer/mitsuba3](https://github.com/mitsuba-renderer/mitsuba3/blob/master/README.md)|オフライン参照画質用。高sppの材質・照明比較に有用。通常フレームのランタイム依存にはしない。|
|20|[KhronosGroup/glTF-Sample-Viewer](https://github.com/KhronosGroup/glTF-Sample-Viewer/blob/main/README.md)|glTF材質拡張の公式比較対象。既存インポートのclearcoat/sheen/transmission等の対応を棚卸し。|
|21|[GPUOpen-Effects/FidelityFX-SSSR](https://github.com/GPUOpen-Effects/FidelityFX-SSSR/blob/master/readme.md)|SSSR。既存DXR反射に対する非DXRフォールバック候補。画面外・遮蔽解除と二重反射を検証。|
|22|[ML200/RoyalTracer-DX](https://github.com/ML200/RoyalTracer-DX/blob/main/README.md)|DX12のReSTIR・SHaRC・階層材質を参考。既存RT経路と差分比較し、個別技術へ分解して採用。|
|23|[GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator/blob/master/README.md)|D3D12メモリのサブ割当。既存transientarenaのplaced resourcesと重複。永続小リソースの割当数・断片化を計測して判断。|
|24|[wolfpld/tracy](https://github.com/wolfpld/tracy/blob/master/README.md)|CPU/GPUプロファイリング。既存CSV/GPU時刻計測を維持し、必要なら任意有効の詳細トレースとして追加。|
|25|[NVIDIA-RTX/ShaderMake](https://github.com/NVIDIA-RTX/ShaderMake/blob/main/README.md)|バッチ・並列シェーダービルド。既存MSBuild依存追跡を保持。初回は不要DXIL情報の容量削減を調べる。|
|26|[NVIDIA-RTX/RTXTS](https://github.com/NVIDIA-RTX/RTXTS/blob/main/README.md)|RTXテクスチャストリーミングのbeta SDK。既存reserved resource/MIP管理と比較。タイル要求・IO・圧縮形式を個別統合。|
|27|[NVIDIA-RTX/RTXMU](https://github.com/NVIDIA-RTX/RTXMU/blob/main/README.md)|BLASメモリの圧縮・管理。既にALLOW_COMPACTION/readback/compactあり。再導入より断片化と破棄時Fenceを監査。|
|28|[intel/xess](https://github.com/intel/xess/blob/main/README.md)|XeSS。既存DLSS/TSRに追加する選択式候補。motion/depth/exposureとSDKライセンス・対応デバイスを確認。|
|29|[NVIDIA/DLSS](https://github.com/NVIDIA/DLSS/blob/main/README.md)|既存DLSS連携あり。SDK更新・Ray Reconstruction等は入力契約と機能検出を検証後に追加。常時重複アップスケールしない。|
|30|[GPUOpen-LibrariesAndSDKs/FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/readme.md)|現行FSR SDK群。既存External/FidelityFX-FSRとバージョンを照合し、SR/FG/denoiseを独立評価。|
|31|[NVIDIA-RTX/Streamline](https://github.com/NVIDIA-RTX/Streamline/blob/main/README.md)|複数ベンダーの機能統合層。現在の直接DLSS連携の置換費用を評価。SR/FG/latencyは別設定にする。|
|32|[arm/accuracy-super-resolution](https://github.com/arm/accuracy-super-resolution/blob/main/README.md)|モバイル向けASR。デスクトップD3D12では優先度を下げる。TSRの低負荷モードとしてアルゴリズム比較。|
|33|[lilxyzw/lilToon](https://github.com/lilxyzw/lilToon/blob/master/README.md)|多機能Toon。既存LiveToonへ陰影・リムの必要部分を個別比較。Unity/VRChat固有の依存は持ち込まない。|
|34|[Unity-Technologies/com.unity.toonshader](https://github.com/Unity-Technologies/com.unity.toonshader/blob/master/README.md)|Unity Toon Shader。主光源・補助光・輪郭の基準参考。既存多灯Toonと画面幅輪郭を維持。|
|35|[stalomeow/StarRailNPRShader](https://github.com/stalomeow/StarRailNPRShader/blob/main/README.md)|Star Rail風NPR。顔SDF・髪の陰影候補。専用テクスチャとモデル規約を一般材質へ無条件適用しない。|
|36|[poiyomi/PoiyomiToonShader](https://github.com/poiyomi/PoiyomiToonShader/blob/master/README.md)|VRChat/Built-in Toon。多灯・リム・材質表現を参考。多数variantによるcompile/容量増を避ける。|
|37|[Hoyotoon/HoyoToon](https://github.com/Hoyotoon/HoyoToon/blob/main/README.md)|ゲーム由来資産向け再現shader。専用lightmap/材質規約と資産利用条件に依存。一般Toonの既定経路にはしない。|
|38|[kaze-mio/UnityGenshinToonShader](https://github.com/kaze-mio/UnityGenshinToonShader/blob/main/README.md)|URP Genshin風shader。顔SDF・髪ハイライト等の比較対象。専用mapのチャネル意味を明示する。|
|39|[billtruong003/stylized-toon-world-kit](https://github.com/billtruong003/stylized-toon-world-kit/blob/main/README.md)|Unity6向けstylized shader群。環境表現・低負荷variantの参考。SRP固有処理を直接移植しない。|
|40|[NguyenDucThuan2209/AnimeShader](https://github.com/NguyenDucThuan2209/AnimeShader/blob/master/README.md)|学習用URP Toonの短い実装。多灯・影・輪郭を既存Toonと比較。製品版NiloToonを含むとは扱わない。|
|41|[guillaumeblanc/ozz-animation](https://github.com/guillaumeblanc/ozz-animation/blob/master/README.md)|SIMD骨格アニメーション。既存VMD/IK/morph/DQSを保持し、汎用clip samplingのバッチ化候補。|
|42|[acl](https://github.com/nfrechette/acl/tree/3ee568542eca4428e1041908b4a644b98e7885bd)|固定ソースとRTM/ライセンス・raw sample/loop/誤差契約を追加調査。実FBX3clipで現行整数キー選択と時刻補間の差を測定。圧縮誤差と再生方式の差を分ける必要。ACL自体は未ビルド。[調査と比較](rendering-technology-acl-source-audit-2026-10-31.md)。|
|43|[skiriushichev/eely](https://github.com/skiriushichev/eely/blob/master/README.md)|engine-agnostic骨格/グラフ。既存timelineとpose cacheがあるため全置換せず機能差を抽出。|
|44|[thecomet/ik](https://github.com/thecomet/ik/blob/master/README.md)|FABRIKと1/2-bone IK。既存Humanoid IKとの精度・収束を比較。MMDのIK順序・可動域を壊さない。|
|45|[chengkehan/GPUSkinning](https://github.com/chengkehan/GPUSkinning/blob/master/README.md)|Unity GPU skinning。既にcompute skinningがある。crowd向けbakeと共有poseを別候補とする。|
|46|[NotteDiLunaPiena/GPUAnimationDemo](https://github.com/NotteDiLunaPiena/GPUAnimationDemo/blob/main/README.md)|DX11 GPUアニメーション比較例。既存GPU skinning/TEO cacheを維持し、bake・instancingの実測参考。|
|47|[ebertolazzi/Splines](https://github.com/ebertolazzi/Splines/blob/master/README.md)|多種スプライン。既存timeline曲線の用途と比較。大きなライブラリの全面追加を避ける。|
|48|[chen0040/cpp-spline](https://github.com/chen0040/cpp-spline/blob/master/README.md)|Bezier/B-spline/Catmull-Rom。timeline補間の不足を確認して限定使用。VMD独自Bezierを置換しない。|
|49|[WangSimiao2000/MiaoAnimEngine](https://github.com/WangSimiao2000/MiaoAnimEngine/blob/main/README.md)|軽量animation/scene/timeline。既存timeline/sceneとの重複を確認し、バッチ評価やグラフ構造の参考。|
|50|[Melell/Animation-Engine](https://github.com/Melell/Animation-Engine/blob/main/README.md)|animation blending/IK/clothの学習エンジン。OpenGL依存を持ち込まず個別手法へ分解。|
|51|[spnda/fastgltf](https://github.com/spnda/fastgltf/blob/main/README.md)|高速glTF 2.0読み込み。AssimpのglTF部分だけを選択式で比較。extension/座標系/skin/morphを一致させる。|
|52|[ufbx/ufbx](https://github.com/ufbx/ufbx/blob/main/README.md)|単一ファイルFBX loader。FBX限定のimport候補。Assimpとbind pose・単位・pivot・アニメーションを比較。|
|53|[microsoft/DirectStorage](https://github.com/microsoft/DirectStorage/blob/main/README.md)|高速非同期IO/GPU展開。VG/texture streamingに候補。NVMe能力・圧縮方式・キャンセル・Fence所有権が前提。|
|54|[zstd](https://github.com/meta/zstd/tree/f8745da6ff1ad1e7bab384bd1f9d742439278e99)|承認済みの静的統合を実装。v1読込維持、縮む新規保存だけv2。固定3,207件の比較でヘッダー込み125.46→84.97MB（32.27%減）、byte完全一致。warm Load合計727→913msへ増加。Debug全体/描画回帰PASS、5枚×2経路RGB完全一致。[測定と制約](rendering-technology-zstd-validation.md)。|
|55|[tinyobjloader/tinyobjloader](https://github.com/tinyobjloader/tinyobjloader/blob/release/README.md)|OBJ loader。OBJ専用の軽量import候補。material/triangulation/頂点属性をAssimpと比較。|
|56|[google/draco](https://github.com/google/draco/blob/main/README.md)|mesh圧縮。既存meshoptimizer/VG量子化と競合。glTF拡張入力とoffline配送用に限定して容量/誤差を比較。|
|57|[basis_universal](https://github.com/BinomialLLC/basis_universal/blob/073eb6a0a34a23d77e75e54f41dcd9653a52da83/transcoder/basisu_transcoder.h)|公開APIを追加調査。BC5第二成分のAlpha契約と既存RG法線の違いを確認。DDS65件122.1MBを棚卸し、実変換・画質/速度比較は未実施。[調査と制約](rendering-technology-ktx-basis-source-audit-2026-10-31.md)。|
|58|[KTX-Software](https://github.com/KhronosGroup/KTX-Software/blob/00d67536657e0e1563d5ed94772c7bd1556b917b/lib/src/basis_transcode.cpp)|固定版のDFD/channel、ライセンス例外、mip別zstd復元を確認。既存65DDS/441mipの可逆圧縮比較ではBC本体122.10→44.18MB、byte完全一致。KTX2自体は未生成・未導入。[比較と制約](rendering-technology-texture-storage-probe.md)。|
|59|[jkuhlmann/cgltf](https://github.com/jkuhlmann/cgltf/blob/master/README.md)|軽量glTF loader。fastgltfとの二重実装を避け、速度・extension・依存量でどちらかを選ぶ。|
|60|[ibireme/yyjson](https://github.com/ibireme/yyjson/blob/master/README.md)|高速JSON。既存scene/asset JSONの計測後に判断。型・エラー位置・保存互換性を維持。|
|61|[iryoku/smaa](https://github.com/iryoku/smaa/blob/master/README.md)|SMAA。native解像度・非temporalのAA候補。TSR/DLSSに常時重ねず選択式で比較。|
|62|[GameTechDev/CMAA2](https://github.com/GameTechDev/CMAA2/blob/master/README.md)|CMAA2。native解像度AA候補だがメンテナンス終了の明記あり。SMAA/既存FXAAと比較して優先度を決める。|
|63|[microsoft/DirectX-Graphics-Samples](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/README.md)|DX12/MiniEngineの一次資料。DXR・async・streamingの実装契約や検証参考。サンプル一括取り込みはしない。|
|64|[gokselgoktas/temporal-anti-aliasing](https://github.com/gokselgoktas/temporal-anti-aliasing/blob/master/README.md)|TAAの研究実装。既存TSRのclipping/reprojectionと比較。velocityなしの単純履歴へ退行させない。|
|65|[Gaukler/PlainRenderer](https://github.com/Gaukler/PlainRenderer/blob/master/README.md)|Vulkan学習renderer。TAA/SDF等の参考。既存D3D12 backend置換はしない。|
|66|[GPUOpen-Effects/FidelityFX-FSR2](https://github.com/GPUOpen-Effects/FidelityFX-FSR2/blob/master/README.md)|FSR2 temporal SR。既存TSR/DLSSの代替候補。FidelityFX SDKと重複するため一本化し、reactive/motionを検証。|
|67|[Unity-Technologies/PostProcessing](https://github.com/Unity-Technologies/PostProcessing/blob/v2/README.md)|旧PostProcessing。開発はGraphicsへ移動。既存効果と順序を照合し、古いstackを新依存として追加しない。|
|68|[ArieLeo/FSR2Unity](https://github.com/ArieLeo/FSR2Unity/blob/master/README.md)|Unity向けFSR2 wrapper。reactive maskやmotionの実装参考。C++側は公式FSR SDKを利用候補とする。|
|69|[MikuMikuShaders/FXAA](https://github.com/MikuMikuShaders/FXAA/blob/master/README.md)|MMDの軽量FXAA。空間AA候補だがtemporal aliasは解決しない。既存FXAA/TSRと比較。|
|70|[AndrewSaraev/STAA](https://github.com/AndrewSaraev/STAA/blob/master/README.md)|色のみを使うSTAA。motion対応済みTSRの主経路を置換しない。低機能環境の比較参考。|
|71|[Delt06/toon-rp](https://github.com/Delt06/toon-rp/blob/master/README.md)|Toon専用SRP。tiled lighting・影の設計参考。既存多灯Toonを維持。|
|72|[blurfeng/unity-urp-outline](https://github.com/blurfeng/unity-urp-outline/blob/main/README.md)|UV/convolution式のURP輪郭。既存TEO/Extrudeとの補完を比較。細線・奥行き境界・膨張費用を検証。|
|73|[FeralPug/URP-Outline-Render-Feature](https://github.com/FeralPug/URP-Outline-Render-Feature/blob/main/README.md)|Jump Flood輪郭。太い選択輪郭候補。既存オブジェクト輪郭と区別し、全画面pass追加費用を測定。|
|74|[Robinseibold/Unity-URP-Outlines](https://github.com/Robinseibold/Unity-URP-Outlines/blob/main/README.md)|深度/法線screen-space輪郭。既存輪郭の補助候補。材質境界・遠景・遮蔽を比較。|
|75|[Delt06/urp-toon-shader](https://github.com/Delt06/urp-toon-shader/blob/master/README.md)|保守停止でtoon-rpへ移行の明記。新依存にせず手法の参考。|
|76|[Iosif2510/SimpleMeshOutline](https://github.com/Iosif2510/SimpleMeshOutline/blob/main/README.md)|Inverted hull輪郭。既にExtrude/TEO/Mixあり。透明stencilやsmooth normalsの不足のみ比較。|
|77|[CristianQiu/Unity-URP-Outline](https://github.com/CristianQiu/Unity-URP-Outline/blob/main/README.md)|Unity6のlayer別outline。既存editor選択表示の改善候補。最大layer数や全画面費用を確認。|
|78|[MirzaBeig/Post-Processing-Wireframe-Outlines](https://github.com/MirzaBeig/Post-Processing-Wireframe-Outlines/blob/main/README.md)|深度ベースwireframe/outline。検査表示の候補。内部の深度不連続が不要な線を生まないか比較。|
|79|[Unity-Technologies/UniversalRenderingExamples](https://github.com/Unity-Technologies/UniversalRenderingExamples/blob/master/README.md)|保守終了・旧URP向け。歴史的renderer構造の参考のみ。新機能の基盤にはしない。|
|80|[increasinglyHuman/babylon-thin-instance-outline](https://github.com/increasinglyHuman/babylon-thin-instance-outline/blob/main/README.md)|Babylon薄instanceのinverted hull。既存GPU Driven輪郭のinstance属性・選択単位の参考。|
|81|[adobe/openpbr-bsdf](https://github.com/adobe/openpbr-bsdf/blob/main/README.md)|OpenPBRの多層BSDF。reference tracerや材質相互運用候補。全Realtime経路でlobesを増やす前にコスト・保存仕様を設計。|
|82|[AcademySoftwareFoundation/MaterialX](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/README.md)|MaterialXの材質交換。既存Material Assetと将来graphの中間表現候補。import/exportとruntime evaluationを分ける。|
|83|[MircoWerner/ReSTIR-SSS](https://github.com/MircoWerner/ReSTIR-SSS/blob/main/README.md)|ReSTIR SSS研究実装。PBR肌の高品質候補。専用sampling・reservoir・参照画像とGPU予算が必要。|
|84|[mstone3201/RIN](https://github.com/mstone3201/RIN/blob/main/README.md)|DX12 PBR renderer。clearcoat/sheenとclustered lightingの比較。既存機能を再実装しない。|
|85|[DarkBlade909/DarkBladeSkinShader](https://github.com/DarkBlade909/DarkBladeSkinShader/blob/main/README.md)|preintegrated SSS/dual-lobe/specular AA。幾何スペキュラAAをFilamentと比較し、SSSは独立材質として後続評価。|
|86|[CJT-Jackton/URP-Iridescence-Example](https://github.com/CJT-Jackton/URP-Iridescence-Example/blob/master/README.md)|薄膜iridescenceのURP例。PBR材質拡張候補。厚み・IOR・色空間を明示し、専用mapは任意にする。|
|87|[DerSchmale/threejs-thin-film-iridescence](https://github.com/DerSchmale/threejs-thin-film-iridescence/blob/master/README.md)|薄膜Fresnel LUT。ray/raster共通の低負荷iridescence候補。物理参照と角度依存を比較。|
|88|[nvpro-samples/vk_order_independent_transparency](https://github.com/nvpro-samples/vk_order_independent_transparency/blob/main/README.md)|7種類のVulkan OIT比較。既存weighted OITとexact方式の品質/メモリtradeoffを判断する参照。|
|89|[mhalber/Weighted-Blended-OIT](https://github.com/mhalber/Weighted-Blended-OIT/blob/master/README.md)|Weighted blended OIT。既に同系統の経路あり。opaque depth・revealage・極端alpha・色飽和を比較。|
|90|[parker-ford/PBR-Material-Editor](https://github.com/parker-ford/PBR-Material-Editor/blob/master/README.md)|PBR材質編集の実験。エディターの材質比較/preview候補。実装・資産の利用条件確認が必要。|
|91|[rubenaryo/Cumulus](https://github.com/rubenaryo/Cumulus/blob/master/README.md)|DX12のinteractive雲。既存雲raymarch/noise/temporal再構成に密度階層・interactionを追加する候補。|
|92|[konai123/AtmosphereRender](https://github.com/konai123/AtmosphereRender/blob/main/README.md)|MiniEngineのBruneton大気/雲。既存AtmosphereLutとcloud lightingの一致比較用。|
|93|[elliahu/atmosphere](https://github.com/elliahu/atmosphere/blob/main/README.md)|大気・雲・god raysの統合例。GPL-3.0表示あり、取り込み前に適合確認。公開性能を本プロジェクトの実測に転用しない。|
|94|[ebruneton/precomputed_atmospheric_scattering](https://github.com/ebruneton/precomputed_atmospheric_scattering/blob/master/README.md)|Brunetonの一次実装。既存LUTの物理量・高度境界・太陽透過を検証し、不足した多重散乱を比較。|
|95|[supercoderrrrr/VolumetricClouds-URP](https://github.com/supercoderrrrr/VolumetricClouds-URP/blob/main/README.md)|URP雲のGPU noise/temporal。既存GPU noiseと重複。空・地形資産は独立である旨を確認し、履歴処理だけ比較。|
|96|[Depersonalizc/volumetric-clouds](https://github.com/Depersonalizc/volumetric-clouds/blob/main/README.md)|OpenGL雲raymarch。既存雲のサンプリング・light march・early exitの比較参考。|
|97|[mxcop/vapor](https://github.com/mxcop/vapor/blob/main/readme.md)|UE5.6向け開発途中の雲plugin。Nubis系構造の参考。UE依存コードをD3D12エンジンへ直接追加しない。|
|98|[steaklive/EveryRay-Rendering-Engine](https://github.com/steaklive/EveryRay-Rendering-Engine/blob/master/README.md)|DX11/DX12多機能renderer。既存SSR/AO/雲等との比較。旧engine全体ではなく測定対象のpassを絞る。|
|99|[mateeeeeee/Adria](https://github.com/mateeeeeee/Adria/blob/master/README.md)|現行cross-platform renderer。DDGI/RT/AO/雲/oceanの比較参考。D3D12実装との不足機能を個別評価。|
|100|[timfox/idTech3](https://github.com/timfox/idTech3/blob/main/README.md)|modernized idTech3/Vulkan。PBR/RTと互換性維持の参考。GPL由来を含むためコード導入条件を先に確認。|

## 導入順序案

1. シェーダー容量: 未使用DXIL情報を落とせる箇所を特定。反射API、debug出力、PSO作成の依存を確認し、実行DXILと描画結果を比較。
2. PBRの幾何スペキュラAA: Filamentの同手法を一次コードまで確認済み。既存DFG/LTCを維持し、法線変化に応じた粗さfilterを任意機能として評価。Visibility Computeでは解析的に再構築した同一三角形の法線差分を用い、computeのddxや隣接三角形の法線を使わない。
3. cache/asset容量: ACL、zstd、KTX2/Basis、fastgltf/ufbxの小規模実資産比較。まず既存形式との読込互換と誤差を固定する。
4. 光線とsampling: STBN対R2/R4、RTXDIの再利用方式、SHaRC/SSSR/CACAOを一項目ずつ比較。既存NRD入力契約を保つ。
5. 新材質・表現: face SDF、髪の接線ハイライト、PBR肌SSS、thin-film、OpenPBRを独立した材質として設計。
6. 大規模基盤: DirectStorage/RTXTS、D3D12MAの永続割当、OMM、RTXCR等。対応GPU/API、データcook、Fence、VRAM予算を確認した後に接続。

## 共通の採用条件

同じscene/camera/seed/frame数/解像度/露光で旧経路と比較する。CPU/GPU時間、1% low、VRAM、保存容量、decode時間、画質と時間方向の安定性をそれぞれ記録する。容量やGPUパス単体の改善だけで総合改善を断定しない。Debug D3D12検証とRelease性能評価を分ける。失敗時は既存経路へ戻し、対応外機能を有効にしない。

## この調査で未完了のこと

一次調査の時点では製品コードの変更、全ソースの詳細監査、SDK導入、ビルド、GPU実行、画質・性能測定は未実施。以後の初回A/B実装と実際の確認結果は [実装・検証記録](rendering-technology-integration-validation.md) を参照。一次調査だけでユーザーの統合目標全体を完了扱いにしない。

## 調査記録の検証

元の目標ファイルとJSONのURL集合一致、重複なし100件、全件のREADME取得記録・branch・評価・参照URL、Markdown表100行をPowerShellで確認し合格。これは調査資料の完全性の検査であり、SDK互換性や実装の動作検証ではない。

