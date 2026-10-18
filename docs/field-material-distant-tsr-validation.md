# Field materials and distant thin geometry

## Field highlight

The supplied scene is configured as `asset/Scenes/ModelTest.scene`. Its legacy Field was manually assigned the PBR shader (roughness .82, dielectric Fresnel .04), which includes view-dependent sun/environment specular lighting. Its point light is outside the Field, at x34.96, rather than a light authored into the floor texture. The floor texture was inspected separately and contains no bright circular glow.

Changed Field's default shader and the existing Field entry in ModelTest from PBR to Lit. This makes the built-in floor diffuse and removes its PBR highlight. The scene edit replaces only that Field's shaderClass value; other entities, materials and lights are retained. This is an intentional change in the floor's lighting response, not a global specular-lighting change. An explicitly assigned material asset still controls its own PBR appearance.

## Material assignment

`MaterialManager::IsRenderable` previously accepted only meshes and static/animated models. Field and Polygon3D use `SpriteComponent` with `Is3D=true`, so they had no Materials slot and Assign rejected them despite their renderer already supporting resolved materials.

World-space sprites now accept one material slot through the existing assignment, undo, persistence and binding paths. Screen-space sprites keep their existing policy. The integration fixture checks assignment, shared runtime/normal-map resolution, rejection of extra slots and screen-space assignment rejection. No conversion to a different geometry type is required.

## TSR at 20% input

Previous strand tests varied color on a continuous depth surface. A new one-output-pixel strand uses separate foreground/background depths, reproducing actual subpixel geometry that disappears between jittered input samples. The old shader discarded coverage history whenever the foreground sample reappeared, causing a brightness restart.

Added default-on, session-only `遠方の細い物体を安定化`. A depth ridge is recognized only when opposite cardinal neighbors are farther away. A newly observed ridge can accumulate coverage over its previous background. A missing ridge can retain decaying coverage for at most about five consecutive input frames, only with low motion (<.25 output pixel/frame), no reactive rejection, and no nearer occluder. History reset still bypasses reuse.

The existing RG16F detail metadata stores the tag in magnitudes above 1; the remaining magnitude stores confidence and its sign retains the detail direction. Ordinary instability remains in [-1,1]. Tagged state is point-loaded to avoid filtering it with neighboring tags. Ordinary instability processing removes the tag. No additional texture, dispatch or ray is introduced; the enabled path adds metadata and depth reads/arithmetic.

Production shader GPU fixture results at 32x24 → 160x120:

| Geometry case | Temporal RMS, off → on | Coverage RMSE, off → on |
| --- | --- | --- |
| Static camera | .234325 → .0235494 | .319952 → .260523 |
| Slow camera (.1 output pixel/frame) | .222024 → .0230283 | .308663 → .258788 |

Temporal RMS decreases about 90% in both cases. Mean detail contrast remains .0425/.0445; the test separately rejects erasing the strand. A removed static strand has residual .000253482 after 16 frames; the slow-camera removal has zero residual. Reactive-mask and reset cases reject retained thin history immediately. Existing diagonal, bright/dim detail, grazing-plane and broad moving-silhouette checks remain in the suite.

This improves temporal coverage and does not guarantee recovery of a distant object never sampled by the input. The low-motion gate intentionally avoids extending retention to fast motion. The attached scene's camera-motion quality and whole-frame overhead still need direct comparison; the synthetic RMS is not a measurement of that scene.

## Application checks

Debug and Release material asset/runtime validations passed, including the new world-space sprite assertions. Debug and Release normal/features and fallback rendering each passed 84 frames. Debug material and both rendering modes explicitly report D3D12 errors=0; Release has no debug queue. Both application configurations were rebuilt.

The first Debug link failed with LNK1318/PDB LIMIT. The accumulated generated PDB was about 6.35GB; regenerating it with non-incremental linking produced a roughly 138MB PDB and a successful build. No source workaround for the linker was introduced. `validate-materials.ps1` now restores settings files by replacement, matching the other rendering validations. The settings snapshot taken before the first material fixture exited was restored after all application tests and verified by hash.

Logs: `build/field-material-{Debug,Release}.log`, `build/field-rendering-{Debug,Release}.log`, `build/field-relink-Debug.log`, `build/tsr-distant-tests.log`. The production TSR suite contains 13 cases, including static/moving thin geometry baselines and stabilization, and thin-history reset/reactive safety. The original broader P1–P4 acceptance criteria, full-frame performance improvement and direct QA of the attached scene remain unproven by these checks.
