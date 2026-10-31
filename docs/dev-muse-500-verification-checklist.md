# dev_muse 500-push run — Windows verification checklist

Everything in the 500-push run is code-complete but NOT build/GPU
verified (developed on a Linux VM: HLSL compiled with standalone
DXC, portable logic CPU-tested). Run through this on the Windows /
RTX machine when the batch lands. Each item lists the flag or env
gate and the expected observable behavior.

## 0. Build

- [ ] Full solution builds with no new warnings in the touched
      files (screenspacereflections, scenepasses, postprocesspass,
      reflectionprobes, reference*, editorsettings,
      virtualgeometrytraversal).
- [ ] `scripts/test-shader-build-targets.ps1` passes (45 tracked
      DXC outputs).
- [ ] `tests/shader_scan_tests.py` and `tests/pfm_compare_tests.py`
      pass, plus the run's source-contract tests:
      traversal_binning / ssr_constants / reference_integrator /
      postprocess_probe contract tests (all under tests/), plus
      shader_build_targets_contract_tests.py, the Linux port of
      the PowerShell targets gate.
      Already green on the Linux side, together with the DXC
      compile sweep (`scripts/compile_check_linux.sh`, 17/17
      entry points) - the Windows build above is the remaining
      gate.

## 1. SSR (flag: スクリーンスペース反射, default OFF)

- [ ] Flag OFF: frame identical to before the run (selection state
      stays env IBL).
- [ ] The very first frame after enabling SSR shows plain env
      reflections, not a one-frame flash of the environment
      texture sampled as the SSR result (tri-state guard,
      b59d5a63).
- [ ] Flag ON in a scene with glossy floors: reflections appear
      after 1 frame of lag; moving the camera keeps them stable
      (T2 hit-point accumulation), no smearing trails on rough
      surfaces (roughness-scaled blend).
- [ ] Checkerboard sub-flag ON: no visible checker pattern in
      motion; GPU ms of the SSR trace roughly halves. (The
      resolve-constants fix in 970b4aae is what makes the parity
      reach the resolve at all - this check validates it.)
- [ ] Rough surfaces with SSR ON: reflections look soft, not
      mirror-sharp (prefiltered mip chain, sampled at
      roughness * 4); lowering the SSR 粗さ上限 slider below a
      surface's roughness removes its trace (env fallback) and
      lowers SSR GPU cost.
- [ ] Teleport the camera (scene/camera cut): no ghost reflections
      from the old viewpoint (history reset on forward swing > ~26
      degrees or origin jump > 10 units).
- [ ] Enable DXR reflections: DXR wins over SSR (tri-state w).
- [ ] Previous-color source: at native resolution reflections
      show real scene colors (the trace reads the AA history
      view there; before 9a994c9f it sampled a target that is
      only written by the upscale AA branch). Known gap: under
      DLSS / Bilateral / TSR upscaling the pre-upscale history
      is not refreshed either, so SSR colors can freeze - decide
      whether that combination matters before relying on it.

## 2. Stage 2H reference captures (env-gated)

- [ ] `DX12_REFERENCE_CAPTURE_TEST=1` run writes
      reference_cornell.pfm/.ppm + _albedo/_normal/_direct PFMs
      next to the exe; log line reports PASS. beauty - direct is
      everywhere >= ~0 (the indirect remainder).
- [ ] `DX12_REFERENCE_CAPTURE_FIXTURE=materials` and `=emissive`
      variants write their files; the emissive render is lit only
      by the panel (soft ceiling glow, no point-light hot spot).
- [ ] Re-run cornell twice with the same seed: outputs identical
      (deterministic).
- [ ] `scripts/pfm_compare.py` between the two runs: RMSE 0.
      With `DX12_REFERENCE_CAPTURE_SEED` changed, the outputs
      differ but stay close (the noise floor for A/B gates).
- [ ] Higher SPP visibly reduces noise in the emissive fixture
      (quad NEE + MIS working).
- [ ] `DX12_REFERENCE_CAPTURE_FIXTURE=furnace`: the mirror
      sphere's region mean (scripts/pfm_stat.py over the sphere's
      pixels in reference_furnace.pfm) approaches the wall
      emission 3.0 (GGX energy conservation); the rough-metal
      sphere reads clearly below it.

## 3. Stage 2J probes

- [ ] Two overlapping probes: reflections blend per-pixel
      across the overlap (Route A is compiled into both
      DeferredLightingPS and modelshaderPS); the frame-global
      top-2 selection drives the weights with the global
      environment as the remainder.
- [ ] Probe Intensity now scales the probe contribution.
- [ ] Inside a probe box, reflections sample from the box surface
      (parallax), not from infinity.

## 4. D3D12MA

- [ ] Settings > Advanced GPU shows live D3D12MA stats under the
      checkbox once heaps exist.
- [ ] Toggling the flag ON: VRAM log lines (D3D12MA...) still
      appear ~every 900 frames; no device removal in a soak run.

## 5. Stage 1C shadows

- [ ] Multiple shadow-casting lights: every shadow pass consumes
      its own VG traversal view (no cross-light shadow bleeding),
      passes beyond 4 views fall back cleanly.
- [ ] With VSM (virtual shadow maps) enabled: page passes draw
      the casters of THEIR light (views key by light layer since
      9e5f93fd, not by pass index) - check a scene where the VSM
      directional light is not the last shadow light.
- [ ] Alpha-tested foliage still casts cutout shadows (v2 path).

## Known follow-ups (not blockers)

- The 6-face probe capture pass (see the Stage 2J capture
  design's scoping note) is the one sized piece of the run's
  designs not implemented; CaptureLive is inert until it lands.
- Stage 1C material binning: design + survey landed; the view
  Constants cbuffer has no spare field, so implementation starts
  with growing it (host + HLSL mirror + asserts).
- Probe live capture: cube->equirect conversion landed; the
  6-face scene capture pass is the remaining step.
