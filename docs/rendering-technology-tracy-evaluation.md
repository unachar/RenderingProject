# Stage 3 evaluation — Tracy profiler (optional detailed tracing)

Question from the plan: can Tracy be added as an *optional*
detailed trace alongside the existing CSV/GPU measurement, which
stays the source of truth for the A/B runbook numbers?

## Facts (checked 2026-10-31)

- Project: wolfpld/tracy, actively maintained (manual current to
  2026, releases past v0.12). License: BSD-3-Clause — compatible
  with this repo's vendoring practice.
- Client integration: vendor `public/` (tracy + client headers),
  compile `TracyClient.cpp` into the app, define `TRACY_ENABLE`
  project-wide. With the define absent the macros compile to
  nothing, so a default build pays zero.
- Runtime model: the client streams to a separate profiler
  server over TCP (port 8086). `TRACY_ON_DEMAND` makes the
  client idle until a server connects — the recommended mode for
  shipped/game builds and the only acceptable mode here.
- GPU: `public/tracy/TracyD3D12.hpp` provides a D3D12 context
  (timestamp queries on a queue + collect step), so GPU zones can
  mirror the engine's existing pass boundaries.
- Data: zones, frames (FrameMark), plots, messages, memory
  events; the server can export zone statistics to CSV, which
  keeps the runbook workflow intact.

## Fit with this engine

- The engine already has a pass profiler with named passes
  (RenderGraph / ScenePasses) and GPU timestamp queries — the
  natural zone source: one Tracy zone per named pass, names
  shared, no second taxonomy.
- FrameMark goes at the end of the main frame loop;
  `TracyD3D12Context` is created on the direct queue after device
  init and collected once per frame.
- Overlap risk is low: Tracy replaces nothing. The CSV/GPU
  numbers remain authoritative for A/B decisions; Tracy is for
  interactive diagnosis (spikes, wait chains, CPU/GPU overlap).

## Recommendation

**Adopt as opt-in, later, in this shape** (not landed in the
500-push run — it touches the project file and the frame loop,
and deserves a verified build of its own):

1. Vendor `External/tracy/` (public/ + TracyClient.cpp) with a
   VENDORED.md pinning the release tag.
2. Build: TracyClient.cpp compiled always; `TRACY_ENABLE` +
   `TRACY_ON_DEMAND` defined only by an opt-in build
   configuration / environment switch.
3. Instrument: FrameMark + CPU zones around the existing frame
   phases; GPU zones fed from the pass profiler's boundaries
   through one D3D12 context.
4. Verify: default build unchanged (no zones, no network),
   opt-in build connects and shows the frame/pass timeline.
