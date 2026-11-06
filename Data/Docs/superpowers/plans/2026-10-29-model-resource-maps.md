# Model Resource Maps Implementation Plan

**Goal:** Shared persistent model maps and VMD humanoid track binding on dev_main.
**Architecture:** Resource-owned name indices, portable binary cache, cached retargeted track bindings.
**Tech Stack:** C++20, Assimp math, existing JsonCpp and DX12 model loaders.
**Spec:** docs/superpowers/specs/2026-10-29-model-resource-maps-design.md

## Global Constraints
- Preserve Karen's PMX path and existing FBX animation playback.
- No GPU/process pointers in persisted metadata.
- Duplicate names retain every runtime index; invalid caches fall back safely.

## Review Focus
- Static derived geometry may have different mesh ordering from Assimp.
- Ambiguous bone aliases must not silently bind to an arbitrary node.
- Import conversion changes cache identity.
- Retarget bindings and user overrides must invalidate all VMD pose caches.
- PMX IK-driven feet cannot be reproduced by FK-only generic humanoid binding.

### Task 1: CPU map and persistence
- [x] Write and run failing tests in tests/model_resource_map_tests.cpp.
- [x] Implement source/models/modelresourcemap.h (generic categories, bounded deterministic serialization, fingerprint-validated cache).
- [x] Run g++ -std=c++20 -Wall -Wextra -Werror -I source tests/model_resource_map_tests.cpp -o /tmp/model_resource_map_tests && /tmp/model_resource_map_tests.

### Task 2: Import integration
- [x] Build bone/material metadata and actual drawable mesh indices in animation/static loaders, expose const maps and manager getters.
- [x] Persist metadata without affecting load success, clear on unload, handle derived geometry separately.
- [x] Add an Assimp-scene fixture to validate runtime index correspondence.

### Task 3: VMD humanoid binding
- [x] Write and run failing alias/retarget tests.
- [x] Implement VRM metadata and common aliases, explicit override API, bind-space conversion for both existing VMD playback paths.
- [x] Add opt-in model test VMD playback for FBX and VRM.
- [x] Run portable tests and available existing CPU tests; inspect integration and report Windows-only checks.

### Task 4: Handoff
- [x] Document lookup API, cache lifecycle, rig mappings, limitations and Windows verification.
- [x] Review diff, run git diff --check, commit and push dev_main without overwriting concurrent work.

## Execution record

Implemented inline on the explicitly requested dev_main checkout. No extra approval gate was added to the authorized implementation. Review was delegated per requesting-code-review skill.

- Map and retarget tests were compiled before new headers existed and failed; implemented headers and reran to green.
- Root target override and torso compensation tests failed before the corresponding helpers existed and passed afterward.
- Review findings fixed: preserve source metadata in derived caches, invalidate revision-2 manifests, use effective 下半身 binding for custom root motion, compensate MMD sibling torso hierarchy in both playback paths.
- Ruling: no generic IK/morph/physics synthesis; support FK track retargeting and report this limit. VMD does not contain the source model's complete bind skeleton; exact foot contact cannot be promised.
- Ruling: metadata cache validates imported layout; it does not bypass GPU/model import. This preserves runtime indices and supports offline readers.
- Four portable suites pass. Five existing CPU suites pass: skinning_work, renderfeature, temporalhistory, rendermeasurement, stablesampling.
- Additional existing simulation_math suite cannot compile unmodified on GCC because source/scene/solarcycle.h:19 uses unqualified isfinite. This file was not modified. Windows/DX12 app build and visual playback are unavailable in this environment.
