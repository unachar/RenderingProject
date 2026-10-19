#pragma once
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

    enum class Basis
    {
        Perlin,
        Value,
        Worley,
        CellEdge, // Worleyノイズの距離差（F2 - F1）
        Count
    };
    enum class FractalType
    {
        FBm,
        Ridged,
        Billow,
        Turbulence,
        HybridMultifractal,
        DomainWarp,
        Count
    };

    struct NoiseParams
    {
        Basis BasisType = Basis::Perlin;
        FractalType Fractal = FractalType::FBm;
        uint32_t Seed = 1337;
        float Frequency = 4.0f; // テクスチャ全体のセル数。立体テクスチャではワールド空間 1 単位あたりのセル数。
        int Octaves = 5;
        float Lacunarity = 2.0f;
        float Gain = 0.5f;
        float WarpStrength = 0.6f;
        float RidgeOffset = 1.0f;
    };

    // 格子ノイズは [-1, 1]（Worley 系は [0, 1]）。period > 0 では格子を折り返してタイル化する。
    float BasisNoise(Basis basis, float x, float y, float z, uint32_t seed, int period);
    // およそ [0, 1] に正規化したフラクタル和。
    float FractalNoise(const NoiseParams& params, float x, float y, float z, bool tileable);

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
        Count
    };
    enum class TextureOutput
    {
        Color,
        Height,
        Normal,
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
    // WriteObj が書き込むすべての OBJ の先頭行。
    inline constexpr const char* kProceduralObjMarker = "# Procedural Studio OBJ v2";
    // ファイルが Procedural Studio のマーカー（任意のバージョン）で始まる場合は true。
    bool IsProceduralObj(const std::filesystem::path& path);
    // 任意の頂点色 "v x y z r g b" を持つ OBJ と、diffuseTexture をファイル名だけで参照する MTL。
    bool WriteObj(const std::filesystem::path& path, const Mesh& mesh, bool vertexColors,
                  const std::string& diffuseTexture, std::string& error);
}
