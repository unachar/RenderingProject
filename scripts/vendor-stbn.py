"""Regenerate the pinned STBN sources entirely from checked-in sources."""
import hashlib
import json
import shutil
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "External" / "STBN"
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

# Copy STBN library headers and sources
lib_src = upstream / "Libraries"
lib_dst = root / "Libraries"
if lib_dst.exists():
    shutil.rmtree(lib_dst)
shutil.copytree(lib_src, lib_dst)

# Copy License and README
shutil.copyfile(upstream / "License.txt", root / "License.txt")
shutil.copyfile(upstream / "README.md", root / "README.md")

# Generate manifest
files = sorted(path for path in root.rglob("*")
               if path.is_file() and (upstream in path.parents or path.name in ("License.txt", "README.md")))
manifest = {"repository": "https://github.com/NVIDIA-RTX/STBN",
            "version": "1.0.0", "commit": "main",
            "sha256": {path.relative_to(root).as_posix():
                       hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Generated STBN sources; hashed {len(files)} files")