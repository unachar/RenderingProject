#pragma once
#include "materialasset.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

enum class MaterialGraphType
{
    Scalar = 1,
    Vector2 = 2,
    Vector3 = 3,
    Vector4 = 4,
    Numeric = 0
};
enum class MaterialNodeKind
{
    Constant,
    ScalarParameter,
    VectorParameter,
    ColorParameter,
    TextureSample,
    Add,
    Subtract,
    Multiply,
    Divide,
    Lerp,
    Clamp,
    Smoothstep,
    Normalize,
    Dot,
    Fresnel,
    UV,
    WorldPosition,
    WorldNormal,
    CameraVector,
    Time,
    Output,
    Group,
    // ---- 以下は UE のマテリアルノードに合わせて追加（シリアライズは名前で行うので末尾に追加する）
    Constant2Vector,
    Constant3Vector,
    Constant4Vector,
    TextureCoordinate,
    Panner,
    Rotator,
    // 単項演算（Abs〜DDY は連続していること）
    Abs,
    Ceil,
    Floor,
    Frac,
    Round,
    Truncate,
    Sign,
    OneMinus,
    Saturate,
    SquareRoot,
    Sine,
    Cosine,
    Tangent,
    Arcsine,
    Arccosine,
    Arctangent,
    Exponential,
    Logarithm2,
    DDX,
    DDY,
    // 二項演算（Power〜Arctangent2 は連続していること）
    Power,
    Min,
    Max,
    Fmod,
    Step,
    Arctangent2,
    ConstantBiasScale,
    CrossProduct,
    Distance,
    Length,
    AppendVector,
    ComponentMask,
    ReflectionVector,
    Desaturation,
    If,
    SphereMask,
    CheapContrast,
    Noise,
    Reroute,
    // ---- 2026-11 追加（UE のマテリアルノード / マテリアル関数）。シリアライズは名前なので末尾に追加する
    StaticSwitch,
    Pi,
    RemapValueRange,
    Posterize,
    HueShift,
    Blend,
    LinearGradient,
    RadialGradient,
    Voronoi,
    NormalFromHeight,
    BlendAngleCorrectedNormals,
    FlattenNormal,
    BumpOffset,
    RotateAboutAxis,
    BlackBody,
    // ---- プロシージャルスタジオのノイズ（ProcStudio::FractalNoise / TextureNoise::Noise と同じ計算）
    ProceduralNoise, // 単体ノイズ（フラクタルなし）。Value = {基底, シード, タイル, 0}
    FractalNoise,    // フラクタルノイズ。Value = {基底, フラクタル種別, シード, タイル}
    NoiseCombine,    // 2 つのノイズの合成。Value = {合成モード, 0..1 に収める, 0, 0}
    NoiseLayers,     // 最大 4 層の重み付き平均（スタジオの Composite と同じ）
    // ---- テクスチャ（色画像 → 高さ / 法線。プロシージャルスタジオの「画像から高さ・法線」と同じ式）
    ConvertToNormalHeight, // 旧（1 ノードで切り替え）。読み込み互換のためだけに残す（追加メニューには出さない）
    ConvertToHeight,       // 色画像 → 高さ（Scalar）。Value = {0, チャンネル, 反転, 0}
    ConvertToNormal,       // 色画像 → 接空間法線（Vector3）。Value = {1, チャンネル, 反転, Sobel}
    BreakVector,           // ベクトルを X / Y / Z / W に分ける（UE の BreakOutFloat4Components）。出力は常に分割
    ConvertToTexture,      // 入力（ノイズなど）をテクスチャに焼いて使う。Value = {サンプラー, 解像度, 0, 0}
                           // Texture が空のあいだは入力をそのまま通す
    // ---- ソリッドテクスチャ（プロシージャルスタジオの立体テクスチャ。3D 座標で評価する）
    SolidTexture,    // 色（Vector3）。Value = {種類, 基底, フラクタル, シード}
    SolidPattern,    // 値 0〜1（Scalar）。Value = {種類, 基底, フラクタル, シード}
    SolidCoordinate, // 立体テクスチャ用の 0〜1 座標（Vector3）= (Position * 0.5 + 0.5) * Scale + Offset
    ThreeColorRamp,  // 値 → A / B / C の 3 色ランプ（スタジオの配色と同じ）
    // 普通のテクスチャ / ノイズ（2D）からソリッドテクスチャ（3D）を作る。Texture = 元画像（ドロップ / Value から作成）
    // Value = {方式（MaterialSolidConvertMethod）, サンプラー（Color / Linear）, シード, グレースケール}
    ConvertToSolid,
    // ---- マテリアルシェーディング用の便利ノード（2026-11）
    LocalPosition,    // 表面の点のローカル（オブジェクト）空間の位置（UE の Local Position）。旧名 Object Position
    VertexColor,      // 頂点カラー（RGBA）
    CameraDistance,   // カメラからの距離（ワールド単位）
    SlopeMask,        // 面の向き（法線と Up の角度）のマスク。雪・苔・砂の積もり
    HeightLerp,       // 高さで 2 つを混ぜる（UE の HeightLerp）
    ColorAdjust,      // 明るさ / コントラスト / 彩度
    TriplanarTexture, // テクスチャを 3 方向から投影（UV 不要、継ぎ目なし）。Value = {サンプラー, 0, 0, 0}
    // ---- オブジェクト / カメラの情報（UE の Object / Coordinates ノード）
    ObjectPosition,    // オブジェクト原点のワールド座標（オブジェクトごとに 1 つ）
    ObjectScale,       // ワールドでのスケール（XYZ）
    ObjectOrientation, // オブジェクトのローカル +Y（上）方向（ワールド）
    ObjectRadius,      // 範囲の半径（ワールド）
    ObjectBounds,      // ローカルの範囲。Value[0] = 出力：0 大きさ / 1 中心 / 2 最小 / 3 最大
    BoundingBoxUVW,    // 範囲内の 0〜1 座標（ローカル。UE の BoundingBoxBased_0-1_UVW）
    PerInstanceRandom, // オブジェクトごとに固定の 0〜1 の乱数
    TransformVector,   // 方向の変換。Value[0] = 0 ローカル→ワールド / 1 ワールド→ローカル / 2 ワールド→ビュー / 3 ビュー→ワールド
    TransformPosition, // 位置の変換。Value[0] = 0 ローカル→ワールド / 1 ワールド→ローカル
    CameraPosition,    // カメラのワールド座標
    PixelDepth,        // カメラからの奥行き（ビュー空間の Z）
    ScreenPosition,    // 画面上の 0〜1 座標
    DitherOpacity,     // ディザ（4x4）で不透明度を 0 / 1 にする（Masked のフェード）
    DistanceFade,      // カメラ距離で 0（Start）→ 1（End）
    HeightGradient,    // 高さで 0（Start）→ 1（End）。Value[0] = 0 ワールド Y / 1 ローカル Y / 2 範囲内 0〜1
    // ---- 2026-11 追加：マテリアルノード 33 種 + Function 用の入出力 2 種（シリアライズは名前なので末尾に追加する）
    //      各ノードのパラメーターの意味は materialgraphext.h の MaterialGraphExt を参照
    // A. トゥーン / キャラクター
    ToonLightRamp,            // ライト方向の段階ランプ（ライトごとの評価は ToonNodeLighting.hlsli）
    FaceSdfShadow,            // 顔専用の SDF 陰影
    HairAnisotropicHighlight, // 髪の異方性ハイライト（Kajiya-Kay）
    DirectionalRimLight,      // 光源方向を考慮したリムライト（既存のリムと二重にならない）
    MatCap,                   // MatCap テクスチャ
    MekakureMask,             // 既存メカクレ処理に渡すマスク
    MultiColorRamp,           // 4〜16 キーのカラーランプ（Three Color Ramp は互換のまま残る）
    EyeHighlight,             // アニメ調の瞳ハイライト
    MaterialFunctionCall,     // Material Function 呼び出し（コンパイル時にインライン展開する）
    TextureParameter2D,       // マテリアルインスタンスで差し替えられるテクスチャ
    // B. 汎用 / PBR
    TwoSidedSign,             // 表 +1 / 裏 -1
    FlipbookUV,               // スプライトシートの UV
    FlowMap,                  // フローマップによる UV 移動（2 位相）
    RgbCurves,                // RGB 独立カーブ
    HsvAdjust,                // Hue / Saturation / Value
    PackedTextureChannels,    // ORM などのチャンネル分離（R / G / B / A の 4 出力）
    DetailNormalBlend,        // ベース法線 + ディテール法線
    TriplanarNormalCorrected, // Triplanar 法線（接空間へ正しく変換）
    CurvatureMask,            // 曲率マスク
    MaterialLayerBlend,       // マテリアル属性のレイヤー合成（8 出力）
    ThinFilmInterference,     // 薄膜干渉
    AnisotropyDirection,      // 異方性の方向
    ClearCoatControl,         // クリアコートの強度 / 粗さ / マスク
    SubsurfaceColor,          // 内部散乱色
    ThicknessMap,             // 厚みマップ（Thickness / Transmission の 2 出力）
    // C. エフェクト / ライブ
    SceneDepth,               // シーン深度
    DepthFade,                // ジオメトリ交差のフェード
    SceneColor,               // シーンカラー
    CustomHlsl,               // ユーザー定義の HLSL 式
    DynamicSwitch,            // 実行時条件の切り替え
    AudioSpectrum,            // 音声スペクトル
    BeatPhase,                // BPM 同期（4 出力）
    CurveAtlas,               // カーブの参照
    // Material Function の入出力（関数グラフの中だけで使う）
    FunctionInput,
    FunctionOutput
};

