# Procedural Studio GPU Implementation Plan

**Goal:** Run independent texture calculations and flat previews on DirectX 12 GPU compute, retaining CPU generation for branching mesh construction and double-precision escape fractals.
**Architecture:** A dedicated texture compute component records work into the editor command list. Flat previews stay on GPU; CPU mesh previews request asynchronous readback. Export workers use a separate queue and read back only for file encoding. Existing CPU generators remain available if GPU setup fails.
**Tech stack:** C++20, DirectX 12, HLSL compute, existing editor descriptors.
**Authorization:** User requested implementation of the preceding proposal on dev_main; no additional design approval is required. Work stays on dev_main.

## Acceptance and review focus
- Non-escape 2D texture kinds, all noise bases/fractals, color/height/normal outputs match CPU within floating-point tolerance.
- Mandelbrot and Julia retain CPU double precision; model topology remains CPU.
- Flat previews do not read pixels back; tiles, gamma, borders and normal wrapping preserve existing behavior.
- Readback is fence guarded; descriptor/resource lifetimes survive editing, view changes and shutdown.
- Export uses GPU where supported and preserves file formats and CPU fallback.

## Tasks
- [x] Add an actual D3D12 compute test harness and observe a missing-feature failure.
- [x] Implement texture compute shaders and resource management; compare actual GPU readback to CPU reference.
- [x] Integrate GPU generation, flat preview, asynchronous CPU preview readback and export fallback.
- [x] Run GPU parity tests, existing procedural regression tests and x64 application build; review changed code.

## Decisions and evidence
- Independent pixel work runs on GPU. Escape fractals remain CPU to preserve double precision at deep zoom.
- CPU software mesh previews remain compatible through asynchronous readback; flat previews remain on GPU.
- No speedup claim until timing evidence exists.

## Verification results
- Missing-backend check failed before implementation; animated editor preview regression failed before its fix.
- NVIDIA GeForce RTX 3050 Laptop GPU and Microsoft WARP: 84 CPU/GPU image comparisons, six bilinear/tiled/sRGB previews, separate export queue, actual editor job/preview transitions, CPU escape-fractal fallback and unavailable-GPU fallback passed. D3D12 debug-layer errors were checked.
- Existing geometric texture tests and terrain preview tests passed.
- Full Debug x64 application build passed with the existing /Z7 validation convention; new C++ files passed clang-format checks.
- Independent review found and verified fixes for animated readback starvation and export notification-failure resource lifetime.
- Initial incremental Debug binaries encountered scene-snapshot restoration/destruction access violations. A full build into a fresh intermediate directory eliminated the stale-build mismatch. With original saved-scene settings restored, the application completed 12 frames and normal shutdown with exit code 0. Evidence: build/procedural-gpu-tests/application-build-clean.log and application-smoke-clean.log.
- Diagnostic settings were restored and their SHA-256 hash matches the original backup. No scene contents were changed. The final application smoke used the original configuration without editing it.
