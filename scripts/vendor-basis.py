"""Regenerate the pinned basis_universal sources entirely from checked-in sources."""
import hashlib
import json
import shutil
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "External" / "basis_universal"
upstream = Path(__file__).resolve().parents[1] / "External" / "KTX-Software" / "upstream" / "external" / "basis_universal"
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

# Copy basis_universal transcoder headers and source
transcoder_src = upstream / "transcoder"
transcoder_dst = root / "transcoder"
if transcoder_dst.exists():
    shutil.rmtree(transcoder_dst)
shutil.copytree(transcoder_src, transcoder_dst)

# Copy LICENSE and NOTICE
shutil.copyfile(upstream / "LICENSE", root / "LICENSE")
shutil.copyfile(upstream / "NOTICE", root / "NOTICE")

# Generate manifest
files = sorted(path for path in root.rglob("*")
               if path.is_file() and (upstream in path.parents or path.name in ("LICENSE", "NOTICE")))
manifest = {"repository": "https://github.com/BinomialLLC/basis_universal",
            "version": "1.16", "commit": "073eb6a0a34a23d77e75e54f41dcd9653a52da83",
            "sha256": {path.relative_to(root).as_posix():
                       hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Generated basis_universal sources; hashed {len(files)} files")