// 追加ノードの個数（IMPLEMENTATION_STATUS.md と tests が数える）
inline constexpr int kMaterialExtNodeCount = 33;

// Convert To Solid の方式（node.Value[0]）
enum class MaterialSolidConvertMethod : int
{
    Stochastic = 0, // 3 方向投影 + ヘックス タイル（タイルごとにランダムな位置 / 回転で混ぜる）：繰り返し・縞が出ない（既定）
    Triplanar = 1,  // 3 方向（YZ / XZ / XY）から投影し、法線で混ぜる（元画像の見た目を保つ。拡大すると繰り返す）
    Volume = 2,     // スタジオの「画像からボリューム」と同じ（互換用。拡大すると繰り返し、面によって縞が出る）
    Count
};
// 旧ファイル（solidMethods < 2）の値 0 は Volume だったが、繰り返しの問題があるため Stochastic として読む

// ソリッドテクスチャの種類（node.Value[0]）。スタジオの IsSolidCapable と同じ 6 種
enum class MaterialSolidKind : int
{
    FractalNoise = 0,
    Marble,
    Wood,
    Granite,
    Voronoi,
    Cloud,
    Count
};

// ノードがテクスチャ（node.Texture）を持つ / 読むか
inline bool IsConvertTextureNode(MaterialNodeKind kind);
inline bool UsesNodeTexture(MaterialNodeKind kind)
{
    return kind == MaterialNodeKind::TextureSample || kind == MaterialNodeKind::ConvertToTexture ||
           kind == MaterialNodeKind::ConvertToSolid || kind == MaterialNodeKind::TriplanarTexture ||
           IsConvertTextureNode(kind);
}

