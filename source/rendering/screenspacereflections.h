#pragma once

// Native Hi-Z screen-space reflections (see
// docs/rendering-technology-ssr-design.md). Step 1 provides the
// single CPU-side selection point for the frame's reflection source,
// which the lighting pass consumes through the existing envSpecular
// override contract: raytraced result wins over SSR, SSR wins over
// probe/environment IBL.
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
} // namespace ScreenSpaceReflections
