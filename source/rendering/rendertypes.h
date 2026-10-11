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
	BASE_COLOR = 0,
	NORMAL,
	DEPTH,
	MATERIAL,
	SHADOW,
	ATMOSPHERE,
	VELOCITY,
	VISIBILITY,
	BLOOM,
	COUNT
};

inline constexpr LPCWSTR g_GBufferTargetNames[] =
{
	L"ColorBuffer",
	L"NormalBuffer",
	L"DepthBuffer",
	L"MaterialBuffer",
	L"ShadowBuffer",
	L"AtmosphereGBuffer",
	L"VelocityBuffer",
	L"VisibilityBuffer",
	L"BloomBuffer"
};

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
	QUAD
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
	XMFLOAT3 CameraPos = { 0.0f, 0.0f, 5.0f };
	int ShaderClass = 10;
	float MaterialMetallic = 0.0f;
	float MaterialRoughness = 0.5f;
	float MaterialFresnel = 0.04f;
	float MaterialPadding = 0.0f;
	float ToonOutlineWidth = 0.035f;
	float ToonOutlineScreenWidth = 3.0f;
	XMFLOAT2 ViewportSize = { 1.0f, 1.0f };
	int ToonOutlineUseScreenSpace = 0;
	float MaterialAlpha = 1.0f;
	int MaterialIsTransparent = 0;
	float ConstantPadding = 0.0f;
	XMMATRIX PreviousWorld{};
	XMMATRIX PreviousViewProjection{};
};

struct VertexResource
{
	EntityID entityid = g_kINVALID_ENTITY;
	ShapeType shapetype = ShapeType::NONE;
	ObjectType objectType = ObjectType::NONE;
	float radius = 0.f;
	Color color = Color::NONE;
};
