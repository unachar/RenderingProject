#pragma once
#include "main.h"
#include "ecs.h"
#include <d3dcompiler.h>
#include <string>
#include <unordered_map>

enum class PostProcessType;
struct PostProcessComponent;
struct MaterialComponent;

enum class RenderMode
{
    FORWARD,
    DEFERRED
};

enum class GBufferType : uint32_t
{
    // Hybrid Deferred + Forward+ Minimal GBuffer (4 render targets)
    BASE_COLOR_METAL = 0,    // RT0: RGB=BaseColor, A=Metallic
    NORMAL_ROUGHNESS,        // RT1: RGB=Normal, A=Roughness
    MOTION_VECTOR,           // RT2: RG=MotionVec, B=Depth, A=MaterialID
    EMISSION_AO,             // RT3: RGB=Emission, A=AO
    
    // Legacy GBuffer indices (for backward compatibility)
    // These map to the same physical buffers as above
    BASE_COLOR = 0,          // = BASE_COLOR_METAL
    NORMAL = 1,              // = NORMAL_ROUGHNESS
    DEPTH = 2,               // = MOTION_VECTOR (B channel)
    MATERIAL = 3,            // = EMISSION_AO (or NORMAL_ROUGHNESS A)
    SHADOW = 3,              // = EMISSION_AO
    VISIBILITY = 3,          // = EMISSION_AO
    BLOOM = 3,               // = EMISSION_AO
    ATMOSPHERE = 3,          // = EMISSION_AO
    VELOCITY = 2,            // = MOTION_VECTOR
    
    COUNT = 4                // Actual number of render targets
};

// Convenience aliases
inline constexpr GBufferType GBufferBaseColorMetal = GBufferType::BASE_COLOR_METAL;
inline constexpr GBufferType GBufferNormalRoughness = GBufferType::NORMAL_ROUGHNESS;
inline constexpr GBufferType GBufferMotionVector = GBufferType::MOTION_VECTOR;
inline constexpr GBufferType GBufferEmissionAO = GBufferType::EMISSION_AO;

inline constexpr LPCWSTR g_GBufferTargetNames[] = {
    L"BaseColorMetalBuffer", L"NormalRoughnessBuffer", L"MotionVectorBuffer", L"EmissionAOBuffer"};

struct Vertex
{
    XMFLOAT3 Pos{};
    XMFLOAT3 Normal{};
    XMFLOAT2 Tex{};
    XMFLOAT4 Color{};
};

enum class ShapeType
{
    NONE,
    TRIANGLE,
    QUAD,
    PENTAGON,
    HEXAGON,
    HEPTAGON,
    OCTAGON,
    CIRCLE
};

enum class ObjectType
{
    NONE,
    CUBE,
    SPHERE,
    CAPSULE,
    CYLINDER,
    PLANE,
    QUAD,
    CIRCLE,
    TRIANGLE,
    CONE,
    TETRAHEDRON,
    TRIANGULAR_PRISM
};

enum class Color
{
    NONE,
    WHITE,
    RED,
    GREEN,
    BLUE,
    YELLOW,
    CYAN,
    MAGENTA,
};

enum class PostProcessType
{
    NONE = 0,
    BLUR,
    SEPIA,
    GRAYSCALE,
    INVERT,
    BLOOM,
    COUNT
};

enum class AntiAliasingMode
{
    NONE = 0,
    FXAA,
    TAA,
    COUNT
};

struct ConstantBuffer3D
{
    XMMATRIX World{};
    XMMATRIX View{};
    XMMATRIX Projection{};
    int UseTexture = 0;
    int FlipNormal = 0;
    int UseNormalMap = 0;
    int MaterialMode = 0;
    XMFLOAT3 CameraPos = {0.0f, 0.0f, 5.0f};
    int ShaderClass = 10;
    float MaterialMetallic = 0.0f;
    float MaterialRoughness = 0.5f;
    float MaterialFresnel = 0.04f;
    float MaterialPadding = 0.0f;
    float ToonOutlineWidth = 0.035f;
    float ToonOutlineScreenWidth = 3.0f;
    XMFLOAT2 ViewportSize = {1.0f, 1.0f};
    int ToonOutlineUseScreenSpace = 0;
    float MaterialAlpha = 1.0f;
    int MaterialIsTransparent = 0;
    float ConstantPadding = 0.0f;
    XMMATRIX PreviousWorld{};
    XMMATRIX PreviousViewProjection{};
    // マテリアルグラフのオブジェクト情報（MaterialGraphVM.hlsli）。Extents が 0 のときはシェーダー側の既定値
    XMFLOAT4 ObjectBoundsCenter = {0.0f, 0.0f, 0.0f, 0.0f};  // xyz = ローカルの範囲の中心, w = インスタンスの番号（乱数の種）
    XMFLOAT4 ObjectBoundsExtents = {0.0f, 0.0f, 0.0f, 0.0f}; // xyz = ローカルの範囲の半分の大きさ, w = 1（有効）
};

struct VertexResource
{
    EntityID entityid = g_kINVALID_ENTITY;
    ShapeType shapetype = ShapeType::NONE;
    ObjectType objectType = ObjectType::NONE;
    float radius = 0.f;
    Color color = Color::NONE;
};

