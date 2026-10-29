# VMD Humanoid Leg IK

User authorized extension of FBX/VRM VMD playback to IK on dev_main.

Implement a portable analytic two-bone leg solver, binding the existing human/VMD map to thigh, knee, ankle and optional toe nodes. Preserve local translations/scales and the PMX solver. Source VMD IK offsets are interpreted relative to target bind-foot/toe positions, scaled with the existing retarget scale; root motion transforms targets, center/groove motion moves the pelvis without moving planted feet. Foot IK parent offsets are supported. Full source bind skeleton is absent in VMD, so exact cross-model proportions/pivots are approximate.

Cache source tracks and switches when single/blend/layer animation bindings rebuild. Resolve Japanese fullwidth/ASCII IK spellings, allow later-layer per-track overrides, respect independent foot/toe switches. Blend active targets against the current FK pose when only one motion supplies enabled IK. Run after FK/morph and before skinning upload. Missing or invalid leg chains and degenerate bones keep FK; unsupported nonuniform/sheared/reflected joint scales must not introduce corrupt transforms.

1. Write failing portable tests for analytic reach, knee direction, unreachable/degenerate goals, bind scales, foot/toe orientation, target offset conversion and disabled/blended IK.
2. Implement CPU pose/solver and track-alias selection helpers.
3. Integrate cached primary, secondary and layered IK channels with AnimationModelResource. Invalidate on model/animation/map/scale changes.
4. Exercise actual production solver through portable tests, existing map/VMD suites and CPU regressions; review diffs. Windows app build and visual playback are unavailable here and must remain explicit.
5. Update docs and publish to dev_main with expected-parent protection.

## Execution and verification

- Implemented portable solver/offset/blend math and cached alias/layer channel selection, integrated through animationhumanoidik.cpp before skinning upload.
- Seven suites in scripts/test-model-resource-maps.sh passed, including synthetic IK math and the repository's actual Moca VRM node-transform fixture for both legs/toes. Five existing CPU regression suites passed: skinning_work, renderfeature, temporalhistory, rendermeasurement, stablesampling.
- IK tests passed under AddressSanitizer and UBSan with leak detection disabled (host ptrace instrumentation prevents LeakSanitizer use).
- Review found a globally rigid leg could hide a compensating nonuniform parent. Added a reproducer, observed failure, then validated every local/global ancestor transform before mutation; regression now passes and FK local matrix remains unchanged.
- Project XML and git diff --check passed. Reviewer found no other blocking issues.
- Ruling: generate only humanoid leg/toe IK. Targets use target bind-foot/toe offsets because VMD lacks the complete source rig; arbitrary chains, source-specific pivots and terrain contact remain outside this change. Karen's PMX solver is untouched.
- Windows compilation, final imported GPU skinning, visible foot contact and user interaction are unverified in this Linux environment. No claim of identical cross-model motion is made.
