# Implementation ledger — 2026-10-14-asset-thumbnails

Base: 0d72d8f2. Scope and plan approved in chat; inline execution authorized.
Ruling: work in the current dev_main checkout, as the user approved this target and inline execution. No push or merge.
Pre-flight: Tasks 1–6 share AssetHandle, revision ticket and fence-retirement contracts. Task 3 borrowed SRVs must never enter the owned descriptor pool. Task 4 must give each in-flight draw independent lighting constants. Task 6 must count pending retirements against memory.
Task 1: complete. CPU state/queue, obsolete tickets, delete/recreate, dimensions, camera fit and retry scheduling tests passed.
Task 2: implemented. Finite 512-slot pool in the existing heap, owned leases, resource retention through submission fences, delayed reuse; allocator CPU tests passed.
Task 3: implemented. Clipped table rows, GUID requests for visible/selected cells, aspect-preserving real images, old icon fallbacks, size/refresh controls. CPU decode is off the UI; ordinary command-list uploads have no thumbnail GPU wait.
Task 4: implemented. Shared production PBR preview path, independent constants and studio context, edit/save dirty notifications.
Task 5: implemented. Assimp CPU worker, node transforms, bounds fit, submesh materials and textures, embedded images; preview never registers a world entity.
Task 6: implemented. Source/dependency/version key, assets-external DDS cache, nonblocking readback/save, LRU, admission budgets and deferred retry. Transient resources release after fences/save completion.
Task 7: implementation and core validation complete. Debug and Release builds, CPU tests, Thumbnail GPU sequence and existing Material Debug GPU regression passed. Debug graphics errors=0. Final logs and remaining manual/load-test limits are in asset-thumbnail-validation.md.
Review: independent reviewer identified actual mesh dependency tracking and capacity retry gaps. Fixed using recorded Assimp I/O paths and a tested defer/retry state.
Ruling: a Debug dump identified Assimp::IOSystem::PushDirectory crossing incompatible Debug/Release STL layouts. Replaced the C++ handler with aiFileIO C callbacks and added a Debug-CRT import regression test. No vendor header change remains.
Ruling: GUID display text contains a colon, invalid for ordinary Windows cache filenames. Cache filenames use 16 hexadecimal digits; regression test added.
Ruling: Shader-visible descriptor heaps cannot be CopyDescriptors sources. Recreate the borrowed texture SRV while sharing its existing resource.
Ruling: MSVC 14.51.36231 produces LNK1103 across multiple /GL + /Z7 objects. An affected-toolset-only Release x64 workaround retains /O2 and symbols but disables whole-program optimization. Default Release build then passed.
