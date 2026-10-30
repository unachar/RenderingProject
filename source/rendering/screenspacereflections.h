#pragma once
#include <cstdint>
#include <d3d12.h>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12DescriptorHeap;
struct ID3D12Resource;

// Native Hi-Z screen-space reflections (see
// docs/rendering-technology-ssr-design.md). Step 1 provides the
// single CPU-side selection point for the frame's reflection source,
// which the lighting pass consumes through the existing envSpecular
// override contract: raytraced result wins over SSR, SSR wins over
// probe/environment IBL. Step 3 adds the trace-pass host: result
// target, pipeline and dispatch recording (frame wiring lands in a
// later step).
namespace ScreenSpaceReflections
{
    enum class Source
    {
        Environment,  // probe override or global environment texture
        ScreenSpace,  // this module's result texture
        Raytraced,    // RayReflections result texture
    };

    // dxrReflectionActive: the raytraced reflection result will be
    // bound this frame. ssrRequested: the settings flag is on.
    // ssrResultReady: the SSR module produced a valid result texture
    // for this frame (false during warmup / when disabled by caps).
    Source SelectSource(bool dxrReflectionActive, bool ssrRequested, bool ssrResultReady);

    // Per-frame decision, computed once in RenderFrame::BeginDraw and
    // consumed by the lighting bindings (postprocess constants +
    // scene pass tables) so every consumer agrees.
    void UpdateFrameSelection(bool dxrReflectionActive);
    Source GetFrameSource();
    bool IsScreenSpaceSelected();
    // Engine-heap SRV index of the result texture (registered with
    // TextureManager when the target is created), or -1.
    int GetResultEngineSrvIndex();

    // Trace constants, laid out to match ScreenSpaceReflectionsTraceCS
    // cbuffer exactly (52 DWORDs). Matrices are stored transposed
    // (engine HLSL convention). DepthNumerator/DepthDenominator come
    // from the projection matrix: Numerator = P[3][2],
    // Denominator = P[2][2] (row-major element indices).
    struct TraceConstants
    {
        float InvViewProjection[16];
        float ViewProjection[16];
        float Resolution[2];
        float InvResolution[2];
        float DepthNumerator;
        float DepthDenominator;
        float MaxDistance;
        float Thickness;
        uint32_t MaxSteps;
        float StridePixels;
        float EdgeFade;
        float FallbackRoughness;
        float CameraPosition[3];
        float CheckerboardParity = -1.0f; // < 0: off; 0/1: traced pixel parity
        float CameraForward[3];
        uint32_t GBufferScale; // 1 = full-res trace, 2 = half-res trace (T1)
    };

    struct TraceInputs
    {
        // GPU handles into the engine's shared SRV heap.
        D3D12_GPU_DESCRIPTOR_HANDLE DepthSrv;
        D3D12_GPU_DESCRIPTOR_HANDLE HiZSrv;
        D3D12_GPU_DESCRIPTOR_HANDLE NormalSrv;
        D3D12_GPU_DESCRIPTOR_HANDLE PreviousColorSrv;
        D3D12_GPU_DESCRIPTOR_HANDLE MaterialSrv;
        TraceConstants Constants;
        // Previous frame's view-projection (transposed, engine
        // convention) for the temporal resolve's hit-point
        // reprojection, plus a history reset request (camera cuts).
        float PrevViewProjection[16] = {};
        bool ResetHistory = false;
        // Checkerboard trace mode: alternate pixel parity per frame,
        // temporal history fills the gaps.
        bool Checkerboard = false;
    };

    // (Re)creates the result target (RGBA16F, UAV+SRV) at the given
    // resolution. Returns false on failure; the previous result, if
    // any, is kept.
    bool EnsureResultTarget(ID3D12Device* device, uint32_t width, uint32_t height);
    void ReleaseResultTarget();
    bool IsResultReady();
    ID3D12Resource* GetResultResource();
    // SRV of the result inside this module's own heap (valid after
    // EnsureResultTarget); the frame binding step consumes it.
    D3D12_GPU_DESCRIPTOR_HANDLE GetResultSrv();

    // Records the trace dispatch. `engineHeap` is the engine's shared
    // shader-visible heap (bound alongside the module heap so the
    // input handles stay valid). Returns false when the pipeline or
    // result target is unavailable; nothing is recorded then.
    bool Execute(ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* engineHeap,
                 const TraceInputs& inputs);
} // namespace ScreenSpaceReflections
