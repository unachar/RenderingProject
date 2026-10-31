"""Regenerate the pinned Streamline sources entirely from checked-in sources."""
import hashlib
import json
import shutil
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "External" / "Streamline"
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

# Copy headers
include_src = upstream / "include"
include_dst = root / "include"
if include_dst.exists():
    shutil.rmtree(include_dst)
shutil.copytree(include_src, include_dst)

# Copy core sources
core_src = upstream / "source" / "core"
core_dst = root / "source" / "core"
if core_dst.exists():
    shutil.rmtree(core_dst)
shutil.copytree(core_src, core_dst)

# Copy platform sources
platforms_src = upstream / "source" / "platforms"
platforms_dst = root / "source" / "platforms"
if platforms_dst.exists():
    shutil.rmtree(platforms_dst)
shutil.copytree(platforms_src, platforms_dst)

# Copy plugin sources
plugins_src = upstream / "source" / "plugins"
plugins_dst = root / "source" / "plugins"
if plugins_dst.exists():
    shutil.rmtree(plugins_dst)
shutil.copytree(plugins_src, plugins_dst)

# Copy shared
shared_src = upstream / "source" / "shared"
shared_dst = root / "source" / "shared"
if shared_dst.exists():
    shutil.rmtree(shared_dst)
shutil.copytree(shared_src, shared_dst)

# Copy LICENSE
shutil.copyfile(upstream / "license.txt", root / "license.txt")

# Generate manifest
files = sorted(path for path in root.rglob("*")
               if path.is_file() and (upstream in path.parents or path.name in ("license.txt",)))
manifest = {"repository": "https://github.com/NVIDIA-RTX/Streamline",
            "version": "2.0.0", "commit": "main",
            "sha256": {path.relative_to(root).as_posix():
                       hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Generated Streamline sources; hashed {len(files)} files")