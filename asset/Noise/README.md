# Runtime STBN

`stbn-vec2-128x128x64.rg8` contains NVIDIA STBN's **uniform Float2**
`stbn_vec2_2Dx1D_128x128x64_0.png` through `_63.png`, in slice/y/x/RG order.
It is numerical linear data, not an sRGB color texture. Do not apply texture
compression, mip generation, interpolation, or a unit-vector decode.

Source: the project's vendored `stbn.tar.gz`, `STBN-main/Assets/STBN.zip`.
The ZIP SHA256 is `f262aaa79704b913ad1ac22b11674931c5c16a788688f8ce49ec43d59eb5c747`,
matching `External/STBN/manifest.json`. Upstream:
https://github.com/NVIDIA-RTX/STBN . See `STBN-License.txt` for NVIDIA's terms.

Regenerate from the workspace root with `powershell -File scripts/pack-stbn.ps1`.
Only PNG R/G channels are copied; alpha is not used as a PDF.
Size: 2,097,152 bytes. SHA256:
`b3d8b7e55f1cb89683d1a4f51c494fe8071a81a36875eecbdc00f27add5ec0b1`.
The shader decodes each byte as `(value + 0.5) / 256`, matching the upstream
exporter's 256-bin quantization while avoiding 0/1 warp endpoints.

The renderer uploads this once to a default-heap raw buffer (plus a four-byte
availability flag) and binds it at `t0, space16` in both ray root signatures.
Keep this file and the notice with the runtime `asset` directory.
Set process environment variable `DX12_STBN=0` before launch to compare with
the existing sampling path. Missing/invalid-size data also uses that
fallback and is reported in `init_log.txt`.