// Convert ノードか（新旧どちらも）
inline bool IsConvertTextureNode(MaterialNodeKind kind)
{
    return kind == MaterialNodeKind::ConvertToNormalHeight || kind == MaterialNodeKind::ConvertToHeight ||
           kind == MaterialNodeKind::ConvertToNormal;
}

// Convert To Normal / Height の出力（node.Value[0]）
enum class MaterialConvertOutput : int
{
    Height = 0, // Scalar（POM の Height へ）
    Normal = 1  // Vector3 接空間法線（Normal へ）
};

// Noise Combine の合成モード（node.Value[0]）
enum class MaterialNoiseCombineMode : int
{
    Mix,
    Add,
    Subtract,
    Multiply,
    Screen,
    Overlay,
    Min,
    Max,
    Difference,
    HeightBlend,
    Count
};

// マテリアルモード（UE の Blend Mode + 屈折に相当）。Material Output の接続できるピンが変わる
enum class MaterialGraphMode : int
{
    Opaque = 0,
    Masked = 1,
    Translucent = 2,
    Glass = 3, // Translucent + 屈折 / 透過 / 吸収
    Count
};

// Material Output の入力ピン（順番はシリアライズ済みグラフのピン順なので変更しない。追加は末尾）
enum class MaterialOutputPin : size_t
{
    BaseColor,
    Metallic,
    Roughness,
    Normal,
    Emissive,
    Opacity,
    OpacityMask,
    AO,
    Height,          // POM の高さ（白 = 元の面、黒 = 最深部）
    NormalIntensity, // 法線マップの強さ（NormalScale）
    ParallaxDepth,   // POM の深さ（UV 単位）
    ParallaxSteps,   // POM の最大ステップ数
    IOR,
    Transmission,
    TransmissionRoughness,
    Thickness,
    Absorption,
    ShadingStrength,
    // ---- 2026-11 追加（既存のピン番号 / 順序は変えない。追加は末尾）。描画時評価（GPU）だけが使うピン
    ToonShade,       // Toon Light Ramp の出力を繋ぐ。ライトごとの陰影ランプとして ToonLighting へ渡る
    ToonRim,         // Directional Rim Light
    ToonHair,        // Hair Anisotropic Highlight
    ToonFaceShadow,  // Face SDF Shadow
    MekakureMaskOut, // Mekakure Mask（既存メカクレのマスク描画が参照する）
    ClearCoat,       // Clear Coat Control（x = 強度, y = 粗さ）
    SubsurfaceTint,  // Subsurface Color
    AnisotropyDir,   // Anisotropy Direction（xyz = 方向, w = 異方性）
    Count
};

