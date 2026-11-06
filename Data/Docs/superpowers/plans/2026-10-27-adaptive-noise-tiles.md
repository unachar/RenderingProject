# Adaptive procedural color tiles

Ruling: replace exact sample memoization with adaptive UV tiles. Measured moving raster scene regressed from GPU 4.9055ms uncached to 5.1096ms exact memoization. UV bit equality cannot reuse TAA/moving pixels, so that implementation does not meet the user's approved regional-cache intent.

## Frozen interface shared by implementers

- Existing production binding API `ProcStudio::NoiseSampleCache` and root t0/u0 space15 remain. Root table slots and BeginFrame/FinishFrame ordering are unchanged.
- `ByteAddressBuffer ProceduralNoiseSamples : t0 space15` is immutable through rendering. `RWByteAddressBuffer ProceduralNoiseRequests : u0 space15` collects tile requests. Merge CS reads requests and writes `RWByteAddressBuffer Cache : u1 space15`.
- 8192 buckets, 4 ways = 32768 physical pages. Each page has a 32-byte header and 16x16 vertex samples, each 8 bytes (RGB FP16 packed in two uints; unused half zero). Page interior covers 15x15 bilinear cells; border vertex avoids cross-page interpolation seams.
- Global cache header 32 bytes; uint at 0 is enabled (0 in fallback, 1 after initialize). Page headers begin at 32: five uint key words at +0/+4/+8/+12/+16, frame age at +20, spare +24/+28. Page texels begin at `32 + 32768*32`; `((page*256 + y*16+x)*8)` offsets from texel base. Total CacheBytes = 32 + 32768*32 + 32768*256*8 = 68157472. Requests = 8192*256 = 2097152 bytes. Dispatch **8192 groups**, numthreads(64,1,1), for clear and merge.
- Request record size 256: five key words at 0..16; definition `Parameters` (208 bytes) at +32..239; atomic ownership word +252. One producer per bucket wins CAS 0->1, stores key and definition. Frame-end UAV barrier guarantees payload completion. No rendering reads requests, no locks/spins. Requests reset at merge/clear.
- Key = {definition Padding1, Padding2, packed grid levels, asuint(tile.x), asuint(tile.y)}. Grid steps per UV axis = exp2(floor(log2(abs footprint axis))). Levels packed as `(level.x+128) | ((level.y+128)<<8)`. Reject nonfinite/zero footprint, levels outside [-120,120], integer coordinate overflow; fallback analytical. Pixel cell=floor(uv/step), tile=floor(cell/15), local=cell-tile*15 in0..14. Handle negative UVs with floor, not truncated integer division.
- Hash all 5 key words and mask 8191. Four-way lookup validates all five words. Selected page has all 256 vertices populated; ungenerated/missing page uses exact analytical Color(Scalar(uv)) and enqueues tile. Warm hits bilinearly interpolate four half-float vertex colors. No fixed UV-resolution cap: smaller footprint requests finer grid. FP16 introduces bounded color precision error; it does not cap spatial detail. Degenerate/extreme UVs fallback analytical.
- HLSL public API `float3 CachedColor(Parameters p,float2 uv,float2 footprint)` and `bool FindCachedColor(Parameters p,float2 uv,float2 footprint,out float3 color)`. Two-argument CachedColor should remain analytic fallback for callers without trustworthy footprint; root integrator will provide footprints where available. Helpers may be named freely.
- Merge per bucket: if request occupied, choose matching/empty/oldest of bucket's four pages; load shared definition/key once, group threads evaluate 256 lattice vertices through production Scalar+Color (single loop call site), pack f32tof16 to 2 uints. No full texture initialization needed; clear only page keys/ages and request states. No render/merge overlap. Store page key+age and complete all vertex writes before frame-end barrier makes cache readable. No cross-group page writes (each group owns its four pages).

## Work ownership

