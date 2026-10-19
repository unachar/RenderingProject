#pragma once
// Procedural Studio: CPU generators for fractal / procedural textures, solid (3D) textures, volumes and
// models. Pure C++ (no D3D) so it can be unit tested and shared by the editor and the cloud renderer.
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ProcStudio
{
struct Float2
{
    float X = 0, Y = 0;
};
struct Float3
{
    float X = 0, Y = 0, Z = 0;
};
struct Float4
{
    float X = 0, Y = 0, Z = 0, W = 1;
};

enum class Basis
{
    Perlin,
    Value,
    Worley,
    CellEdge, // Worley F2 - F1
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
    float Frequency = 4.0f; // cells across one texture / one world unit for solid textures
    int Octaves = 5;
    float Lacunarity = 2.0f;
    float Gain = 0.5f;
    float WarpStrength = 0.6f;
    float RidgeOffset = 1.0f;
};

// Lattice noise in [-1, 1] (Worley variants in [0, 1]). period > 0 wraps the lattice for tiling.
float BasisNoise(Basis basis, float x, float y, float z, uint32_t seed, int period);
// Fractal sum, normalized to roughly [0, 1].
float FractalNoise(const NoiseParams& params, float x, float y, float z, bool tileable);

struct Image
{
    int Width = 0, Height = 0;
    std::vector<Float4> Pixels; // linear RGBA
    Float4 Sample(float u, float v) const; // bilinear, wrapping
};

struct Volume
{
    int Extent = 0;
    std::vector<float> Density; // Extent^3, x fastest, [0, 1]
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
    // Marble / wood
    float Turbulence = 4.0f;
    float RingCount = 8.0f;
    // Brick / checker
    int BricksX = 4, BricksY = 8;
    float Mortar = 0.06f;
    // Water
    float Time = 0.0f;
    float Choppiness = 0.6f;
    // Cloud
    float Coverage = 0.5f;
    float Softness = 0.25f;
    // Escape-time fractals
    float CenterX = -0.5f, CenterY = 0.0f, Zoom = 1.0f;
    float JuliaX = -0.8f, JuliaY = 0.156f;
    int Iterations = 200;
    float NormalStrength = 4.0f;
};

const char* TextureKindName(TextureKind kind);
bool IsSolidCapable(TextureKind kind);
// Scalar field at a 2D (u, v) or 3D point, [0, 1]. Used for height/normal output and solid textures.
float TextureScalar(const TextureParams& params, float x, float y, float z, bool solid);
Float3 TextureColor(const TextureParams& params, float x, float y, float z, bool solid);
Image GenerateTexture(const TextureParams& params, int width, int height);
// Tileable 3D density, e.g. for the volumetric cloud base noise.
Volume GenerateVolume(const TextureParams& params, int extent);
// Build a tileable volume from any 2D texture: octaves of the image sampled on sheared planes.
Volume VolumeFromImage(const Image& image, int extent, int octaves, float verticalVariation, uint32_t seed);
// Solid texture baked to a 3D volume (RGBA unpacked per voxel), for DDS export.
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
    FractalLeaf, // single leaf, for previewing / exporting the leaf used by the tree
    Count
};

enum class LeafShape
{
    Card,     // legacy crossed quads (cheapest)
    Simple,   // one curved blade
    Pinnate,  // fractal compound leaf: leaflets along a rachis, each again a compound leaf
    Fern,     // fractal frond: many alternate pinnae, strong self-similarity (Barnsley-like)
    Palmate,  // fractal palmate leaf: lobes radiate from the base, each lobe again palmate
    Count
};

struct LeafParams
{
    LeafShape Shape = LeafShape::Pinnate;
    int Depth = 2;             // recursion depth of the self-similar subdivision
    int Leaflets = 4;          // leaflet pairs per rachis (pinnate / fern) or lobes (palmate)
    float LeafletAngle = 50.0f;  // degrees between rachis and leaflet
    float LeafletScale = 0.42f;  // child length / parent length
    float Taper = 0.6f;        // leaflets shrink towards the tip (0 = uniform)
    float Width = 0.32f;       // blade width / blade length
    float Droop = 0.25f;       // curvature along the length (bends towards -normal)
    float Fold = 0.15f;        // V-fold across the midrib
    int Segments = 6;          // blade segments along the length
    float ColorVariation = 0.15f;
    std::array<float, 3> Color{0.16f, 0.42f, 0.12f};
    std::array<float, 3> TipColor{0.42f, 0.55f, 0.15f};
};

const char* LeafShapeName(LeafShape shape);
// Leaf template in a local frame: base at the origin, length along +Y (1 unit), width along X,
// front face +Z. Front and back faces are separate (double sided). Normals are computed.
Mesh GenerateLeaf(const LeafParams& params, uint32_t seed);

struct ModelParams
{
    ModelKind Kind = ModelKind::FractalTree;
    NoiseParams Noise;
    uint32_t Seed = 7;
    // Terrain / water
    int Resolution = 96;
    float Size = 10.0f;
    float Height = 2.5f;
    bool TerrainColors = true;
    // Tree
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
    // Upper bound for leaf triangles in a tree; leaf detail (recursion depth) is reduced to fit.
    int LeafTriangleBudget = 600000;
    LeafParams Leaf;
    int RadialSegments = 8;
    // Rock
    int Subdivisions = 4;
    float Displacement = 0.35f;
    // Water
    float Time = 0.0f;
    float WaveAmplitude = 0.35f;
    // Fractal solids
    int Level = 2;
};

const char* ModelKindName(ModelKind kind);
Mesh GenerateModel(const ModelParams& params);
// First line of every OBJ written by WriteObj.
inline constexpr const char* kProceduralObjMarker = "# Procedural Studio OBJ v2";
// True when the file starts with the Procedural Studio marker (any version).
bool IsProceduralObj(const std::filesystem::path& path);
// OBJ with optional "v x y z r g b" vertex colors and an MTL referencing diffuseTexture (file name only).
bool WriteObj(const std::filesystem::path& path, const Mesh& mesh, bool vertexColors, const std::string& diffuseTexture,
              std::string& error);
} // namespace ProcStudio

