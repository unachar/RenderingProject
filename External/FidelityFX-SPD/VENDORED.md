# FidelityFX SPD (vendored subset)

Source: GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, commit
7539ae8b6568ad29ed78a753404351daf3b5cca7 (2026-10-28).

Files (verbatim, no local modifications; MIT license text is in each file's
header):

- `ffx_spd.h` — from `Kits/FidelityFX/upscalers/fsr3/include/gpu/spd/`
  (SPD 2.x single-pass downsampler, shader-only)
- `ffx_core.h`, `ffx_core_hlsl.h`, `ffx_core_gpu_common.h`,
  `ffx_core_gpu_common_half.h`, `ffx_core_portability.h`,
  `ffx_common_types.h` — from `Kits/FidelityFX/api/internal/gpu/`
  (type/helper headers ffx_spd.h depends on)

Only the shader-side headers are vendored; the SDK host API is not used.
The engine's integration lives in `shader/hlsl/HiZSpdCS.hlsl` and
`source/rendering/occlusionculling.cpp`.
