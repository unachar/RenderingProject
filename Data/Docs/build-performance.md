# Build Performance

## Further optimization, 2026-10-26

Latest Release x64 measurements on the same machine, with four MSBuild nodes:

| Operation | Measured time | Result |
| --- | --- | --- |
| Full Rebuild | **129.325 seconds (2 min 9 sec)** | Pass |
| Build without edits | **1.589 seconds** | Pass |
| Native compilation before shared project PCH | 141.159 seconds | Pass, diagnostic run |
| Native compilation with shared project PCH | 84.592 seconds | Pass, diagnostic run |

The full build is about 52% faster than the preceding 271.453-second result.
All **189 C++ object files, 42 custom DXC outputs and 68 FXC outputs** have modification times
after this run began; `build/build-performance-next/full-output-coverage.json`
records that none were missing or stale. Prebuilt SDK libraries are reused.

The native compiler profile showed that front-end parsing dominated compilation.
`pch.h` now precompiles the frequently repeated ECS/world and renderer declarations
and `<fstream>`. Individual sources retain their explicit includes. The project
enables the shared declarations with `RENDERINGPROJECT_BUILD_SHARED_PCH=1`;
standalone tools that use the lightweight PCH do not enable them. Syntax checking
the existing procedural GPU test outside the application project passes.

`ParallelBuild.targets` dispatches the existing DXC targets into distinct MSBuild
project instances, preserving their original commands, conditions and dependency
checks. Four MSBuild nodes allow concurrent jobs. With one node, the original
serial DXC targets remain available. `EnableParallelShaderBuild=false` disables
the new parallel scheduling. FXC uses the built-in MultiToolTask scheduler with
`ParallelFxcJobs=4`. Native C++ retains its original `/MP` mechanism: enabling the
native MultiToolTask alternative measured 89.013 seconds versus 84.592 seconds,
so that alternative was not adopted. Runtime optimization flags are unchanged.

VC Clean filters generated outputs outside IntDir/OutDir. Registering the shader
files with FileWrites alone did not reliably remove them: an intermediate run
left the two ShadowCacheCopy outputs cached. The explicit CleanCustomShaderOutputs
hook now deletes registered `.cso` outputs before CoreClean. The reported final
129.325-second result includes this fix and regenerates every custom shader.

The final run spent approximately 83.7 seconds in native compilation, 25.8 seconds
in the parallel DXC phase, 17.3 seconds in FXC, and 1.7 seconds in linking. DXC Exec
task durations overlap across workers and must not be added as wall-clock time.
Native compilation alone still exceeds 60 seconds, so a one-minute full build was
not achieved.

Reproduce the fastest measured configuration with:

```powershell
pwsh -File scripts/measure-build.ps1 -Configuration Release -Target Rebuild -BuildNodes 4
pwsh -File scripts/measure-build.ps1 -Configuration Release -Target Build -BuildNodes 4
```

The final run IDs are `Release-Rebuild-20261026-213111` and
`Release-Build-20261026-213528`. Text logs, binary logs and JSON measurements are in
`build/build-performance`; native profiles and additional checks are in
`build/build-performance-next`. Worker reuse is disabled so completed measurements
do not leave MSBuild worker nodes running. Parallel workers require local process
communication; the constrained tool sandbox blocked that communication, so the
successful parallel measurements ran outside that sandbox. An IDE build using
one MSBuild node may take longer than the four-node command above.

Both shader dependency checks, parallel target coverage, distinct output checks,
and the standalone PCH syntax check pass. This change does not modify shader
evaluation or gameplay code. The existing GPU parity failure and incomplete
runtime validation described below remain limitations; they were not rerun as
passing tests for this build-only follow-up.

## Release x64 regression repair, 2026-10-26

On this machine, Visual Studio 18 MSBuild x64, `/m:1`, with the normal compiler
environment and all project C++ and shader outputs regenerated:

