# Evaluation: Stage 4 asset-pipeline candidates (ACL / zstd / KTX2-Basis / ufbx)

Date: 2026-10-31. Scope: the plan's Stage 4 - compare ACL,
zstd, KTX2/Basis and fastgltf/ufbx on small real assets (size /
decode / error) before touching load formats; define version +
back-compat first if a format changes. fastgltf has its own
evaluation. Tree survey for this doc: no animation-compression,
general compression, or supercompressed-texture library is
currently vendored; textures ship as DDS (DirectXTex), models
and animations arrive via assimp into the engine's own formats.

## ACL (Animation Compression Library, MIT)

The industry reference for skeletal animation compression
(error-bounded quantization, used by Unreal). Engine fit: the
only candidate here with a clear quality/size win available -
assimp-extracted clips are stored uncompressed today.
Adoption gates: (1) measure clip memory on the largest current
scene; (2) bake-time integration only (runtime decompresses
into the existing pose path), with a max-error budget per clip
and the uncompressed clip kept as the editor fallback;
(3) format version bump in the scene format with old scenes
still loading (the plan's rule).

## zstd (BSD-3 / GPL-2 dual, Meta)

General-purpose compression for scene/asset containers.
Engine fit: trivially integrable, but the payoff depends
entirely on what dominates load time - if texture upload and
shader compilation dominate (the usual case here, see the
fastgltf evaluation), zstd changes little. Gate: a load profile
splitting IO / decompress / upload / compile; adopt for the
container layer only if IO+decompress is a top cost, with
dictionary-less default settings and a format version field
from day one.

## KTX2 / Basis Universal (Apache-2.0)

Supercompressed GPU textures (UASTC/ETC1S + transcode to BCn).
Engine fit: replaces DDS-at-rest with smaller downloads and
faster mip streaming, at a transcode step on load (or a
bake-time transcode to BCn stored in KTX2). This interacts
with the TextureManager streaming/residency design and with
the RTXTS deferral (same layer). Gates: (1) texture bytes as a
share of scene size and load time; (2) a quality pass on the
normal-map heavy assets (UASTC artifacts show there first);
(3) bake-pipeline change only - runtime keeps consuming BCn
surfaces exactly as today.

## ufbx (MIT / public-domain style license)

Single-file FBX loader. Engine fit: FBX arrives via assimp
today; ufbx would matter only if FBX (not glTF) sources
dominate and assimp's FBX path misbehaves - no such evidence.
Deferred with fastgltf under the same parity-harness gate.

## Recommendation

Sequence by evidence: profile load and memory first (the same
profile gates Stage 8's DirectStorage). ACL is the strongest
standalone candidate and can proceed on its own bake-time
track whenever clip memory shows up in that profile; KTX2 is a
pipeline project, not a library drop-in; zstd and ufbx wait
for their profiles.
