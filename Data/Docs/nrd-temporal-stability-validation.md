# NRD / low-resolution temporal stability

## Scope and input contract

The reported GI bounce noise and grazing shadow shimmer occur in both Custom TSR and NIS + TAA. The integration uses NRD 4.17.3: REBLUR diffuse radiance in packed checkerboard layout, SIGMA shadow penumbra at full internal resolution, and full-size motion, normal/roughness and view-depth guides. At 1280x720 output and 29% input the internal size is 371x209. Increasing the checkerboard texture width alone does not add traced samples.

`NrdDenoiser::Execute` now checks these actual resource extents before dispatch. The checkerboard parity remains BLACK; NRD and tracing alternate it using the same frame sequence. Motion remains normalized previous-minus-current UV without projection jitter, with explicit `(1,1,0)` scale. The existing matrix and jitter conventions were checked against the installed SDK and retained.

## Independently reversible changes

- `NRDの時間・安定化設定を固定`: opts the raytracing-owned signals into an explicit 16.666667 ms denoiser reference interval. The old zero value let NRD's private wall clock affect resolve/anti-lag weights. This reference interval does not change simulation time or force rendering to 60 fps. The candidate profile also uses disocclusion threshold .015, SIGMA stabilization 7 / plane sensitivity .025, and diffuse accumulation 63 / fast-history sigma 2.5. Other settings, hit-distance packing and ray counts are retained. Other NRD owners keep their previous policy unless explicitly opted in.
- `GI・影の低偏差サンプリング`: uses screen-stable scrambled R2 sequences for cosine-hemisphere samples and an eight-frame shadow pattern. The shared shadow/AO tracer also uses a separate long-period AO domain when AO is enabled; the isolated lighting fixture disables AO, so it does not establish AO quality improvement. Emissive selection and triangle coordinates jointly use four R4 dimensions, avoiding correlation between selected triangles and sampled surface areas. Checkerboard pixels advance their sequence on traced frames. This is low-discrepancy sampling, not a blue-noise texture. The original hash sampling is retained when disabled.
- `浅い角度の平面履歴を安定化`: Custom TSR only. Reuses four depths from the existing 3x3 neighborhood. A symmetric affine depth gradient allows a one-input-pixel depth footprint in history validation. Curvature, sky and excessive gradients retain strict rejection. The toggle invalidates TSR history; no extra texture or dispatch is added.

Changing either raytracing toggle invalidates the shared ray history configuration. Camera/resolution/scene/lighting invalidation continues through the existing temporal history contract. The production shaders share the sampling helper with CPU checks, and shader build dependencies include the helper.

## Repeatable checks

`scripts/test-stablesampling.ps1` checks replay, sample bounds, prefix means, checkerboard sequence coverage and conditional emitter/barycentric means. It compiles with /W4 /WX.

`scripts/test-temporalupscale.ps1` compiles the production shader as cs_6_5 and executes it on the GPU. Existing diagonal, bright/dim fine-detail, removal, motion, reset and reactive-mask checks are retained. A new continuous sloped-depth strand reproduces false history rejection: temporal RMS .284062 with slope handling off versus .030093 on (89.4% lower), coverage RMSE .315001 versus .207816, and removal residual .000393027 after 16 frames. This synthetic test models a continuous plane; it is not a measurement of the user's scene.

`scripts/measure-temporal-lighting.ps1` constructs an isolated emissive/shadow fixture and compares legacy, fixed NRD, fixed NRD + sampling, all changes, and exact replay. Each run uses 512 frames: TSR/NIS + TAA, ordinary/grazing views, 96 warmup and 32 measured frames per case. Receiver samples are projected from a fixed world-space grid and validated on the actual floor plane. Noise measurements use the GI red component, shadow visibility X, and final-image Rec.709 luminance; hashes cover the complete image channels. It records temporal RMS/means, image hashes, frame timing and PNGs. Settings files are restored by replacement, including on failure.

Timing from this readback fixture is diagnostic: synchronous captures affect CPU pacing. It does not prove an average-fps or 1% Low improvement. The fixture disables irradiance cache and ReSTIR to isolate NRD and sample generation. Its exact replay result does not establish determinism for those separate paths, arbitrary scenes, other GPUs or driver versions. Static camera cases do not establish absence of moving-scene ghosting.

SIGMA's installed temporal shader explicitly bypasses stabilization for hard-shadow regions. Low-resolution geometric coverage can therefore shimmer even with valid NRD inputs. The TSR plane change addresses one downstream rejection source; it cannot recover every occluder absent from the input.

## Integrated measurements

`build/temporal-lighting/20261018-070158-comparison.json` contains all five profiles. Every case passed the receiver coverage and 32-frame sample-window checks. Stable/replay hashes match exactly for all measured GI, shadow and final-image frames.

| View | GI RMS, legacy → all changes | Shadow RMS | Final-image RMS |
| --- | --- | --- | --- |
| TSR, ordinary | .007959 → .007906 | .033858 → .035298 | .001048 → .001136 |
| TSR, grazing | .013501 → .011911 | .071256 → .069795 | .014394 → .002238 |
| NIS + TAA, ordinary | .007228 → .008802 | .032898 → .032939 | .007732 → .006830 |
| NIS + TAA, grazing | .014045 → .012446 | .063836 → .060736 | .018480 → .018285 |

The target grazing TSR final image improves by 84.5%. The isolated sampled profile (without the TSR depth change) has exactly the same GI/shadow values and final RMS .014156. Thus the large final improvement comes from avoiding false TSR rejection on a continuous plane, rather than a large improvement in SIGMA itself. The NIS grazing shadow improves by 4.9%, but its final image changes by only about 1.1%. Ordinary-view GI noise increases in the NIS case, and the ordinary TSR final RMS increases by about 8.5% from a small baseline. Do not describe the result as universally lower noise. GI means change by -0.32%, +0.45%, +0.07%, +1.98% respectively; shadow means remain within .001 absolute. No extra rays are introduced.

Stable GPU frame means were 1.571 / 1.555 / 1.444 / 1.428 ms, versus legacy 1.570 / 1.561 / 1.459 / 1.436 ms. Replay timing differs despite identical pixels. These small differences do not establish a performance improvement, and the report deliberately keeps `AdoptionProven=false` for the original broader phase acceptance criteria. The TSR change adds arithmetic using existing depth loads; NRD settings change weights/history limits without adding passes or resources.

## Final regression verification

Final session64391 rebuilt Debug and Release after the shared NRD owner default was scoped to opt-in, and passed both normal/features and fallback rendering (84 frames each). Each configuration also passed the dynamic GI fixture (218 frames): emission edits, moving occlusion, zero emission, emitter motion, texture emission, distance and render-mode switching. Both Debug rendering modes and the Debug GI run explicitly report D3D12 errors=0. Release runs report the debug queue unavailable; they do not establish debug-layer validation independently.

Logs: `build/nrd-stability-{Debug,Release}.log`, `build/nrd-stability-gi-{Debug,Release}.log`, `build/tsr-planar-tests.log`. The eight TSR GPU cases and CPU stable-sampling checks pass. Both application configurations are current and no validation job remains active. The attached scene's actual camera motion, grazing moving shadows, AO quality and frame pacing still require scene-level comparison; those are not established by the static fixture.
