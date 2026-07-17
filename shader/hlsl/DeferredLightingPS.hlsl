// Directional VSM receiver-plane bias wrapper.
//
// The implementation is kept in DeferredLightingPS_Impl.hlsli.  This wrapper
// intercepts comparison samples made with the deferred pass' ShadowSampler and
// applies a derivative-based depth offset only when the sampled array layer is
// one of the active directional VSM clipmap layers.  Point/spot shadows and
// conventional cascades therefore keep their existing comparison depth.

#define VSM_RP_LEVEL_MATCH(LOCATION, LEVEL) \
    (step(0.0f, VirtualShadowParams[LEVEL].x) * \
     (1.0f - step(0.5f, abs((LOCATION).z - VirtualShadowParams[LEVEL].x))))

#define VSM_RP_LAYER_MATCH(LOCATION) saturate( \
    VSM_RP_LEVEL_MATCH(LOCATION, 0) + \
    VSM_RP_LEVEL_MATCH(LOCATION, 1) + \
    VSM_RP_LEVEL_MATCH(LOCATION, 2) + \
    VSM_RP_LEVEL_MATCH(LOCATION, 3))

#define VSM_RP_MATCHED_BASE_BIAS(LOCATION) ( \
    VSM_RP_LEVEL_MATCH(LOCATION, 0) * max(VirtualShadowParams[0].z, VirtualShadowParams[0].w) + \
    VSM_RP_LEVEL_MATCH(LOCATION, 1) * max(VirtualShadowParams[1].z, VirtualShadowParams[1].w) + \
    VSM_RP_LEVEL_MATCH(LOCATION, 2) * max(VirtualShadowParams[2].z, VirtualShadowParams[2].w) + \
    VSM_RP_LEVEL_MATCH(LOCATION, 3) * max(VirtualShadowParams[3].z, VirtualShadowParams[3].w))

#define VSM_RP_ACTIVE \
    (step(0.5f, VirtualShadowGlobal.x) * (1.0f - step(1.5f, VirtualShadowGlobal.x)))

// fwidth(depth) measures the receiver-plane depth change across the current
// pixel quad.  Expanding it by the PCF radius prevents adjacent taps on a
// sloped receiver from alternating around the stored directional-shadow depth.
// The cap is tied to the authored per-level bias so silhouettes do not detach.
#define VSM_RP_DEPTH_OFFSET(LOCATION, DEPTH) \
    min( \
        fwidth(DEPTH) * (1.0f + max(VirtualShadowGlobal.z, 1.0f)), \
        max(VSM_RP_MATCHED_BASE_BIAS(LOCATION) * 4.0f, 0.000004f))

#define VSM_RP_ADJUST_ShadowSampler(LOCATION, DEPTH) \
    ((DEPTH) - VSM_RP_ACTIVE * VSM_RP_LAYER_MATCH(LOCATION) * \
        VSM_RP_DEPTH_OFFSET(LOCATION, DEPTH))

// Comparison samplers passed as function parameters inside common.hlsl use the
// lower-case identifier and must remain untouched.
#define VSM_RP_ADJUST_shadowSampler(LOCATION, DEPTH) (DEPTH)

#define VSM_RP_SELECT_I(SAMPLER_NAME, LOCATION, DEPTH) \
    VSM_RP_ADJUST_##SAMPLER_NAME(LOCATION, DEPTH)
#define VSM_RP_SELECT(SAMPLER_NAME, LOCATION, DEPTH) \
    VSM_RP_SELECT_I(SAMPLER_NAME, LOCATION, DEPTH)

// HLSL's preprocessor suppresses re-expansion of the currently expanding macro,
// so the SampleCmpLevelZero token in the replacement remains the texture method.
#define SampleCmpLevelZero(SAMPLER_NAME, LOCATION, DEPTH) \
    SampleCmpLevelZero( \
        SAMPLER_NAME, \
        LOCATION, \
        VSM_RP_SELECT(SAMPLER_NAME, LOCATION, DEPTH))

#include "DeferredLightingPS_Impl.hlsli"
