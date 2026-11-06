# Rendering expansion: implementation and acceptance ledger

Source requirements: [rendering-expansion-design.md](rendering-expansion-design.md).
The requested end state remains all ten features and integrated acceptance.
No phase is complete merely because its source compiles. No commits are authorized.

## Current implementation evidence

The initial worktree was clean at `5cfccfbf`. Existing code contains VG traversal,
mesh raster, residency, VSM, DXR reflections/GI, procedural editing, texture mip
streaming, measurement windows and A/B benchmark scripts. Those existing systems
are dependencies, not evidence that the ten extensions are implemented.

Phase 0 additions:

- `renderfeature.h`: hardware capability contract and CPU budget decision logic.
  A zero limit means unbounded by configuration. Invalid timings are rejected.
  This decision does not establish shader, dependency or resource readiness.
- `GraphicsDevice`: query tiled resources, DXR 1.1, mesh shaders and sampler
  feedback after device creation; failed queries leave support false. Reset on
  initialization and shutdown. No new GPU resource ownership or queues.
- Rendering overview: show hardware support separately from runtime readiness.
- Benchmark metadata: record the same hardware capabilities for each run.
- `tests/renderfeature_tests.cpp` and `scripts/test-renderfeatures.ps1`: CPU
  coverage for disable, unsupported capability, boundary and exceeded budgets,
  invalid timings and unlimited configurations.
- Fixed the existing shader dependency validator to expand the declared shared
  ray telemetry input list before checking files. Both valid dependencies and
  intentional missing-file rejection were tested.

Phase 1 A preparatory reference work:

- CPU hierarchy selection now supports orthographic shadow views: geometric error
  is scaled to texels without perspective distance attenuation. Default camera
  behavior is unchanged. Tests cover distance independence, resolution, object
  scale, forced LOD and existing perspective hierarchy cuts. GPU shadow traversal,
  material bins and shadow raster integration are still required.
- Added shadow selection and common feature policy tests to the normal CTest suite.
- GPU traversal now has an orthographic projection flag at constant-buffer offset
  252 (previously unused padding). The 256-byte ABI and both predictive-streaming
  words are preserved. Main-camera preparation explicitly uses perspective mode.
  Orthographic views use texel error and skip perspective camera-motion prefetch.
- GPU/CPU parity cases cover three orthographic scales at near/far camera
  positions. Before the shader change they failed with a hierarchy-cut mismatch;
  after the change, all 52 traversal cases passed on the RTX 3050 Laptop GPU,
  together with existing paged/unpaged native mesh raster and velocity tests.
  Result: `bin/validation/extension-shadow-gpu/result.log`.
- The same 52 cases and raster/velocity cases passed using WARP:
  `bin/validation/extension-shadow-gpu/warp-result.log`. This WARP version reports
  mesh shader tier 10, so it is not a missing-mesh-shader fallback test.

## Ordered implementation and acceptance gates

| Phase | Source and integration work still required | Evidence required to close |
| --- | --- | --- |
| 0 | Per-feature project settings, readiness/fallback diagnostics, shared lifecycle invalidation, regression baseline | Debug/Release and shader builds, CPU suite, debug-layer/GBV baseline, reproducible A/B captures and measurement conditions |
| 1 A | Shadow-view traversal and independent visible/bin/indirect buffers; mesh shadow PSOs; ancestor residency fallback; VSM dirty tracking | Directional/spot/point and masked/two-sided reference comparisons; movement, missing pages and clipmap boundary tests; shadow GPU time/VRAM |
| 1 G | Surface decal component/material/save/inspector; DBuffer raster and GBuffer composite graph passes | Normal/roughness/depth agreement, ordering/layers/angle fade, VG and legacy receivers, TSR stability and timings |
| 2 H | Independent progressive DXR reference integrator, MIS/RR/bounces/materials/lights, seed and cancellation, EXR/AOV capture | Cornell/metal/glass/emission reference images, decreasing variance, reproducible seed, linear HDR RMSE/SSIM |
| 2 J | Probe components/assets/capture/prefilter, box projection/blend, static/on-demand/realtime updates and reflection fallback | Parallax/boundaries/roughness/DXR transitions, no recursive capture, save/load, capture timing |
| 3 B | Feedback shader writes/map resolve/readback; tile requests, pinned tail, page table, residency publication after fences, generation-safe destruction | 8K/16K textures, mapping/readback lifetime stress, capped VRAM/upload and fallback paths |
| 4 C | Typed deterministic DAG, dirty subgraph cache, editing/undo/save, scatter constraints, preview/bake and GPU Scene/VG integration | Seed equality, partial recompute, 10K/100K instance CPU/GPU timings and save/load |
| 5 D | Versioned cell manifests/GUIDs/dependencies/HLOD, asynchronous transactional loading/cancellation, editor references and fence-deferred release; cell PCG bake | Teleport/reverse/interrupted load, no missing/duplicate visible entities, stable references and bounded memory/spikes |
| 6 E | GPU pool emit/simulate/compact/indirect, bounds/overflow, depth collision/soft particles, OIT/light/reactive integration and CPU fallback | 10K/100K/1M particle costs and overflow, TSR motion, reset/resize/unload safety |
| 7 F | Typed material graph/IR/optimizer/HLSL, all raster/shadow/velocity/hit variants, asynchronous compile and retained last-good PSOs, editor errors/cache | Cross-pass material parity, failed hot reload without PSO loss/crash, cache reuse and VG compatibility |
| 8 I | Phillips/JONSWAP spectrum/IFFT/displacement/normal/foam, near/far water material, depth/optics/reflection/weather integration | Phase continuity, shore/LOD boundary, wind transitions, transparency/TSR stability and timing |
| 9 | All ten enabled together, fallback matrix, long-run lifecycle and memory stress | Debug/Release, GBV zero errors, captures/metrics, 1.3M/100M/1B actual vs visible vs instance counts, resize/alt-tab/scenes/teleports/VRAM stress |

