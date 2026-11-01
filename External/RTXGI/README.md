# RTXGI SHaRC

Vendored shader headers from NVIDIA Spatial Hash Radiance Cache, revision
`4e21b585c33c83d723ca9a1e11bbb1090d145793` (SHaRC 1.8.3).
Source: https://github.com/NVIDIAGameWorks/Spatial-Hash-Radiance-Cache
RTXGI: https://github.com/NVIDIA-RTX/RTXGI
License: [SHaRC/License.md](SHaRC/License.md).

`shader/hlsl/RtxgiSharcIrradiance.hlsli` adapts the renderer's existing diffuse
irradiance cache to the SDK. It stores material-independent `E / pi`, restores
irradiance on secondary hits, and keeps the existing direct-light evaluation,
emissive sampling, ReSTIR GI and NRD. This is an irradiance-cache backend, not
the RTXGI sample's complete path tracer, NRC, or DDGI.

Updates use a deterministic spatial/temporal hash to select approximately 1/16
of existing diffuse samples, following the SDK's sparse-update recommendation.
Entries expire after 64 frames without updates so sparsely sampled cells can
survive the typical 16-frame interval between updates.
No primary GI rays or cache queries are omitted. Updates are fused into the
existing diffuse trace. A UAV barrier precedes the
SDK resolve pass, followed by another UAV barrier. Queries use the previous
resolved frame, matching the existing backend's one-frame propagation delay.
Primary GI rays and their actual hit distances remain available to NRD.
The optional primary-ray cache-skipping budget is disabled for SHaRC because
the SDK cache does not store hit distances.

Requirements: DXR 1.1, Shader Model 6.5, DXC and native 16-bit shader types.
The implementation uses SDK lock buffers rather than 64-bit atomic operations.
Unsupported devices or failed shader compilation use the existing DXR backend
and report the fallback in the GI status text. Cache allocation failure stops
that frame's GI instead of binding incompatible descriptors.

The UI mode has persistent numeric value 4; existing values 0 through 3 retain
their meanings. Switching cache backends waits for GPU completion and clears
cache, reservoir and GI denoiser history. The selected backend alone owns GPU
cache buffers: 262,144 entries, 18 MiB for the original backend or 11 MiB for
SHaRC including its lock buffer. Neither NRC nor CUDA/Tensor Core dependencies
are introduced.
