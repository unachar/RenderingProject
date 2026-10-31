"""Regenerate the pinned D3D12MemoryAllocator sources entirely from checked-in sources."""
import hashlib
import json
import shutil
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "External" / "D3D12MemoryAllocator"
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

# Copy D3D12MemoryAllocator header
include_src = upstream / "include" / "D3D12MemAlloc.h"
include_dst = root / "D3D12MemAlloc.h"
shutil.copyfile(include_src, include_dst)

# Copy D3D12MemoryAllocator source files
src_files = [
    "src/Common.cpp",
    "src/Common.h",
    "src/D3D12MemAlloc.cpp",
    "src/D3D12MemAlloc.natvis",
    "src/cmake/config.cmake.in",
    "src/Shaders/FxBatchCompilerScript.fxbc",
    "src/Shaders/PS.hlsl",
    "src/Shaders/PS_Compiled.h",
    "src/Shaders/VS.hlsl",
    "src/Shaders/VS_Compiled.h",
]
for src_file in src_files:
    src = upstream / src_file
    dst = root / src_file
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)

# Copy CMakeLists.txt
shutil.copyfile(upstream / "CMakeLists.txt", root / "CMakeLists.txt")
shutil.copyfile(upstream / "src" / "CMakeLists.txt", root / "src" / "CMakeLists.txt")

# Copy LICENSE and NOTICES
shutil.copyfile(upstream / "LICENSE.txt", root / "LICENSE.txt")
shutil.copyfile(upstream / "NOTICES.txt", root / "NOTICES.txt")

# Generate manifest
files = sorted(path for path in root.rglob("*")
               if path.is_file() and (upstream in path.parents or path.name in ("LICENSE.txt", "NOTICES.txt", "CMakeLists.txt", "D3D12MemAlloc.h")))
manifest = {"repository": "https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator",
            "version": "2.1.0", "commit": "master",
            "sha256": {path.relative_to(root).as_posix():
                       hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Generated D3D12MemoryAllocator sources; hashed {len(files)} files")