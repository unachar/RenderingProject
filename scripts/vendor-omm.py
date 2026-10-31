"""Regenerate the pinned OMM sources entirely from checked-in sources."""
import hashlib
import json
import shutil
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "External" / "OMM"
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

# Copy omm-gpu-nvrhi
omm_gpu_src = upstream / "libraries" / "omm-gpu-nvrhi"
omm_gpu_dst = root / "omm-gpu-nvrhi"
if omm_gpu_dst.exists():
    shutil.rmtree(omm_gpu_dst)
shutil.copytree(omm_gpu_src, omm_gpu_dst)

# Copy omm-lib headers
include_src = upstream / "libraries" / "omm-lib" / "include"
include_dst = root / "omm-lib" / "include"
if include_dst.exists():
    shutil.rmtree(include_dst)
shutil.copytree(include_src, include_dst)

# Copy omm-lib shaders
shaders_src = upstream / "libraries" / "omm-lib" / "shaders"
shaders_dst = root / "omm-lib" / "shaders"
if shaders_dst.exists():
    shutil.rmtree(shaders_dst)
shutil.copytree(shaders_src, shaders_dst)

# Copy omm-lib sources
src_src = upstream / "libraries" / "omm-lib" / "src"
src_dst = root / "omm-lib" / "src"
if src_dst.exists():
    shutil.rmtree(src_dst)
shutil.copytree(src_src, src_dst)

# Copy CMakeLists.txt and ShaderCompilation.cmake
shutil.copyfile(upstream / "libraries" / "omm-lib" / "CMakeLists.txt", root / "omm-lib" / "CMakeLists.txt")
shutil.copyfile(upstream / "libraries" / "omm-lib" / "ShaderCompilation.cmake", root / "omm-lib" / "ShaderCompilation.cmake")
shutil.copyfile(upstream / "libraries" / "omm-lib" / "shaders.cfg", root / "omm-lib" / "shaders.cfg")
shutil.copyfile(upstream / "libraries" / "omm-gpu-nvrhi" / "CMakeLists.txt", root / "omm-gpu-nvrhi" / "CMakeLists.txt")

# Copy LICENSE and README
shutil.copyfile(upstream / "LICENSE.txt", root / "LICENSE.txt")
shutil.copyfile(upstream / "README.md", root / "README.md")

# Generate manifest
files = sorted(path for path in root.rglob("*")
               if path.is_file() and (upstream in path.parents or path.name in ("LICENSE.txt", "README.md")))
manifest = {"repository": "https://github.com/NVIDIA-RTX/OMM",
            "version": "1.0.0", "commit": "main",
            "sha256": {path.relative_to(root).as_posix():
                       hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Generated OMM sources; hashed {len(files)} files")