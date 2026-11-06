# Procedural Studio noise implementation plan

**Goal:** Implement the requested missing noises on dev_main in noise.h / noise.hlsl and expose them in Procedural Studio.

**Architecture:** Keep existing Basis and FractalType numeric IDs. Append missing basis methods and multifractal variants. Mirror CPU and GPU algorithms, with periodic sampling for tileable textures. Existing fBm, Ridged (already feedback-weighted ridged multifractal), Domain Warp, Perlin, Value and Worley remain compatible.

**Tech stack:** Header-only C++17 noise, HLSL compute shaders, ImGui.

**Decisions:** Voronoi returns nearest feature ID; Cell returns lattice-cell ID; White hashes subcell samples. Local Random is a local random-phase kernel sum. Chevron is a seeded chevron pattern. Lattice-Free uses random Fourier waves without spatial lattice interpolation. Wavelet uses compact zero-mean wavelet kernels (not Pixar's precomputed tile algorithm). Curl exposes the full vector and uses one bounded component for scalar textures. New directional controls are shared with the GPU. All variants are available to texture, volume and model generation through the existing evaluator.

## Tasks

- [x] Add a standalone failing noise regression executable covering added enum values, range, determinism, seed variation, periodicity, continuity, curl divergence and multifractal differences.
- [x] Implement distinct algorithms in noise.h, preserving existing IDs and evaluator behavior.
- [x] Mirror algorithms in noise.hlsl; wire controls into GPU constants and Studio selectors.
- [x] Run CPU regressions, existing procedural tests where supported, HLSL compilation/parity checks where available. Document platform limits.
- [ ] Review diff and publish a normal commit to dev_main using an expected-head lease.

## Review focus

- Tile seams in simplex, spectral waves and directional kernels.
- Distinct cellular interpretations and nonconstant seed-dependent outputs.
- Curl is a vector curl, not scalar turbulence.
- Existing numeric IDs, root constant size and CPU/GPU control packing.
- No placeholder aliases for requested algorithms; clearly document variant definitions.

Validation: standalone CPU regression; texture/volume/terrain integration; geometric patterns; tree normals; 2,801 simulation checks; 10,752 legacy samples; DXC Generate/Normals/Preview and cloud/deformation include consumers. D3D12 runtime parity and full Windows build remain platform-limited. Independent review reported no blockers. See docs/procedural/studio-noises.md for algorithm variants and reproduction commands.
