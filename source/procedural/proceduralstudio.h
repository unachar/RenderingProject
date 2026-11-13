#pragma once
#include "noise.h"
// Procedural Studio: フラクタル・プロシージャルテクスチャ、立体テクスチャ、ボリューム、
// モデルの CPU 生成処理。D3D に依存しない C++ とし、単体テストやエディター・クラウドレンダラーで共有する。
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ProcStudio
{
    struct Float2
    {
        float X = 0;
        float Y = 0;
    };

    struct Float3
    {
        float X = 0;
        float Y = 0;
        float Z = 0;
    };

    struct Float4
    {
        float X = 0;
        float Y = 0;
        float Z = 0;
        float W = 1;
    };

    struct Image
    {
        int Width = 0;
        int Height = 0;
        std::vector<Float4> Pixels;            // 線形 RGBA。
        Float4 Sample(float u, float v) const; // 折り返し付き双線形補間。
    };

    struct Volume
    {
        int Extent = 0;
        std::vector<float> Density; // Extent^3 個、X が最速で変化し、値域は [0, 1]。
    };

    enum class TextureKind
    {
        FractalNoise,
        Marble,
        Wood,
        Granite,
        Voronoi,
        Brick,
        Checker,
        FractalWater,
        FractalCloud,
        Mandelbrot,
        Julia,
        Geometric,
        Count
    };
    enum class TextureOutput
    {
        Color,
        Height,
        Normal,
        Count
    };

    enum class GeometricPattern
    {
        Stripes,
        Grid,
        Dots,
        Diamonds,
        Hexagons,
        Triangles,
        Count
    };

    struct TextureParams
    {
        TextureKind Kind = TextureKind::FractalNoise;
        TextureOutput Output = TextureOutput::Color;
        NoiseParams Noise;
        std::array<float, 3> ColorA{0.05f, 0.07f, 0.12f};
        std::array<float, 3> ColorB{0.85f, 0.82f, 0.76f};
        std::array<float, 3> ColorC{0.45f, 0.28f, 0.12f};
        bool Tileable = true;
        float Contrast = 1.0f;
        float Bias = 0.0f;
        // 大理石・木目。
        float Turbulence = 4.0f;
        float RingCount = 8.0f;
        // レンガ・市松模様。
        int BricksX = 4;
        int BricksY = 8;
        float Mortar = 0.06f;
        // 幾何学模様。サイズと境界の幅は、各セルに対する比率。
        GeometricPattern Pattern = GeometricPattern::Stripes;
        int RepeatX = 4;
        int RepeatY = 4;
        float PatternSize = 0.5f;
        float PatternSoftness = 0.02f;
        // 水。
        float Time = 0.0f;
        float Choppiness = 0.6f;
        // 雲。
        float Coverage = 0.5f;
        float Softness = 0.25f;
        // 発散時間型フラクタル。
        float CenterX = -0.5f;
        float CenterY = 0.0f;
        float Zoom = 1.0f;
        float JuliaX = -0.8f;
        float JuliaY = 0.156f;
        int Iterations = 200;
        float NormalStrength = 4.0f;
    };

    const char* TextureKindName(TextureKind kind);
    bool IsSolidCapable(TextureKind kind);
    // 2D の (u, v) または 3D 座標における [0, 1] のスカラー場。高さ・法線の出力と立体テクスチャに使う。
    float TextureScalar(const TextureParams& params, float x, float y, float z, bool solid);
    Float3 TextureColor(const TextureParams& params, float x, float y, float z, bool solid);
    Image GenerateTexture(const TextureParams& params, int width, int height);
    // タイル化可能な 3D 密度。ボリューム雲の基底ノイズなどに使う。
    Volume GenerateVolume(const TextureParams& params, int extent);
    // 2D 画像をずらした平面で複数オクターブにわたりサンプリングし、タイル化可能なボリュームを作る。
    Volume VolumeFromImage(const Image& image, int extent, int octaves, float verticalVariation, uint32_t seed);
    // DDS 書き出し用に立体テクスチャを 3D ボリュームへ焼き込む。ボクセルごとに RGBA を展開する。
    std::vector<Float4> GenerateSolidVolume(const TextureParams& params, int extent);

    struct Mesh
    {
        std::vector<Float3> Positions, Normals, Colors;
        std::vector<Float2> UVs;
        std::vector<uint32_t> Indices;
        void Clear();
        uint32_t AddVertex(Float3 position, Float3 color, Float2 uv = {});
        void ComputeNormals();
        void Bounds(Float3& minimum, Float3& maximum) const;
    };

    enum class ModelKind
    {
        Sphere,
        Cube,
        Plane,
        Terrain,
        FractalTree,
        Rock,
        WaterSurface,
        MengerSponge,
        SierpinskiPyramid,
        FractalLeaf, // 木に使う葉を単独でプレビュー・書き出しする。
        Count
    };

    enum class LeafShape
    {
        Card,    // 従来の交差した四角形（最も低負荷）。
        Simple,  // 湾曲した 1 枚の葉身。
        Pinnate, // フラクタル複葉。葉軸に並ぶ小葉も、再び複葉になる。
        Fern,    // フラクタルのシダ葉。多数の小葉を左右交互に配置する、Barnsley 型に近い強い自己相似構造。
        Palmate, // フラクタル掌状葉。基部から放射状に広がる各裂片も掌状に分岐する。
        Count
    };

    struct LeafParams
    {
        LeafShape Shape = LeafShape::Pinnate;
        int Depth = 2;              // 自己相似な細分化の再帰の深さ。
        int Leaflets = 4;           // 葉軸ごとの小葉の対数（羽状葉・シダ）または裂片の数（掌状葉）。
        float LeafletAngle = 50.0f; // 葉軸と小葉の間の角度（度）。
        float LeafletScale = 0.42f; // 子の長さ / 親の長さ。
        float Taper = 0.6f;         // 先端へ向かう小葉の縮小率（0 は均一）。
        float Width = 0.32f;        // 葉身の幅 / 葉身の長さ。
        float Droop = 0.25f;        // 長さ方向の湾曲（法線の反対方向へ曲げる）。
        float Fold = 0.15f;         // 中央の葉脈を軸とする V 字の折り曲げ。
        int Segments = 6;           // 葉身の長さ方向の分割数。
        float ColorVariation = 0.15f;
        std::array<float, 3> Color{0.16f, 0.42f, 0.12f};
        std::array<float, 3> TipColor{0.42f, 0.55f, 0.15f};
    };

    const char* LeafShapeName(LeafShape shape);
    // 葉のローカル基底。基部は原点、長さは +Y に 1 単位、幅は X 方向、
    // 表面は +Z。表裏は別の頂点を持つ両面構造とし、法線を計算する。
    Mesh GenerateLeaf(const LeafParams& params, uint32_t seed);

    inline constexpr int kMaxTerrainMaps = 4;

    // リアルな地形のための形状・侵食・配色パラメーター。既定値では従来の地形と同じ形になる（従来クランプされていた山頂・谷底のみ滑らかに丸める）。
    struct TerrainShapeParams
    {
        // 形状。
        float MacroWarp = 0.0f;       // 低周波のドメインワープ。尾根や谷を自然に蛇行させる。
        float RidgeBlend = 0.0f;      // リッジノイズとの合成率。鋭い稜線（山脈）を作る。
        float MountainMask = 0.0f;    // 山地と平地の分布マスクの強さ。0 で全域が均一。
        float MaskFrequency = 1.0f;   // 分布マスクの周波数（1区画あたり）。
        float Exponent = 1.0f;        // 高さの再分布。1 より大きいと谷が広く平らに、峰が鋭くなる。
        int TerraceSteps = 0;         // 段丘（台地）の段数。0 で無効。
        float TerraceStrength = 0.6f; // 段丘の強さ。
        float SeaLevel = 0.0f;        // 海面（正規化した高さ）。0 で無効。
        float SeaFlatten = 0.7f;      // 海面より下を平らにする量。
        float EdgeFalloff = 0.0f;     // 外周を下げて島にする幅。0 で無効。
        float MicroDetail = 0.0f;     // 急斜面に加える細かな岩の凹凸。
        // 侵食。
        int ThermalIterations = 0;    // 熱侵食（崩落）の反復回数。
        float TalusAngle = 38.0f;     // 安息角（度）。これより急な斜面が崩れる。
        float HydraulicDensity = 0.0f; // 水滴侵食の水滴数（頂点 1 つあたり）。0 で無効。
        float ErosionRate = 0.3f;
        float DepositionRate = 0.3f;
        float Evaporation = 0.02f;
        float Inertia = 0.05f;
        float SedimentCapacity = 4.0f;
        // 配色（正規化した高さ・傾斜の境界）。
        float SandLine = 0.20f;
        float RockLine = 0.625f;
        float SnowLine = 0.83f;
        float RockSlope = 0.675f;
    };

    enum class TerrainPreset
    {
        Default,
        Mountains,
        Hills,
        Mesa,
        Island,
        Canyon,
        Count
    };

    const char* TerrainPresetName(TerrainPreset preset);
    // 形状・侵食・配色をプリセット値に置き換える（ノイズ設定は変更しない）。
    void ApplyTerrainPreset(TerrainShapeParams& params, TerrainPreset preset);

    struct ModelParams
    {
        ModelKind Kind = ModelKind::FractalTree;
        NoiseParams Noise;
        uint32_t Seed = 7;
        // 地形・水。
        int Resolution = 96;
        float Size = 10.0f;
        float Height = 2.5f;
        bool TerrainColors = true;
        int TerrainMapsX = 1;
        int TerrainMapsZ = 1;
        TerrainShapeParams Terrain;
        // 木。
        int Depth = 6;
        int Branches = 3;
        float TrunkLength = 2.0f;
        float TrunkRadius = 0.14f;
        float LengthRatio = 0.72f;
        float RadiusRatio = 0.62f;
        float SpreadAngle = 32.0f;
        float Tropism = 0.15f;
        float Jitter = 0.25f;
        bool Leaves = true;
        float LeafSize = 0.22f;
        int LeavesPerTip = 2;
        // 木に使う葉の三角形数の上限。収まるよう葉の詳細度（再帰の深さ）を下げる。
        int LeafTriangleBudget = 600000;
        LeafParams Leaf;
        int RadialSegments = 8;
        // 岩。
        int Subdivisions = 4;
        float Displacement = 0.35f;
        // 水。
        float Time = 0.0f;
        float WaveAmplitude = 0.35f;
        // フラクタル立体。
        int Level = 2;
    };

    const char* ModelKindName(ModelKind kind);
    Mesh GenerateModel(const ModelParams& params);
    bool WriteGridObj(const std::filesystem::path& path, const ModelParams& params, bool vertexColors,
                      float uvScale, const std::string& diffuseTexture, std::string& error);
    // WriteObj が書き込むすべての OBJ の先頭行。
    inline constexpr const char* kProceduralObjMarker = "# Procedural Studio OBJ v2";
    // ファイルが Procedural Studio のマーカー（任意のバージョン）で始まる場合は true。
    bool IsProceduralObj(const std::filesystem::path& path);
    // 任意の頂点色 "v x y z r g b" を持つ OBJ と、diffuseTexture をファイル名だけで参照する MTL。
    bool WriteObj(const std::filesystem::path& path, const Mesh& mesh, bool vertexColors,
                  const std::string& diffuseTexture, std::string& error);
}

