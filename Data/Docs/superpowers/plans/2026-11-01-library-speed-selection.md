# External library speed selection

Goal: Compare installed libraries against existing implementations and adopt measured improvements without changing rendering quality or asset compatibility. The user's latest instruction considers both speed and size for compression, and both quality and cost for rendering. Unimplemented libraries are implementation targets.

Architecture: Keep proven existing paths until comparisons justify replacement. First address shader/texture cache zstd: keep beneficial-only compression as the balanced default given the measured 32%/64% savings; DX12_COMPRESS_CACHES=0 selects raw storage for speed-sensitive uses. Both readers keep supporting compressed data. Do not rewrite existing caches during loading. This supersedes the initial speed-only assumption.

Tech stack: C++/Windows, existing zstd, DirectXTex, native optimized benchmarks.

- [x] Measure raw versus zstd on isolated copies of real caches.
- [x] Add tests for balanced compressed storage, explicit raw storage, roundtrips and replacing a compressed texture with raw data.
- [x] Implement a shared storage preference and preserve both readers and atomic writes.
- [x] Run codec/storage/DXC tests and benchmarks, inspect formatting and diff.
- [x] Record evidence and audit remaining External candidates; distinguish real integrations, placeholders and unmeasured proposals.

This bounded change does not establish that all External candidates are faster. GPU rendering alternatives require equivalent scene/quality measurements before default changes.

User scope clarification: implement currently missing libraries as evaluation candidates, then select using size/speed or quality/cost. Additional first implementation: opt-in Tracy CPU frame/update/draw traces, default disabled. SDK disabled/enabled probes passed. Both full Debug builds passed with the environment workaround /Z7; both renderer variants passed 3-frame startup/shutdown smoke. The existing standalone ShaderMakeBlob Release path masked NRD's Debug dependency; exclude that path when NRD is enabled. OpenColorIO runtime/license copying and Tracy license copying are implemented and verified. Remaining libraries and Tracy GPU/collector validation are still open; the overall goal is active.
