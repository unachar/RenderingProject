# Shader Cache zstd Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Preserve v1 DXC cache reads and add beneficial-only, bounded zstd v2 storage without changing shader bytes or cache keys.

**Architecture:** Keep compiler identity/key calculation in dxcshadercache.h. Isolate header validation and compression/decompression in shadercachecodec.h; existing Load/Store own filesystem and atomic replacement. Compile one pinned zstd C amalgamation into each executable, with no runtime DLL.

**Tech Stack:** Windows C++/C, MSVC, DXC, zstd1.5.7.

**Spec:** docs/superpowers/specs/2026-10-30-shader-cache-zstd-design.md (user approved2026-10-31).

## Global Constraints

- zstd commit f8745da6ff1ad1e7bab384bd1f9d742439278e99; BSD LICENSE/provenance/generation hashes included. No network/Python generation at application build time.
- v1 magic0x3143584443484143,32byte header, raw object+diagnostics remains readable. v2 magic0x3243584443484143,48byte header: magic,codec1,objectBytes,diagnosticBytes,storedBytes,checksum (uint64 little-endian).
- object limit64MiB, diagnostics1MiB; reject zero object, overflow, invalid sizes/codec, unknown/unmatched content size, truncated/concatenated/trailing frames and checksum mismatch. Failures are cache misses, with no stale outputs used.
- zstd level3; write v2 only when its total file size is smaller than v1, otherwise retain raw v1. Existing keys, disable-cache/debug/reflection flags and atomic file replacement remain intact. No bulk cache conversion/deletion.
- Execute inline in current workspace as already authorized. Git metadata is read-only; no automatic commit/push or new worktree. Match native test CRT (/MT) and project CRT; third-party C uses no PCH.

## Review Focus

- Declared sizes must be validated before body/decode allocations, including UINT64_MAX inputs.
- Known bounded output size does not permit concatenated/skippable/unknown-size frames.
- Corruption must leave caller outputs empty and allow recompilation; no failed payload reaches PSO creation.
- Raw fallback must include the extra v2 header cost in the benefit decision.
- Every standalone dxccompiler.h consumer must link the static codec, not rely on a locally installed zstd DLL.

### Task1: Static dependency and versioned payload codec

**Files:** External/zstd/{zstd.c,zstd.h,zstd_errors.h,LICENSE,SOURCE.md,manifest.json}, scripts/vendor-zstd.py; source/shadercachecodec.h; tests/shader_cache_codec_tests.cpp; scripts/test-shader-cache-codec.ps1; scripts/zstd-test-build.ps1.

**Interfaces:** ShaderCacheCodec::Header stores validated header/payload lengths, checksum and compression mode. ParseHeader(words,fileBytes,Header&) validates metadata without allocation. TryCompress(object,size,diagnostics,vector<char>&) produces level3 payload or false. DecodePayload(header,payload,object,diagnostics) validates frame/checksum and publishes only successful output. DxcShaderCache::Hash remains the public streaming FNV interface.

- [x] Write native tests against actual Load/Store: compressible8192byte object+diagnostics must produce v2 and exact roundtrip; small/noncompressible input must remain v1; manually written old-v1 must load.
- [x] Run against current implementation and confirm RED: compressible cache still has v1 magic.
- [x] Fetch pinned source through GitHub connector; use official combine.py/zstd-in.c, preserving public headers. Record original source hashes, generator command, generated SHA256 and BSD license. Compile as C, roundtrip without DLL.
- [x] Add adversarial tests for each size/codec/checksum field, truncation, bad frame, content-size mismatch/unknown, concatenated/trailing frames; malformed loads must return false with cleared outputs. Use actual frame data, not mocked decompression.
- [x] Implement codec and route Load/Store through it, preserving key code and atomic replacement. Run native codec tests GREEN.

### Task2: Product and standalone compiler integration

**Files:** DirectX12.vcxproj/.filters; source/dxcshadercache.h; scripts/test-dxccompilerpolicy.ps1, test-geometric-specular-aa.ps1, test-shader-cache.ps1, test-thumbnail-dispatch.ps1.

**Interfaces:** Existing DxcShaderCache::Load/Store signatures remain unchanged. Get-ZstdTestObject(developerCommand) builds/returns an x64 /MT C object for standalone tests; application compiles zstd.c with its own CRT/no PCH.

- [x] Add project C item, local warning/optimization metadata, BSD license packaging and header dependencies; no global language/CRT change.
- [x] Update all four standalone compiler consumers to link the helper object. Existing pure AA math/quality tests remain independent of zstd.
- [x] Run codec tests, real DXC policy/cache persistence/PSO tests, legacy shader-cache invalidation tests, thumbnail isolation tests and AA GPU fixture. Repair failures only in the responsible component.
- [x] Request independent review and resolve actionable findings with affected reruns.

### Task3: Native capacity/load measurements and renderer acceptance

**Files:** tests/shader_cache_codec_benchmark.cpp; scripts/benchmark-shader-cache-codec.ps1; docs/rendering-technology-zstd-validation.md; survey/ledger updates.

- [x] Snapshot existing v1 cache into an isolated benchmark input without overwriting it. Native runner decodes with real loader, encodes with real Store, exact-compares outputs, records original/new headers+payload bytes and separate CPU/I/O timings. Use matching baseline raw v1 logic for comparison; include file count/errors.
- [x] Record pre-change Debug EXE44950016bytes and post-change actual EXE size, noting Debug build context. Do not substitute Python-probe timings.
- [x] Full Debug build via matching VsDevCmd, /Z7, /m:1 /nr:false, EnableParallelShaderBuild=false, IntDir=build\extension-debug\.
- [x] Run normal features/fallback smoke; compare fixed-clock captures with after-aa reference. Record cache population/version counts without deleting old cache.
- [x] Accept default v2 writes only with meaningful measured capacity benefit and reasonable load cost. Document remaining performance limits, update approved plan/ledger; broad100-repository goal stays active.