// Texture Sample のサンプラー種別（UE の Sampler Type）。node.Value[0] に入れる
enum class MaterialSamplerType : int
{
    Color = 0,  // sRGB → 線形
    Normal = 1, // 格納値 * 2 - 1（BC5 の Z 再構成込み）
    Linear = 2  // 格納値そのまま（Masks / Linear Color）
};

// Blend ノードのモード（node.Value[0]）
enum class MaterialBlendMode : int
{
    Normal,
    Multiply,
    Screen,
    Overlay,
    SoftLight,
    HardLight,
    ColorDodge,
    ColorBurn,
    LinearDodge,
    LinearBurn,
    Difference,
    Darken,
    Lighten,
    Subtract,
    Count
};

// グラフ全体の設定（UE のマテリアルの Details に相当）。-1 / 負値は「マテリアルアセットの設定を使う」
struct MaterialGraphSettings
{
    int Mode = -1;             // MaterialGraphMode（-1 = 旧グラフ：すべてのピンを使える）
    int Surface = -1;          // 0 Opaque / 1 Masked / 2 Transparent
    int Cull = -1;             // 0 Back / 1 None（両面）/ 2 Front
    int ShadingModel = -1;     // 0 標準 / 1 クリアコート / 2 布 / 3 肌 / 4 髪 / 5 異方性
    int ShadowMode = -1;       // 0 None / 1 Opaque / 2 AlphaTest / 3 Transmittance / 4 Auto
    float AlphaCutoff = -1.0f; // Opacity Mask Clip Value（負値 = UE 既定 0.3333）
    int BakeResolution = 512;  // ピクセルごとの式をテクスチャへ焼くときの解像度
    float BakeTime = 0.0f;     // Time ノードを焼くときの時刻（秒）
    bool AutoBake = true;      // グラフ変更後に自動でテクスチャを焼き直す
    // ライブ更新：Time を使う式を、実行中に焼いたテクスチャへ連続で書き込む（マテリアルが動く）
    bool LiveUpdate = true;
    int LiveResolution = 256; // ライブ更新の解像度（ベイク解像度とは別。CPU 負荷とのトレードオフ）
    float LiveRate = 30.0f;   // 1 秒あたりの更新回数（上限）
    // 描画時評価（GPU）：グラフを GPU プログラムにして、エンジンのシェーダーが画素ごとに 3D 位置で評価する。
    // ソリッドテクスチャ / ノイズが UV に依存せず継ぎ目なく貼られる。対応外のノードがあるとベイクに戻る
    bool RuntimeEvaluation = true;
    int SolidSpace = 0; // ソリッドテクスチャ / ノイズの既定の座標：0 オブジェクト空間 / 1 ワールド空間
    // Material Function（関数グラフ）：Material Output を持たず、Function Input / Function Output で入出力を定義する
    bool IsFunction = false;
    bool operator==(const MaterialGraphSettings&) const = default;
};

struct MaterialGraphInput
{
    const char* Name;
    MaterialGraphType Type;
    std::array<float, 4> Default{};
};

struct MaterialNodeDefinition
{
    MaterialNodeKind Kind;
    const char* Name;
    MaterialGraphType OutputType;
    std::vector<MaterialGraphInput> Inputs;
};

