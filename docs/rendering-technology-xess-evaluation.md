# Evaluation: Intel XeSS as a selectable super-resolution option

Date: 2026-10-31. Scope: Stage 8 foundation list ("XeSS 選択式 SR
候補"). Facts from Intel's XeSS SDK repository and release
coverage; engine facts from this tree.

## What XeSS 2 offers

- XeSS-SR: temporal AI upscaling. On Intel Arc it uses XMX; on
  every other SM 6.4 GPU (GeForce GTX 10+, Radeon RX 5000+) it
  runs a DP4a path with near-equal quality at a few percent more
  cost. SDK 2.1.0 also opens XeSS-FG (frame generation) and XeLL
  (low latency, requires FG) to non-Intel GPUs.
- Distribution: intel/xess on GitHub, prebuilt libxess.dll +
  headers, under Intel's XeSS SDK license (not MIT) -
  redistribution terms need a license review before shipping,
  same class as the RTXDI note.

## Incumbents in this engine

- DLSS: integrated (DlssUpscaler), best-in-class on the RTX
  development hardware.
- TSR: the engine's own temporal upscaler, vendor-neutral,
  already the default fallback.
- FSR 1: spatial only (RCAS), for the cheapest path.

## Assessment

XeSS-SR's niche here is narrow but real: on AMD/Intel GPUs it
would very likely beat TSR quality at similar cost, where DLSS
is unavailable. On the primary NVIDIA target it adds nothing
over DLSS. Integration shape is DLSS-like (DLL + jitter +
motion vectors + exposure), so the cost is mostly another
UpscaleMode branch in ResolveUpscaleMode and the post chain -
moderate, and it interacts with the SSR previous-color caveat
(DLSS/Bilateral/TSR modes do not refresh the pre-upscale AA
history; XeSS would join that list).

## Recommendation

**Defer.** Adopt only if cross-vendor quality becomes a goal
(AMD/Intel test hardware in the loop) or TSR shows a quality
ceiling the project cares about. XeSS-FG is a separate, larger
decision (frame generation touches presentation and latency
end to end) and is not covered by an SR adoption. Revisit
triggers: a non-NVIDIA test target, or a TSR-vs-XeSS quality
comparison on the same scene via the existing upscale A/B
procedure.
