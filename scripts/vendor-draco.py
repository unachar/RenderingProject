"""Regenerate the pinned Draco sources entirely from checked-in sources."""
import hashlib
import json
import shutil
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "External" / "Draco"
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

# Copy Draco source files
src_files = [
    "src/draco/compression/decode.cc",
    "src/draco/compression/decode.h",
    "src/draco/compression/draco_compression_options.cc",
    "src/draco/compression/draco_compression_options.h",
    "src/draco/compression/encode.cc",
    "src/draco/compression/encode.h",
    "src/draco/compression/encode_base.h",
    "src/draco/compression/expert_encode.cc",
    "src/draco/compression/expert_encode.h",
    "src/draco/compression/mesh/mesh_decoder.cc",
    "src/draco/compression/mesh/mesh_decoder.h",
    "src/draco/compression/mesh/mesh_edgebreaker_decoder.cc",
    "src/draco/compression/mesh/mesh_edgebreaker_decoder.h",
    "src/draco/compression/mesh/mesh_edgebreaker_encoder.cc",
    "src/draco/compression/mesh/mesh_edgebreaker_encoder.h",
    "src/draco/compression/mesh/mesh_encoder.cc",
    "src/draco/compression/mesh/mesh_encoder.h",
    "src/draco/compression/point_cloud/point_cloud_decoder.cc",
    "src/draco/compression/point_cloud/point_cloud_decoder.h",
    "src/draco/compression/point_cloud/point_cloud_encoder.cc",
    "src/draco/compression/point_cloud/point_cloud_encoder.h",
    "src/draco/core/bit_utils.cc",
    "src/draco/core/bit_utils.h",
    "src/draco/core/data_buffer.cc",
    "src/draco/core/data_buffer.h",
    "src/draco/core/decoder_buffer.cc",
    "src/draco/core/decoder_buffer.h",
    "src/draco/core/draco_index_type.h",
    "src/draco/core/draco_types.cc",
    "src/draco/core/draco_types.h",
    "src/draco/core/draco_version.h",
    "src/draco/core/encoder_buffer.cc",
    "src/draco/core/encoder_buffer.h",
    "src/draco/core/hash_utils.cc",
    "src/draco/core/hash_utils.h",
    "src/draco/core/options.cc",
    "src/draco/core/options.h",
    "src/draco/core/status.cc",
    "src/draco/core/status.h",
    "src/draco/core/status_or.h",
    "src/draco/io/mesh_io.cc",
    "src/draco/io/mesh_io.h",
    "src/draco/io/point_cloud_io.cc",
    "src/draco/io/point_cloud_io.h",
    "src/draco/io/gltf_decoder.cc",
    "src/draco/io/gltf_decoder.h",
    "src/draco/io/gltf_encoder.cc",
    "src/draco/io/gltf_encoder.h",
    "src/draco/mesh/mesh.cc",
    "src/draco/mesh/mesh.h",
    "src/draco/mesh/corner_table.cc",
    "src/draco/mesh/corner_table.h",
    "src/draco/point_cloud/point_cloud.cc",
    "src/draco/point_cloud/point_cloud.h",
]

for src_file in src_files:
    src = upstream / src_file
    dst = root / src_file
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)

# Copy LICENSE
shutil.copyfile(upstream / "LICENSE", root / "LICENSE")

# Generate manifest
files = sorted(path for path in root.rglob("*")
               if path.is_file() and (upstream in path.parents or path.name in ("LICENSE",)))
manifest = {"repository": "https://github.com/google/draco",
            "version": "1.5.6", "commit": "main",
            "sha256": {path.relative_to(root).as_posix():
                       hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Generated Draco sources; hashed {len(files)} files")