struct MaterialGraphNode
{
    uint64_t Id = 0;
    MaterialNodeKind Kind = MaterialNodeKind::Constant;
    std::string Name;
    std::array<float, 4> Value{};
    AssetHandle Texture = 0;
    std::vector<uint64_t> InputPins;
    uint64_t OutputPin = 0;
    std::array<float, 2> Position{};
    std::array<float, 2> GroupSize{400.0f, 250.0f};
    float Width = 0.0f;  // ノード本体の幅（0 = 既定）。右端 / 右下のグリップで変更する
    float Height = 0.0f; // ノード本体の高さ（0 = 内容に合わせる）。下端 / 右下のグリップで変更する
    std::string Comment; // ノードの上に表示するコメント（UE のノードコメント）
    // 出力ピンの分割（UE の Split Struct Pin）。空 = 分割なし。要素 = X / Y / Z / W の出力ピン
    // 分割中は OutputPin（まとめた出力）は表示されず、接続もできない
    std::vector<uint64_t> SplitPins;
    // 未接続の入力ピンの値（UE と同じく、ピンの横のボックスで編集する）。添字 = 入力ピン。NaN = 定義の既定値
    std::vector<float> PinValues;
    // 未接続のベクトル入力ピンの直値（Material Output の Base Color / Normal / Emissive / Absorption）。
    // 添字 = 入力ピン。[0] が NaN = 未設定（定義の既定値 / マテリアルアセットの値を使う）
    std::vector<std::array<float, 4>> PinVectors;
    // ---- 2026-11 追加（旧ファイルには無い。無ければ空 = 既定値）
    std::vector<float> Params;           // 可変長のパラメーター（ランプのキー / カーブの制御点 / 関数のシグネチャなど）
    std::string Text;                    // 文字列（Texture Parameter の名前 / Custom HLSL の式 / 関数ピンの名前）
    std::vector<std::string> PinNames;   // ユーザー定義のピン名（Custom HLSL の入力 / Function Input・Output）
    AssetHandle Function = 0;            // Material Function のアセット

    // pin がこのノードの出力（まとめた出力 / 分割した成分）か
    bool OwnsOutput(uint64_t pin) const
    {
        if (pin == 0)
        {
            return false;
        }
        if (pin == OutputPin)
        {
            return true;
        }
        for (const auto split : SplitPins)
        {
            if (split == pin)
            {
                return true;
            }
        }
        return false;
    }
};

struct MaterialGraphLink
{
    uint64_t Id = 0;
    uint64_t SourcePin = 0;
    uint64_t TargetPin = 0;
};

// This is the material document, independent of ImGui/node-editor and GPU resources.
// The UI keeps undo snapshots of this value; shader jobs consume immutable copies.
class MaterialGraph
{
  public:
    std::vector<MaterialGraphNode> Nodes;
    std::vector<MaterialGraphLink> Links;
    MaterialGraphSettings Settings;
    uint64_t Revision = 0;
    uint64_t ParameterRevision = 0;

