# Build Performance

Measured on 2026-10-14 with Release x64, MSBuild `/m:1` and C++ `/MP4`:

| Operation | Measured Time | Result |
| --- | --- | --- |
| Forward pixel shader, FXC `/O3` | 15.8 seconds | Pass |
| Release Build after native repairs | 213.2 seconds | Pass |
| Release Rebuild | 231.6 seconds | Pass |
| Release Build without edits | 5.9 seconds | Pass |
| Cached custom DXC targets | 4.3 seconds | No shader outputs changed |

The earlier Forward shader exceeded the 60-second bounded compiler check even
after reducing cascade unrolling. Sharing its legacy shadow query and removing
BTDF/BSDF blocks that were unreachable after the existing early return resolved
the optimizer bottleneck. Release optimization remains enabled.

## Changes

- Cascade loops remain loops rather than duplicating the full shadow sampler.
- Custom DXC outputs have individual targets and transitive include dependencies.
  A culling shader no longer rebuilds because an unrelated material shader changed.
  Update the relevant target's Inputs when introducing a new include dependency.
- Release C++ debug information uses `/Z7`, avoiding shared compiler PDB-manager
  coordination. The linker still produces debugging information. C++ optimization
  and link-time optimization are unchanged.
- Required `std::` qualifications were restored in standalone translation units
  and their public headers. Other namespace cleanup was retained. Installed MSVC
  headers were not modified.

Rebuild cleans native and tracked FXC outputs; custom DXC outputs retain their
existing dependency-based cache behavior. The first build after changing targets
or common headers can still recompile several shaders. Normal builds should be
preferred over Rebuild for routine edits.

## Validation

`scripts/test-shader-build-targets.ps1` checks individual outputs, dependencies,
optional NRD handling and preserved optimization settings. Material unit and GPU
regressions run with `scripts/validate-materials.ps1 -Configuration Release
-SkipBuild -Transparency`. Logs and compiler measurements are in `build/build-time`.