| Operation | Measured time | Result |
| --- | --- | --- |
| Release x64 Rebuild | **271.453 seconds (4 min 31 sec)** | Pass, below 300 seconds |
| Release x64 Build without edits | 1.426 seconds | Pass |
| DeferredPS, loop annotations only, FXC `/O3` | More than 120 seconds | Timed out |
| DeferredPS, loop annotations only, DXC `-O3` | 57.767 seconds | Pass |
| DeferredPS, shared noise evaluation, FXC `/O3` | More than 120 seconds | Timed out |
| DeferredPS, shared noise evaluation, DXC `-O3` | 6.394 seconds | Pass |
| DeferredLightingPS, DXC `-O3` | 2.371 seconds | Pass |

The slow baseline was interrupted; it is not a completed full-build measurement.
The full-build result is one measured run, not a guarantee under other machine loads.
Prebuilt external SDK libraries are reused, as in the normal solution Rebuild.

The regression was in shader optimization. The new procedural basis dispatch
was duplicated by separate fractal and domain-warp call sites, with nested
neighborhood loops also eligible for expansion. FXC stalled on material shaders,
including the `modellightingPS.hlsl` entry point that includes `modelshaderPS.hlsl`.
FXC also spent over 69 CPU seconds on DeferredLightingPS before that measurement
was stopped.

`noise.hlsl` now shares the basis evaluation for unfractalized noise and shares
fractal evaluation across the three domain-warp samples. Loop annotations limit
code expansion. The basis types, sample counts, seed calculations and output
normalization are preserved. `MaterialShaders.targets` compiles all eight material
entry points and DeferredLightingPS with DXC, SM6.0 and `-O3`, consistent with the
existing DXC runtime compiler. C++ optimization settings are unchanged. Win32
retains its original compiler configuration; the measured target is x64.

Each migrated shader tracks its transitive local includes. Custom shader outputs
are registered with MSBuild `FileWrites`, so Clean/Rebuild removes them instead of
silently reusing the custom DXC cache. This differs from the historical measurement
below. The full run spent approximately 168.5 seconds in CL, 67.9 seconds in Exec
(DXC and build commands), 30.4 seconds in FXC and 3.6 seconds in Link.

Reproduce the measurements and dependency checks with PowerShell:

```powershell
pwsh -File scripts/measure-build.ps1 -Configuration Release -Target Rebuild
pwsh -File scripts/measure-build.ps1 -Configuration Release -Target Build
powershell -File scripts/test-material-shader-build.ps1
powershell -File scripts/test-shader-build-targets.ps1
```

The measurement command records JSON, a text log and an MSBuild binary log in
`build/build-performance`, and fails if compilation fails or exceeds 300 seconds.
The recorded full run is `Release-Rebuild-20261026-204503`; the no-edit run is
`Release-Build-20261026-205227`. Compiler probes and runtime validation logs are in
`build/build-performance-20261026`.

All 42 custom DXC outputs exist and have timestamps after the full rebuild began;
`output-coverage.json` records that check. Both shader dependency checks and the
Release material asset unit tests pass.

`validate-materials.ps1 -Configuration Release -SkipBuild -Transparency -Parallax`
timed out at its three-minute limit on both attempts. In this script, `-Parallax`
selects the 12-frame POM mode instead of the 107-frame transparency mode. The
second run reached the first rendered frame and passed the production GBuffer
capture and oblique-view POM coordinate-change checks, but did not finish the
suite. Full runtime/visual validation remains incomplete. These timeouts do not
invalidate the successful build measurement, and are not classified as an
unchanged baseline issue.

The procedural GPU/CPU parity suite fails at White noise (`basis=4`, `fractal=0`,
maximum error `0.737312`). The unchanged HEAD version of `noise.hlsl`, executed
with the same test binary and copied original shaders, reproduces that exact
failure. The complete parity suite therefore remains unverified beyond that
existing failure; this build repair does not claim to fix it.

## Historical measurement, 2026-10-14

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
