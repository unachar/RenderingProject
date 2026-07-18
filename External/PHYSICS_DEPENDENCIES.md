# Physics dependencies

The project uses three source-built static physics libraries:

| Backend | Repository revision |
| --- | --- |
| Bullet Physics | `bulletphysics/bullet3` tag `3.25` (`2c204c49e56ed15ec5fcfa71d199ab6d6570b3f5`) |
| Jolt Physics | `jrouwe/JoltPhysics` tag `v5.6.0` (`e77f175595e64cb44218cc9d9d56fc365ad0e36a`) |
| NVIDIA PhysX | `NVIDIA-Omniverse/PhysX` tag `107.3-physx-5.6.1` (`5ca9f472105a90d70d957c243cb0ef36fe251a9f`) |

Run the setup script from the project root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\setup_physics_dependencies.ps1 -Configuration All
```

The pinned source revisions and the required Debug/Release static libraries are
vendored in the parent repository so that GitHub source archives build without
an additional dependency download. The script reuses vendored source when
present; if a dependency directory is empty, it clones the pinned revision.
It configures each library for the project's static MSVC runtime (`/MTd` and
`/MT`) and writes generated libraries below `External/Build`. Physics
dependency builds currently target the project's x64 Debug and Release
configurations.

## Runtime usage

```cpp
auto& physics = entity.Get<PhysicsComponent>();
physics.UsePhysics = true;
physics.UsePhysicsEngine = PhysicsEngine::Bullet; // Bullet, Jolt, or PhysX
physics.UsePhysicsBone = true;                    // PMX rigid bodies and joints
++physics.SettingsRevision;                      // optional; direct edits are also detected
```

Play, Pause, and Stop are available in the center of the editor's top bar.
`Window > 物理設定` exposes the fixed update, substep, time scale, gravity,
backend status, and runtime object counts. The selected entity's inspector
contains its backend-independent rigid-body and PMX parameters.

To exercise all three PMX backends without UI interaction, set
`DX12_PHYSICS_SMOKE_TEST=1` before launching a Debug build. The application
enters Play mode, switches Karen from Bullet to Jolt to PhysX, writes
`Save/physics_smoke_test.txt`, then exits.
