# Live character lighting and outline optimization

The user delegated implementation details on dev_main. Aim for the readable skin,
colored stage lighting and separated silhouettes of an idol live scene. This is an
artistic approximation, not a reconstruction of another game's proprietary shader.

## Approach

Keep the existing Forward/Deferred, material-class and shadow infrastructure.
Evaluate cel bands per light, rather than shading once with an averaged direction:
opposing colored lights must not cancel their direction or lose their colors.
Hair (1), cloth (2), skin (3), and Toon (4) participate. Eye/unlit/PBR retain their
existing meaning. A shared shader helper owns the cel response, softer skin profile,
highlight and directional rim. Base color is applied once; light intensity and
shadow visibility are applied once. Use existing per-pixel shadow settings and
roughness, without adding buffers or using another entity's global material style.
Ambient fill preserves readability; it does not replace direct stage lighting.

Alternatives considered: an aggregate-light color correction is cheaper but loses
opposing lights; dedicated face SDF and hair-tangent textures offer more control
but require authored assets absent from this request. Keep them future work.

## Outline work

Keep Extrude/TEO/Mix and all four imported TEO presets compatible. Normalize the
projected extrusion direction in pixel space, preserve zero-normal TEO base vertices,
and suppress extrusion behind the camera. Reuse the base transform and transform
the normal as a vector. Skip zero-width mesh outlines. Skin only the requested
TEO preset, cache its pose independently, and skin no TEO in Extrude or shadow passes.
This reduces GPU work; imported TEO buffers remain allocated for immediate mode changes.

## Validation and limits

Execute the actual shared HLSL math in portable C++ regression tests, plus tests for
per-mode cache changes, opposing lights, linear albedo response, shadow suppression,
skin color response, black lights, diagonal pixel widths and TEO anchors. Compile
affected shaders using DXC, including ray-query variants where supported.
Windows/D3D12 runtime screenshots and timings require a Windows GPU environment;
do not claim frame-rate improvements or visual equivalence from numerical tests.