    static std::span<const MaterialNodeDefinition> Definitions();
    static const MaterialNodeDefinition& Definition(MaterialNodeKind kind);
    static MaterialGraph FromMaterial(const MaterialAssetData& material);
    // Material Function 用の空のグラフ（Material Output を持たない）
    static MaterialGraph NewFunction();
    // ---- 複数出力（固定の出力スロット）。Break Vector と同じく、スロットは SplitPins として常に存在する
    struct OutputSlot
    {
        const char* Name;
        MaterialGraphType Type;
    };
    // 固定スロットを持つノードのスロット表（持たなければ空）。Break Vector は従来どおり別扱い
    static std::span<const OutputSlot> FixedOutputSlots(MaterialNodeKind kind);
    static bool HasFixedOutputs(MaterialNodeKind kind);
    // 呼び出し側が登録するマテリアルインスタンス側の上書き（Texture Parameter 2D）。保存しない実行時の値
    std::vector<std::pair<std::string, AssetHandle>> InstanceTextures;
    // 出力ピンの型を、ノードのスロット表 / 関数のシグネチャまで含めて返す（分割成分は Scalar）
    MaterialGraphType SlotType(const MaterialGraphNode& node, size_t slot) const;
    // マテリアルアセットの値からマテリアルモードを決める（Transmission > 0 = Glass）
    static MaterialGraphMode ModeFromMaterial(const MaterialAssetData& material);
    // ---- 入力ピン横の数値ボックス（UE と同じ）
    // Scalar / Numeric で、UV / Time / 位置などの特別な既定値が無いピンだけ
    static bool IsPinValueEditable(MaterialNodeKind kind, size_t input);
    // 未接続の入力ピンの値（ボックスで設定した値、無ければ定義の既定値）
    static std::array<float, 4> UnconnectedInputValue(const MaterialGraphNode& node, size_t input);
    static bool HasPinValue(const MaterialGraphNode& node, size_t input);
    // ---- ベクトルの直値（Material Output の色 / 法線ピン）
    static bool IsPinVectorEditable(MaterialNodeKind kind, size_t input);
    static bool HasPinVector(const MaterialGraphNode& node, size_t input);
    // 直値を消す（スカラー / ベクトルとも）。消したら true
    static bool ClearPinOverride(MaterialGraphNode& node, size_t input);
    // Material Output のピンが「未接続だが直値が入っている」か（マテリアルモードで使えないピンは false）
    bool HasOutputDirectValue(const MaterialGraphNode& output, size_t input) const;
    // ---- 定数ノード（Constant / Constant2〜4Vector）の成分ピン
    // 成分ごとに入力ピンを持ち、未接続の成分はノード上の値（node.Value[i]）、接続した成分は入力の値を使う
    static bool IsComponentConstant(MaterialNodeKind kind);
    // 成分ピンのどれかが接続されているか（false = 従来どおりの定数 / パラメーターとして扱う）
    bool HasConnectedComponent(const MaterialGraphNode& node) const;
    // ---- 出力ピンの分割（UE の Split Struct Pin / Recombine）
    // 出力ピンの成分番号：-1 = まとめた出力、0〜3 = X / Y / Z / W
    int OutputComponent(uint64_t pin) const;
    // 出力ピンの型（成分ピンは Scalar）
    MaterialGraphType PinOutputType(uint64_t pin) const;
    bool CanSplitOutput(uint64_t node) const;
    // 分割する（まとめた出力の接続は外れる）。成分数は今の出力の型から決まる
    // forcedCount > 0：成分数を指定して分割する（コピー＆ペーストで、接続前の型に関係なく同じ分割を作る）
    bool SplitOutput(uint64_t node, std::string& error, int forcedCount = 0);
    // 統合する（成分ピンの接続は外れる）。Break Vector は常に分割なので統合できない
    bool RecombineOutput(uint64_t node);
    // マテリアルモード / シェーディングモデルで Material Output のピンが使えるか（UE の灰色ピン）
    static bool IsOutputPinActive(MaterialOutputPin pin, const MaterialGraphSettings& settings);
    bool IsOutputPinActive(size_t index) const;
    // 使えない理由（ツールチップ用）。使えるなら nullptr
    static const char* OutputPinInactiveReason(MaterialOutputPin pin, const MaterialGraphSettings& settings);
    // マテリアルモードを設定する（Blend Mode も合わせる）
    void SetMode(MaterialGraphMode mode);
    uint64_t AddNode(MaterialNodeKind kind, float x, float y);
    bool RemoveNode(uint64_t node);
    bool CanConnect(uint64_t source, uint64_t target, std::string& error) const;
    // エディターでの新規接続用：CanConnect に加えて、マテリアルモードで使えないピンを拒否する
    bool CanConnectActive(uint64_t source, uint64_t target, std::string& error) const;
    bool Connect(uint64_t source, uint64_t target, std::string& error);
    // 検証せずに接続する（Material Function の展開用。呼び出し側が最後に Validate する。既存の接続は置き換える）
    bool ConnectUnchecked(uint64_t source, uint64_t target);
    bool RemoveLink(uint64_t link);
    MaterialGraphNode* FindNode(uint64_t node);
    const MaterialGraphNode* FindNode(uint64_t node) const;
    const MaterialGraphNode* FindPinNode(uint64_t pin, bool* input = nullptr, size_t* index = nullptr) const;
    // 入力ピンに繋がっているリンク。マテリアルモードで使えない Material Output のピンへのリンクは
    // 「無いもの」として扱う（リンク自体は残す：モードを戻すと復活する。UE と同じ）
    const MaterialGraphLink* InputLink(uint64_t pin) const;
    // モードに関係なく、実際に存在するリンク
    const MaterialGraphLink* RawInputLink(uint64_t pin) const;
    // ノード出力の型（Numeric の演算は入力から決まる。Append / Component Mask は成分数から決まる）
    MaterialGraphType ResolveOutputType(uint64_t node) const;
    bool Validate(std::string& error) const;
    std::string Serialize() const;
    static bool Deserialize(const std::string& contents, MaterialGraph& graph, std::string& error);
    bool Save(const std::filesystem::path& path, std::string& error) const;
    static bool Load(const std::filesystem::path& path, MaterialGraph& graph, std::string& error);

  private:
    uint64_t m_NextId = 1;
    bool Reaches(uint64_t startNode, uint64_t targetNode) const;
    MaterialGraphType OutputType(uint64_t node, std::vector<uint64_t>& visiting) const;
    MaterialGraphType SourcePinType(uint64_t pin, std::vector<uint64_t>& visiting) const;
};






