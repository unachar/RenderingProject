#pragma once
#include <cstdint>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

// Stage 1D: DBuffer surface decals (see
// docs/rendering-technology-stage1d-design.md). A decal is an oriented
// box projector with a bindless material; this module is the CPU
// registry and the GPU packing. Rendering passes consume the packed
// records; the registry itself owns no GPU resources.
//
// Texture indices are bindless heap indices; kSurfaceDecalNoTexture
// means "no texture" (the channel keeps its default: white albedo,
// flat normal, material roughness parameter).
namespace SurfaceDecals
{
    inline constexpr uint32_t kNoTexture = 0xFFFFFFFFu;
    inline constexpr uint32_t kMaxDecals = 4096;

    struct Decal
    {
        // Row-major world -> decal-local matrix. Local space is the
        // unit box: receivers with |local| > 1 on any axis are outside.
        float WorldToLocal[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float Opacity = 1.0f;
        uint32_t AlbedoTexture = kNoTexture;
        uint32_t NormalTexture = kNoTexture;
        uint32_t RoughnessTexture = kNoTexture;
        // Receiver layer mask: the decal applies when
        // (receiverLayer & LayerMask) != 0. v1 receivers are layer 1.
        uint32_t LayerMask = 0xFFFFFFFFu;
        // Angle fade on dot(receiverNormal, -projectionAxis): full
        // strength at/above StartCos, fading to zero at EndCos.
        float AngleFadeStartCos = 0.5f;
        float AngleFadeEndCos = 0.25f;
        // Roughness used when no roughness texture is bound.
        float Roughness = 0.85f;
        // Draw order: lower priority rasterizes first.
        int32_t Priority = 0;
    };

    // GPU record; must match GpuSurfaceDecal in the decal shaders.
    struct GpuDecal
    {
        float WorldToLocal[16];
        float ExtentsOpacity[4]; // xyz unused in v1 (unit box), w = opacity
        uint32_t Textures[4];    // albedo, normal, roughness, layer mask
        float Fade[4];           // start cos, end cos, reserved, reserved
    };
    static_assert(sizeof(GpuDecal) == 112, "GpuDecal layout must stay 112 bytes");

    void Clear();
    // Returns the decal index, or kNoTexture when the registry is full.
    uint32_t Add(const Decal& decal);
    uint32_t Count();
    const Decal& Get(uint32_t index);

    // Fills `out` with the packed records in raster order (priority,
    // then submission order). Returns the record count.
    uint32_t PackAll(std::vector<GpuDecal>& out);

    // ---- DBuffer targets -------------------------------------------------
    // The DBuffer lives in this module with its own small descriptor
    // heaps, so the engine-wide descriptor layout is untouched. Three
    // targets at scene resolution: albedo (RGBA8, rgb = color,
    // a = coverage), normal (RGBA16F, rgb = world normal), roughness/
    // mask (RGBA8, r = roughness, a = composite mask). Cleared to zero;
    // a zero mask means "no decal here".
    inline constexpr uint32_t kDBufferCount = 3;

    // Creates or recreates the targets when the size changed. Returns
    // false on allocation failure (callers then skip the decal passes).
    bool EnsureTargets(ID3D12Device* device, uint32_t width, uint32_t height);
    void ReleaseTargets();
    bool TargetsReady();
    ID3D12Resource* TargetResource(uint32_t index);
    D3D12_CPU_DESCRIPTOR_HANDLE TargetRtv(uint32_t index);
    // Start of the shader-visible SRV heap holding the three targets.
    ID3D12DescriptorHeap* TargetSrvHeap();
    D3D12_GPU_DESCRIPTOR_HANDLE TargetSrvGpuStart();

    // ---- Rendering -------------------------------------------------------
    // Everything Render() needs from the frame. The GBuffer targets
    // must be in PIXEL_SHADER_RESOURCE state on entry and are returned
    // to that state; the scene depth is only read. InvViewProjection
    // is row-major for HLSL mul(float4, M) (the engine convention:
    // XMMatrixTranspose(XMMatrixInverse(view * projection))).
    struct RenderParams
    {
        ID3D12Device* Device = nullptr;
        ID3D12GraphicsCommandList* List = nullptr;
        ID3D12DescriptorHeap* EngineHeap = nullptr;
        uint32_t Width = 0;
        uint32_t Height = 0;
        float InvViewProjection[16] = {};
        float CameraPosition[4] = {};
        D3D12_GPU_DESCRIPTOR_HANDLE DepthSrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE GBufferNormalSrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE TextureTable{};
        ID3D12Resource* GBufferTargets[3] = {}; // base color, normal, material
        D3D12_CPU_DESCRIPTOR_HANDLE GBufferRtvs[3] = {};
        DXGI_FORMAT GBufferFormats[3] = {};
    };

    // Rasterizes all registered decals into the DBuffer and composites
    // the result into the GBuffer targets. Returns false when setup
    // failed before any state changed (safe to skip decals then).
    bool Render(const RenderParams& params);
} // namespace SurfaceDecals
