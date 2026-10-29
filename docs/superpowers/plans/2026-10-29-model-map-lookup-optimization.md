# Model Map Lookup Optimization

Goal: use the shared resource map to reduce actual repeated searches and editor work on dev_main.

- Add an eagerly maintained inverse category index, rebuilt after binary deserialization; preserve mutation/failed-read behavior and binary compatibility.
- Use inverse lookup for VMD aliases and mapping conflict detection, preserving exact-match priority and ambiguity handling.
- Replace PMX material-morph full mesh filtering with the existing material-to-mesh map. Preserve negative-index all-material operation and offset order.
- Resolve editor model resources once per draw, reference existing names without copying, cache slot counts per loop, and clip single-line mesh tables. Preserve full traversal for variable-height names.
- Verify map mutation/round trips, sparse material work, VMD alias behavior, and actual ImGui clipping in a headless context. Run the existing portable map suites. Windows engine build and visual interaction remain unverified in Linux.

Progress: implementation verified; read-only review found no blocking issues. Publishing to the requested dev_main.

Validation:
- All five portable suites in scripts/test-model-resource-maps.sh passed, including headless ImGui clipping and variable-height fallback.
- Existing skinning_work, renderfeature, temporalhistory, rendermeasurement and stablesampling CPU suites passed.
- Model-map tests passed under AddressSanitizer/UBSan with leak detection disabled. LeakSanitizer itself cannot run under this host's ptrace instrumentation; no leak-check result is claimed.
- Visual Studio project XML and git diff --check passed.
- Windows application build, visual interaction and end-to-end FPS remain unverified.

Review consideration retained: the inverse index covers all categories to support the generic NamesForIndex API, adding hash/vector nodes. Strings are borrowed from owned forward-map keys, avoiding duplication per index; copies rebuild their borrowed pointers and moves preserve ownership. A specialized VMD-only index would use less memory but narrow the reusable API.