## Integration constraints

Each GPU feature owns frame-ring output buffers until the submission fence passes;
RenderGraph declares reads, writes, indirect-argument states and interqueue waits.
Unpublished resources never replace live resources. Camera cuts, resize/scale,
scene transitions, device loss and material reload invalidate affected histories.
Keep shader/resource failures on the existing fallback path, with a diagnostic
reason; never enable a feature solely because a hardware flag is true.

Start Phase 1 A by separating shadow traversal inputs and storage from the camera
traversal; reusing its visible list would omit off-camera shadow casters. Preserve
legacy/root-proxy shadow submission until the complete replacement path is ready.
Keep G separate from `LightRenderMode::Decal`. Add graph passes in small reviewable
changes, and record actual results below rather than treating this table as proof.

## Verification record

- `scripts/test-renderfeatures.ps1`: passed, compiled with `/W4 /WX`.
- `scripts/test-rendermeasurement.ps1`: passed.
- Initial Debug build: failed with existing C1902 PDB-manager incompatibility.
- Applying `/Z7` against a previously built PCH: failed C2855; an isolated
  `build/extension-debug/` intermediate directory is used for the retry.
- Isolated Debug C++/HLSL build: passed (exit 0),
  `bin/validation/build-extension-debug.log`.
- Existing procedural editor checks: 976 passed; simulation checks: 2801 passed.
- Existing measurement, A/B comparator, temporal history and stable sampling tests:
  passed. Per-shader dependency validation and missing shared dependency rejection:
  passed.
- Debug feature smoke: 84 frames passed; reported D3D12 errors 0 and feature smoke
  failures 0. Logs/captures: `bin/validation/Debug-features/`.
- Debug DXR/DLSS fallback smoke: 84 frames passed;
  `bin/validation/Debug-fallback/`. This does not emulate missing mesh shader or
  sampler feedback hardware and is not full GBV acceptance.
- `scripts/test-vg-shadow-selection.ps1`: passed with `/W4 /WX`, including the
  existing perspective selection regressions, missing detail-page requests,
  resident ancestor selection and missing-root fallback.
- Capture comparison now reports RGB RMSE and local uniform-window SSIM, with
  explicit encoded 8-bit (not linear HDR) scope. Five Python tests passed using
  bundled Python, including comparison against direct window statistics.
  The actual feature/fallback morning captures differ: RGB RMSE 4.55599955,
  SSIM 0.90091802, max channel error 101. Exact equality correctly failed; these
  different configurations are not a before/after regression acceptance pair.
  Result: `bin/validation/feature-fallback-capture-comparison.json`.
- Release C++/HLSL build: passed (exit 0),
  `bin/validation/build-extension-release.log`.
- Release feature and DXR/DLSS fallback smoke: both passed, 84 frames each,
  `bin/validation/Release-features/` and `bin/validation/Release-fallback/`.
  Release reports the debug queue unavailable: do not infer zero D3D12 errors.
- The full VG CPU suite initially failed to compile because a pre-existing stress
  path called removed `VirtualGeometrySerializer::LoadEncoded`. Updated it to
  the runtime's actual `VirtualGeometryPageCache::Open` API.
- The rebuilt CPU suite exposed two fixture issues: atomic cache publication was
  unavailable in the sandbox temp directory, and the standalone raw SSD I/O test
  omitted the now-required per-page cluster directory. Tests now use build-local
  temp directories and provide a valid empty directory for the raw page. Cache
  roundtrip assertions and I/O error diagnostics were strengthened, not removed.
