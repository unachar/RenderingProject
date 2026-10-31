"""Regenerate the pinned KTX-Software sources entirely from checked-in sources."""
import hashlib
import json
import shutil
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "External" / "KTX-Software"
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

# Copy KTX headers
include_src = upstream / "lib" / "include"
include_dst = root / "include"
if include_dst.exists():
    shutil.rmtree(include_dst)
shutil.copytree(include_src, include_dst)

# Copy KTX source files
src_src = upstream / "lib" / "src"
src_dst = root / "src"
if src_dst.exists():
    shutil.rmtree(src_dst)
shutil.copytree(src_src, src_dst)

# Copy LICENSE
shutil.copyfile(upstream / "LICENSE.md", root / "LICENSE.md")

# Generate manifest
files = sorted(path for path in root.rglob("*")
               if path.is_file() and (upstream in path.parents or path.name in ("LICENSE.md",)))
manifest = {"repository": "https://github.com/KhronosGroup/KTX-Software",
            "version": "4.3.0", "commit": "00d67536657e0e1563d5ed94772c7bd1556b917b",
            "sha256": {path.relative_to(root).as_posix():
                       hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Generated KTX-Software sources; hashed {len(files)} files")