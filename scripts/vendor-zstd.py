"""Regenerate the pinned zstd amalgamation entirely from checked-in sources."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1] / "External" / "zstd"
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
subprocess.run([sys.executable, "-X", "utf8", "combine.py", "-r", "../../lib", "-x",
                "legacy/zstd_legacy.h", "-k", "zstd.h", "-o",
                str(root / "zstd.c"), "zstd-in.c"],
               cwd=upstream / "build" / "single_file_libs", check=True,
               stdout=subprocess.DEVNULL)
# The upstream generator preserves the first include's original relative path.
# Relocate that include beside the standalone output, leaving upstream untouched.
generated = root / "zstd.c"
text = generated.read_text(encoding="utf-8")
text = text.replace('#include "../zstd.h"', '#include "zstd.h"')
generated.write_text(text, encoding="utf-8", newline="\n")
for name in ("zstd.h", "zstd_errors.h"):
    shutil.copyfile(upstream / "lib" / name, root / name)
shutil.copyfile(upstream / "LICENSE", root / "LICENSE")
files = sorted(path for path in root.rglob("*")
               if path.is_file() and (upstream in path.parents or path.name in
                                     ("zstd.c", "zstd.h", "zstd_errors.h", "LICENSE")))
manifest = {"repository": "https://github.com/meta/zstd",
            "version": "1.5.7", "commit": "f8745da6ff1ad1e7bab384bd1f9d742439278e99",
            "sha256": {path.relative_to(root).as_posix():
                       hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Generated {root / 'zstd.c'}; hashed {len(files)} files")
