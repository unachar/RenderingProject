# Stage 4 evaluation — fastgltf as a glTF fast path

Question from the plan: judge asset-pipeline libraries (ACL /
zstd / KTX2 / fastgltf / ufbx) by real-asset comparison. This
note covers fastgltf; the others remain unevaluated.

## Current state (surveyed 2026-10-31)

- All model loading goes through assimp at runtime:
  `Model::LoadAssimpModel` / `ImportModelScene` (modelmanager,
  animationmodel), with assimp post-processing doing real work —
  left-handed conversion (`aiProcess_ConvertToLeftHanded`),
  tangent generation, and the animation/skin extraction the
  engine's formats are built from.
- assimp covers every format the asset tree uses, not just glTF.

## What fastgltf would change

- fastgltf (MIT, header + small cpp, SIMD JSON/base64 parsing)
  loads .gltf/.glb dramatically faster than assimp's glTF path
  and with a much smaller surface. It does NOT do what the
  engine relies on assimp for: no format breadth, no
  post-process graph — handedness conversion, tangent-space
  generation, and node/skin/animation normalization would have
  to be reimplemented to match assimp's output bit-for-bit,
  or the two paths would produce subtly different geometry
  (winding, tangents) for the same asset depending on extension.

## Recommendation

**Defer.** The deciding evidence does not exist yet: no profile
shows assimp parsing dominating load time on the current asset
set, and correctness parity (not parse speed) is the risk.

Adopt only when all of these hold:

1. A load-time profile on a representative glTF-heavy scene
   shows parsing (not texture upload / shader compile) as the
   bottleneck.
2. A parity harness exists: for a fixed model set, the fastgltf
   path's positions/normals/tangents/indices/skin weights and
   animation curves match the assimp path within tolerance
   (vertex-data diff, automated).
3. The fast path is glTF-only and falls back to assimp for
   everything else, behind the usual default-OFF flag until the
   A/B (load ms, parity) passes.
