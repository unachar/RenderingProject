# GPU-driven rendering phase 2

## Scope

This phase optimizes the command-compaction compute shader already used by the
shadow and velocity passes. It does not change material shading or raster output.

## Implemented changes

### Cooperative object visibility test

All indirect commands in one dispatch belong to the same entity/model and share
the same local AABB, world matrix and view-projection matrix. The previous shader
repeated the eight-corner homogeneous clip-space test once for every mesh command.
The new shader evaluates the object AABB once per 64-command thread group and
shares the result through `groupshared` memory.

The rejection test remains conservative: the object is rejected only when all
eight corners lie outside the same D3D clip-space plane.

### Group-level command reservation

The previous shader called `InterlockedAdd` once per visible command. The new
shader counts the valid lanes in each 64-command group and reserves one contiguous
range with a single atomic operation. Threads then write to deterministic offsets
inside that range.

This changes global atomic traffic from approximately:

```
visible command count
```

to:

```
ceil(visible command count / 64)
```

### Vectorized command copies

Indirect records are copied as `uint4` blocks where possible, followed by a DWORD
tail. This preserves arbitrary DWORD-aligned command strides while reducing the
number of ByteAddressBuffer load/store instructions.

## API invariants preserved

- The count buffer remains a 32-bit command count consumed by `ExecuteIndirect`.
- The argument and count buffers remain transitioned to
  `D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT` before execution.
- `MaxCommandCount` continues to limit the GPU-produced count.
- The existing direct-draw fallback remains unchanged.
- Command record order between thread groups is not guaranteed, but draw order is
  irrelevant for the opaque shadow and velocity workloads using this path.

## References

- Microsoft, **Indirect Drawing** — command signatures, root arguments and
  GPU/CPU command generation.
- Microsoft, **ID3D12GraphicsCommandList::ExecuteIndirect** — count-buffer
  semantics and required resource states.
- Akenine-Möller et al., **Real-Time Rendering, Fourth Edition**, visibility and
  bounding-volume culling chapters.
- Greene, Kass and Miller, **Hierarchical Z-Buffer Visibility**, SIGGRAPH 1993 —
  hierarchical visibility rejection principles used as the basis for a later
  Hi-Z occlusion phase.

## Next phase

The next substantial architectural step is a depth-pyramid/Hi-Z pass and an
instance-level GPU Scene buffer. That requires an explicit depth SRV lifecycle,
mip generation, previous-frame validity handling and conservative projected-bound
tests; it should not be folded into the current command-copy shader without those
resources and synchronization rules.