- Shader implementer owns shader/hlsl/NoiseCacheLayout.hlsli, NoiseCacheCS.hlsl and cache portion of ProceduralTexture.hlsli. Keep Scalar/Color/Normal analytical functions unchanged. Compile DXC cache tests/merge only after test author updates signatures; standalone merge can compile independently.
- Resource implementer owns source/procedural/proceduralnoisecache.h/.cpp only. Update capacities, dispatch group count, names/comments; preserve real small fallback resources, lifecycle and barriers. No full builds.
- Test implementer owns tests/noise_cache_shader.hlsl and noise_cache_gpu_tests.h only. Update to tile API; exercise all22bases at grid-node UVs on a 256x256 grid with step2^-20 (negative UV/sub16K texel spacing); interleaved definitions fit ~5632 tiles. At nodes original RGB tolerance0.0006 for FP16+compiler differences, warm cache stored values exact on repeat, warm hit rate >99% after ~8 frames. Test off-node independent interpolation, edited definitions, new finer levels, negative seams, fallback, eviction/regions. Expose hit flag via FindCachedColor. Cold path is analytical so cold-vs-warm need FP16 tolerance, not bit equality. Keep no-cache baseline/moving/cold/warm timing distinguished. Do not run GPU tests concurrently with app; root runs final tests.
- Root owns root/footprint call-site integration, docs, builds, real-scene measurements and review. Do not edit other agents' files.

## Validation

Shader compile, GPU cache tests including real fallback buffers, CPU definition tests, Release build. Run isolated copied scene with .noise on base color in moving/TAA conditions, cache on/off; measure actual results and never infer FPS from unit benchmarks. Preserve user's assets/settings. Existing White image parity fails identically before this change (0.737312); keep separate.


## Implementation rulings and evidence

- Implemented shader layout/generation, 67MiB resource pool, primary raster/visibility footprint integration, secondary footprint estimation and real-buffer fallback. All22basis GPU tests passed with legal maximum definition frequency64. Negative borders, interpolation, finer grid, edits, eviction and fallback passed; D3D12 debug queue clean.
- The aligned fixture uses57600unique lattice nodes and65536requests across5632definition tiles. A wider7128tile test produced98.8-98.9% hits due4-way collisions; it was not a color mismatch. The capacity test still verifies actual eviction.
- Analytical code paths have2e-5 tolerance for compiler FP differences; resident FP16 vertex comparison uses0.0006, repeated resident values exact. The discarded65536frequency fixture exceeded .noise's validated limit64 and produced large-angle harmonic rounding differences.
- Ruling: secondary rays/cards must only read existing tiles, never enqueue pages. Initial252pDXR comparison showed1.65ms extra frame-end work caused by sparse secondary hits requesting whole pages. Primary raster retains requests; secondary misses remain analytical.
- Empty merge buckets return before shared-memory synchronization. The full22basis GPU suite passed again after this change.
- User clarified internal rendering is about250p; acceptance measurements now use448x252 with output1280x720, moving camera andTAA. 720p-native was diagnostic, not the requested target. Benchmark flags preserve existing defaults and do not change saved user settings.
- Release build and FXC5.1/DXC6 shader compilations passed. FullDXR/NRD application reached complete120frame measurement at448x252; final admission-policy A/B measurement pending.

- Final acceptance update from user: internal resolution is about250p and >60fps is sufficient; do not continue chasing150fps. Final verification uses448x252, DXR/NRD on, moving camera/TAA,120warmup+240measuredframes.
- Final Voronoi case completed: cache on7.2934ms/frame(~137.1fps), cache off7.9306ms/frame(~126.1fps). Tile update averaged0.0635ms. Heavy Simplex/DomainWarp10octave verification still running at this record.

- Complete: final primary-only admission policy reached137.1fps Voronoi (1%low68.4),119.6fps Simplex/DomainWarp10octaves (1%low63.6) at448x252 withDXR/NRD+moving camera/TAA. User's revised >60fps criterion verified in both representative actual files. Heavy no-cache comparison was cancelled as unnecessary after the user's relaxed criterion. Evidence preserved inbuild/noise-cache-validation; user assets remain unchanged.
- Final read-only review found no blocking issues. Final22basis GPU suite andD3D12diagnostics passed; Release build andlegacyFXC/DXCpaths passed. Existing pre-cache Whiteimage parity issue remains separately documented.
