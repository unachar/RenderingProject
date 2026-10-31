"""Regenerate the pinned DirectStorage sources entirely from checked-in sources."""
import hashlib
import json
import shutil
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "External" / "DirectStorage"
upstream = root / "upstream"
expected = json.loads((root / "upstream-hashes.json").read_text(encoding="utf-8"))
for name, expected_hash in expected.items():
    data = (upstream / name).read_bytes()
    actual = hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()
    if actual != expected_hash:
        raise RuntimeError(f"Pinned upstream Git blob mismatch: {name}")
if "--check" in sys.argv:
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    for name, expected_hash in manifest["sha256"].items():
        if hashlib.sha256((root / name).read_bytes()).hexdigest() != expected_hash:
            raise RuntimeError(f"Vendored file SHA256 mismatch: {name}")
    print(f"PASS {len(expected)} pinned Git blobs and {len(manifest['sha256'])} SHA256 files")
    sys.exit(0)

# Copy GDeflate headers and sources
gdeflate_src = upstream / "GDeflate" / "GDeflate"
gdeflate_dst = root / "GDeflate"
if gdeflate_dst.exists():
    shutil.rmtree(gdeflate_dst)
shutil.copytree(gdeflate_src, gdeflate_dst)

# Copy GDeflate shaders
shaders_src = upstream / "GDeflate" / "shaders"
shaders_dst = root / "shaders"
if shaders_dst.exists():
    shutil.rmtree(shaders_dst)
shutil.copytree(shaders_src, shaders_dst)

# Copy zstd GPU decompression headers and sources
zstdgpu_src = upstream / "zstd" / "zstdgpu"
zstdgpu_dst = root / "zstdgpu"
if zstdgpu_dst.exists():
    shutil.rmtree(zstdgpu_dst)
shutil.copytree(zstdgpu_src, zstdgpu_dst)

# Copy zstd shaders
zstd_shaders_src = upstream / "zstd" / "zstdgpu" / "Shaders"
zstd_shaders_dst = root / "zstdgpu" / "Shaders"
if zstd_shaders_dst.exists():
    shutil.rmtree(zstd_shaders_dst)
shutil.copytree(zstd_shaders_src, zstd_shaders_dst)

# Copy zstd srt headers
srt_headers_src = upstream / "zstd" / "zstdgpu" / "srt_headers"
srt_headers_dst = root / "zstdgpu" / "srt_headers"
if srt_headers_dst.exists():
    shutil.rmtree(srt_headers_dst)
shutil.copytree(srt_headers_src, srt_headers_dst)

# Copy ThirdParty headers
thirdparty_src = upstream / "zstd" / "ThirdParty"
thirdparty_dst = root / "ThirdParty"
if thirdparty_dst.exists():
    shutil.rmtree(thirdparty_dst)
shutil.copytree(thirdparty_src, thirdparty_dst)

# Copy tools/srtgen headers
srtgen_src = upstream / "zstd" / "tools" / "srtgen"
srtgen_dst = root / "srtgen"
if srtgen_dst.exists():
    shutil.rmtree(srtgen_dst)
shutil.copytree(srtgen_src, srtgen_dst)

# Copy LICENSE and NOTICES
shutil.copyfile(upstream / "LICENSE", root / "LICENSE")
shutil.copyfile(upstream / "NOTICES.txt", root / "NOTICES.txt")

# Generate manifest
files = sorted(path for path in root.rglob("*")
               if path.is_file() and (upstream in path.parents or path.name in ("LICENSE", "NOTICES.txt")))
manifest = {"repository": "https://github.com/microsoft/DirectStorage",
            "version": "1.0.0", "commit": "main",
            "sha256": {path.relative_to(root).as_posix():
                       hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Generated DirectStorage GPU decompression sources; hashed {len(files)} files")