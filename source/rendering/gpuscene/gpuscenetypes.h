#pragma once
#include <DirectXMath.h>
#include <cstdint>
#include <cstddef>
#include <array>

using GPUInstanceID = uint32_t;
using GPUMeshID = uint32_t;
using GPUMaterialID = uint32_t;
inline constexpr GPUInstanceID InvalidGPUInstanceID = UINT32_MAX;
inline constexpr uint32_t GpuSceneInitialCapacity = 1024;
inline constexpr uint32_t GpuSceneMaterialParameterVectors = 160;

enum class GpuSceneDirtyFlags : uint32_t
{
    None = 0,
    Transform = 1,
    Mesh = 2,
    Material = 4,
    Bounds = 8,
    All = 15
};

inline GpuSceneDirtyFlags operator|(GpuSceneDirtyFlags first, GpuSceneDirtyFlags second)
{
    return static_cast<GpuSceneDirtyFlags>(static_cast<uint32_t>(first) | static_cast<uint32_t>(second));
}

struct GPUInstanceData
{
    DirectX::XMFLOAT4X4 World{};
    DirectX::XMFLOAT4X4 PreviousWorld{};
    uint32_t MeshIndex = 0;
    uint32_t MaterialIndex = 0;
    uint32_t Flags =
        0; // bit 0: active; bit 1: frustum; bit 2: GPU opaque; bit 3: moving; bit 4: native hierarchy eligible.
    uint32_t Entity = UINT32_MAX;
    DirectX::XMFLOAT4 BoundsCenter{}; // World-space AABB.
    DirectX::XMFLOAT4 BoundsExtents{};
    DirectX::XMFLOAT4 LocalCenter{};
    DirectX::XMFLOAT4 LocalExtents{};
    DirectX::XMFLOAT4 LodDistances{}; // Preserve existing component metadata; no new LOD selection.
    uint32_t LODGroupIndex = UINT32_MAX;
    uint32_t VirtualGeometryAssetIndex = UINT32_MAX;
    uint32_t VisibilityPadding[2]{};
};

struct GPUMeshData
{
    uint32_t VertexAddressLow = 0;
    uint32_t VertexAddressHigh = 0;
    uint32_t IndexAddressLow = 0;
    uint32_t IndexAddressHigh = 0;
    uint32_t VertexCount = 0;
    uint32_t IndexCount = 0;
    uint32_t StartIndex = 0;
    int32_t BaseVertex = 0;
    DirectX::XMFLOAT4 BoundsCenter{};
    DirectX::XMFLOAT4 BoundsExtents{};
};

struct GPUMaterialData
{
    DirectX::XMFLOAT4 BaseColor{1, 1, 1, 1};
    DirectX::XMFLOAT4 Surface{};
    uint32_t AlbedoTextureIndex = UINT32_MAX;
    uint32_t NormalTextureIndex = UINT32_MAX;
    uint32_t Flags = 0;
    uint32_t ShaderClass = 0;
    uint32_t ParameterVectorCount = 0;
    uint32_t ShaderMode = 0;
    uint32_t Padding[2]{};
    array<DirectX::XMFLOAT4, GpuSceneMaterialParameterVectors> ShadingParameters{};
};

static_assert(sizeof(GPUInstanceData) == 240);
static_assert(offsetof(GPUInstanceData, LODGroupIndex) == 224);
static_assert(offsetof(GPUInstanceData, VirtualGeometryAssetIndex) == 228);
static_assert(offsetof(GPUInstanceData, PreviousWorld) == 64);
static_assert(offsetof(GPUInstanceData, MeshIndex) == 128);
static_assert(offsetof(GPUInstanceData, BoundsCenter) == 144);
static_assert(offsetof(GPUInstanceData, LocalCenter) == 176);
static_assert(sizeof(GPUMeshData) == 64);
static_assert(sizeof(GPUMaterialData) == 2624);
static_assert(offsetof(GPUMaterialData, ShadingParameters) == 64);

struct GpuSceneStatistics
{
    uint32_t InstanceSlots = 0;
    uint32_t ActiveInstances = 0;
    uint32_t InstanceCapacity = 0;
    uint32_t Meshes = 0;
    uint32_t Materials = 0;
    uint32_t DirtyInstances = 0;
    uint32_t EntitiesProcessed = 0;
    uint64_t UploadedBytes = 0;
    uint64_t InstanceUploadedBytes = 0;
    uint64_t BufferMemoryBytes = 0;
    double UpdateCpuMilliseconds = 0;
};