- Debug non-stress CTest suite: all three tests passed, including the existing
  geometry/cache/page-I/O suite (116.79 seconds), shadow selection and feature
  policy. `bin/validation/virtualgeometry/tests-Debug-final.log`.
- Latest Debug C++/HLSL build including the orthographic constant-buffer change:
  passed (exit 0), `bin/validation/build-extension-debug-final.log`.
- Release non-stress CTest suite: all three tests passed, existing suite 7.78
  seconds; `bin/validation/virtualgeometry/tests-Release-final.log`. Million-
  triangle and larger stress acceptance is still pending.
- The GPU Driven validation's standalone header build exposed unqualified
  `overflow_error` names previously hidden by the engine PCH. Qualified them
  with `std::`; the standalone layout/capacity/geometry tests then passed.
- Initial indexed GPU Driven GBV run completed with D3D12 errors 0 but 29 failed
  single-ExecuteIndirect assertions: the runner had not disabled the newer native
  mesh route. The runner now sets/restores `DX12_PHASE5_MESH_SHADER=0` explicitly;
  its assertion is unchanged. The native mesh route has separate parity tests.
- Indexed GPU Driven GBV rerun: passed, 84 frames, D3D12 errors 0 and validation
  failures 0. Includes growth to 50,000 static mesh instances with matching GPU
  counts and indirect arguments. `build/gpudriven/runtime-Debug-gbv.log`.
  This baseline suite does not cover every full-frame auxiliary pass or the new
  shadow path, which is still unimplemented.
- Release A/A measurement plumbing: both runs completed with 60/60 valid GPU
  samples after 30 warmup frames. Session `d58374f9efe5`, under
  `bin/validation/render-benchmark/`; manifests include hardware capabilities.
  Final 1280x720 captures match exactly: changed pixels 0, RMSE 0, SSIM 1.
  Baseline/candidate GPU means were 4.4933344/4.6465781 ms, frame means
  7.1959033/8.5496583 ms. Same-binary variation is +3.41% GPU and +18.81% frame;
  this short functional check is not performance regression/improvement evidence.
  Capture result: `d58374f9efe5-capture-comparison.json`.
- Ignored asset-registry sidecars for already-ignored generated VG cache files;
  authored source asset metadata is unaffected.
- Full-feature GBV, extension A/B captures, linear HDR metrics and performance
  acceptance gates remain pending.

## Validation handles for continuation

No validation process remains running from this implementation batch.

- Indexed GBV `13234` and A/A benchmark `6900`: completed with exit 0.
- CPU suite `10705` completed successfully for Debug and Release. Logs:
  `bin/validation/virtualgeometry/build-<Configuration>-final.log` and
  `tests-<Configuration>-final.log`. Older non-final test logs are not evidence.
- Latest Debug build `65751`, Release build `33725` and Release smoke `6731`
  completed successfully. Prior `46379`, `8303`, `82579` and `92278` are terminal
  failures, as is first GBV run `99444`. Do not poll/restart those jobs.

Do not restart completed jobs solely to recover their output; inspect the saved
logs. Original Debug feature smoke predates the orthographic changes, while the
successful indexed GBV uses the subsequent Debug build. The orthographic GPU
parity test itself does not enable GBV and is not full native-mesh GBV acceptance.

Build command used for each engine configuration (with process-local `_CL_=/Z7`):

```text
MSBuild.exe DirectX12.sln /t:Build /p:Configuration=Debug /p:Platform=x64 /p:IntDir=build\extension-debug\ /m /v:quiet /clp:ErrorsOnly /nologo
MSBuild.exe DirectX12.sln /t:Build /p:Configuration=Release /p:Platform=x64 /p:IntDir=build\extension-release\ /m /v:quiet /clp:ErrorsOnly /nologo
```

Other validation commands used:

```text
scripts/test-renderfeatures.ps1
scripts/test-rendermeasurement.ps1
scripts/test-renderbenchmark-comparison.ps1
scripts/test-temporalhistory.ps1
scripts/test-stablesampling.ps1
scripts/test-shader-build-targets.ps1
scripts/test-vg-shadow-selection.ps1
scripts/validate-rendering.ps1 -Configuration Debug -SkipBuild
scripts/validate-virtualgeometry.ps1 -SkipMillion
scripts/validate-gpudriven.ps1 -Configuration Debug -SkipBuild -GpuValidation -TimeoutSeconds 240
scripts/measure-rendering.ps1 -Configuration Release -SkipBuild -WarmupFrames 30 -MeasuredFrames 60 -Pairs 1
<bundled-python> tests/render_capture_comparison_tests.py
```

`<bundled-python>` is
`C:\Users\songt\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe`;
the system Python lacks NumPy. No packages were installed or tool settings changed.

All phases remain open until their required evidence exists.
