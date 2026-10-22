# Custom TSR 20%: actual application validation, 2026-10-22

## Changes

- TSR uses nine bilinear taps for Catmull-Rom history reprojection. The existing neighborhood clipping, depth rejection, reactive rejection and reset paths remain. The static reconstruction weight floor changes from .002 to .0001, with a .12 positional contribution, to reduce contamination by input samples far from an output pixel.
- Stable reflection sampling now uses the existing low-discrepancy sampler. At input scales of 25% or less and roughness below .25, four reflection samples cover the pixel footprint and GGX lobe. These dimensions use the existing four-dimensional sequence with a separate reflection scramble. Footprint positions intersect the primary surface tangent plane. Other surfaces retain one sample; checkerboard packing remains unchanged. Four samples cost additional reflection and secondary visibility rays.
- The stable NRD specular profile uses 63 main/stabilized frames and eight fast frames. Spatial blur radii are unchanged. The existing stable-profile and sampling toggles control these changes; the sampling label now mentions reflections.

## Actual application checks

No test code was created or changed for this task. Testing used the native DirectX12 application on the attached GPU, through its editor UI and Play mode. Debug and Release executables were rebuilt successfully; the visual UI tests used Release. TSR and reflection shaders compiled successfully, including the reflection path with NRD disabled. Formatting and whitespace checks passed for the changed rendering files. The changed UI label passes its line-scoped formatting check; the existing settings file has unrelated whole-file formatting violations at lines 205, 1019 and 1298, which were left unchanged. Builds also report existing warnings outside this change.

The source scene is `asset/Scenes/rightshaft_test.scene`. A copy at `build/tsr20-live/comparison.scene` was used for repeatable UI checks; the cube was rotated to -50 degrees in this copy so that the character appears in the reflection. The original scene was restored byte-for-byte. Project settings and layout backups are in the same results directory and are restored after testing.

Both baseline and candidate screenshots show 384x205 internal resolution at 1920x1025 output, i.e. 20%, with Custom TSR, checkerboard reflections and NRD enabled. Static character and mirror views used the editor's F focus command from the initial camera orientation. Camera zoom was exercised in both directions. Play mode ran the existing cube rotation timeline; the recorded playback screenshot shows 42.282 seconds and 621.289 degrees, then playback was stopped and the scene returned to its initial angle.

Results:

- Grain is visibly reduced on the flat lower-left mirror area. Reflected silhouette stair steps remain; they have not been eliminated.
- The static character silhouette comparison shows a modest change, not a dramatic recovery of missing geometry. Camera zoom continues to reconstruct the character at 20%.
- The observed Play sequence continued to render the rotating mirror. This does not establish absence of short-lived ghosting or flicker: motion and the reflected silhouette remain the main limitations.
- A candidate mirror-view profiler screenshot reports GPU Render 5.071 ms. This is one observed frame, not an average or a performance guarantee. The baseline profiler screenshot reports 4.524 ms in the character view; the views differ, so those two values must not be interpreted as a measured overhead.
- An exploratory pair of captures of the same flat mirror rectangle (x370..499, y410..489) has RGB difference RMS 0.9974 before and 0.6360 after, in 8-bit screenshot units. Only two captures per version were used, at uncontrolled frame phases and intervals; this is supporting observation, not a statistically reliable temporal-noise reduction percentage.
- The candidate session log confirms NRD initialization at 384x205. No device removal was observed during these UI checks. A full D3D12 debug-queue validation was not performed, and Release UI execution must not be presented as that check.

## Evidence

All files below are under `build/tsr20-live/`:

- `baseline-character.png`, `candidate-character.png`, `character-comparison.png`
- `baseline-mirror-{1,2}.png`, `candidate-mirror-{1,2}.png`, `mirror-comparison.png`
- `baseline-profiler.png`, `candidate-mirror-profiler.png`
- `candidate-camera-move.png`, `candidate-play-rotation.png`
- `candidate-build.log`, `debug-build.log`, shader compilation logs, `final-runtime.log`

The comparison images are crops of actual application screenshots, placed side by side without resampling. The result is an improvement, not removal of all aliasing/noise at 20%. More reflected samples and longer history trade GPU time and responsiveness for stability.
