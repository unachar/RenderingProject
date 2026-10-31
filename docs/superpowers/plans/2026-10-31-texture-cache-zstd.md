# Texture Cache zstd Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Preserve legacy DDS cache reads and add beneficial-only zstd sidecars with bounded decoding and unchanged GPU texture bytes.

**Architecture:** Extract filesystem operations into source/assets/texturecachestorage.h; header/frame/checksum handling lives in texturecachecodec.h. Existing texturecompression.inl retains key, BC encoding, queue, settings and counter ownership and delegates only SaveCache/TryLoadCached storage. Reuse existing static zstd C implementation; no DXC codec refactor.

**Tech Stack:** Windows x64 C++23 product, MSVC C++17 native tests, DirectXTex, zstd1.5.7.

**Spec:** docs/superpowers/specs/2026-10-31-texture-cache-zstd-design.md (user approved2026-10-31).

## Global Constraints

- Existing key, encoder version, DDS flags, BC4/5/7 and GPU/mip/sRGB bytes unchanged. Legacy .dds preserved. No bulk conversion/deletion, UI, dictionary or worker change.
- New path rawPath+.zst; 32byte little-endian uint64 header: magic0x003154535A534444, rawDDSbytes, storedBytes, FNV1a64(rawDDS). Raw target136bytes..256MiB; larger raw assets use existing DDS path.
- zstd level3; write sidecar only if header32+frame < rawDDSfile. Allocation/compression/staging/write failures fall back to old save path best-effort.
- Validate sizes before allocation, file/frame exact end, known matching content size, decoded length and checksum; reject concatenated/trailing/skippable/unknown-size frames. Failed candidates never published. Sidecar miss→legacy DDS→source regeneration.
- Same-directory PID/TID temp files, close success then atomic MoveFileExW. Keep settings, counters and background behavior. Failure preserves caller image/metadata; corrupt-file eviction follows existing cache behavior, no valid-file removal on OOM.
- Inline current workspace, preserve all unrelated/user changes. Git metadata read-only: no commit/push/worktree. Match standalone /MT and product /MTd Debug.

## Review Focus

- Valid sidecar failing allocation must fall back without being classified as corrupted and removed.
- Staging estimates must reject huge input before making an additional DDS copy; larger valid assets still save/load legacy DDS.
- Corrupt sidecar plus valid legacy DDS must hit once and preserve exact metadata/BC bytes.
- Concurrent writes/reads must yield complete candidates or misses; cache-file eviction must not delete source assets.
- Existing BC counters mean resident BC bytes, not new disk savings; Settings.Enabled/key0 retain their existing behavior.

### Task1: Storage codec and actual texture-cache integration

**Files:** source/assets/texturecachecodec.h, texturecachestorage.h, texturecompression.inl; tests/texture_cache_storage_tests.cpp; scripts/test-texture-cache-storage.ps1; DirectX12.vcxproj/.filters.

**Interfaces:** TextureCacheStorage::Load(rawPath,TexMetadata&,ScratchImage&) and Store(rawPath,const ScratchImage&) return bool. CompressedPath(rawPath) returns sidecar path. TextureCacheCodec exposes Header/ParseHeader/TryEncode/Decode; Decode returns Success/Invalid/AllocationFailure so storage can distinguish corruption from resource failure.

- [ ] Extract legacy storage without changing behavior; native harness includes actual production texturecompression.inl with only unrelated source-import helpers stubbed. Verify legacy DDS/Enabled/key0/counters initially GREEN.
- [ ] Add compressible-BC fixture that expects .dds.zst and unchanged reconstructed BC bytes; run RED against legacy extracted Store.
- [ ] Add codec and bounded Load/Store; support advantageous-only save, legacy fallback, atomic write and publishing only successful staging. Run native tests GREEN.
- [ ] Extend native tests: header overflow/caps, truncation, bad magic/checksum/frame, unknown size, concatenation/trailing/skippable; invalid DDS after valid decompression; valid sidecar and missing/corrupt sidecar+legacy cases; raw fallback/no stale outputs; concurrent store/load; actual BC counters/format mappings. Compile and pass.
- [ ] Add only new header items to product project/filters; static zstd already linked. Independent review at completion of this approved change.

### Task2: Corpus and renderer acceptance

**Files:** tests/texture_cache_storage_benchmark.cpp; scripts/benchmark-texture-cache-storage.ps1; docs/rendering-technology-texture-cache-zstd-validation.md; survey and progress ledger.

- [ ] Snapshot65 existing DDS files to new isolated input. Use real Store/Load and compare metadata, all BC bytes, actual header-inclusive filesizes. Record compression/raw counts and CPU/I/O costs separately with explicit allocation/working-set limits. Do not overwrite original caches.
- [ ] Full Debug build via matching VsDevCmd, /Z7, /m:1 /nr:false, IntDir=build\extension-debug\, EnableParallelShaderBuild=false.
- [ ] Run affected DXC cache/thumbnail regressions; normal features84/fallback84 and fixed-clock capture comparisons. Reuse unaffected earlier evidence.
- [ ] Fresh independent review per executing-plans; fix Important/Critical findings with a reproducing RED→GREEN test and affected suite. No automatic Git mutation.
- [ ] Report measured storage/load/memory tradeoff and adoption decision, update survey/ledger/checks. Broad100-repository goal stays active.
