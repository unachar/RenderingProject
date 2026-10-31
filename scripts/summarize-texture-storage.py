"""Summarize a successful native DDS/BC zstd experiment without changing inputs."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("report", type=Path)
parser.add_argument("cache", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
report = json.loads(args.report.read_text(encoding="utf-8"))
files = report["files"]
if len(files) != report["fileCount"]:
    raise ValueError("Native report file-count mismatch")
manifest = []
for item in files:
    path = args.cache / item["name"]
    if path.stat().st_size != item["ddsBytes"]:
        raise ValueError(f"Current source length differs from native input: {path}")
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    manifest.append(dict(name=item["name"], sha256=digest, bytes=item["ddsBytes"]))

def totals(group, level_index, kind):
    return {key: sum(item["levels"][level_index][kind][key] for item in group)
            for key in ("storedBytes", "compressMs", "decompressMsMean")}

levels = []
for index, level in enumerate((1, 3, 9)):
    whole = totals(files, index, "wholeDds")
    mips = totals(files, index, "perMipBcFrames")
    levels.append(dict(level=level, wholeDds=whole, perMipBcFrames=mips,
                       wholeDdsSavedFraction=1 - whole["storedBytes"] / report["ddsBytes"],
                       bcPayloadSavedFraction=1 - mips["storedBytes"] / report["bcPayloadBytes"],
                       decodeAndDdsMemoryLoadMsMean=sum(item["levels"][index]["decodeAndDdsMemoryLoadMsMean"] for item in files),
                       expandedWholeDdsFiles=sum(item["levels"][index]["wholeDds"]["storedBytes"] >= item["ddsBytes"] for item in files),
                       expandedPerMipBcFiles=sum(item["levels"][index]["perMipBcFrames"]["storedBytes"] >= item["bcPayloadBytes"] for item in files)))
formats = []
for name in sorted({item["format"] for item in files}):
    group = [item for item in files if item["format"] == name]
    formats.append(dict(format=name, files=len(group), bcPayloadBytes=sum(item["bcPayloadBytes"] for item in group),
                        zstd3PerMipBytes=totals(group, 1, "perMipBcFrames")["storedBytes"]))
unique_names = []
seen_hashes = set()
for item in manifest:
    if item["sha256"] not in seen_hashes:
        seen_hashes.add(item["sha256"])
        unique_names.append(item["name"])
unique_files = [item for item in files if item["name"] in unique_names]
unique_raw_bytes = sum(item["ddsBytes"] for item in unique_files)
unique_stored_bytes = totals(unique_files, 1, "wholeDds")["storedBytes"]
result = dict(nativeReport=str(args.report), scope=report["scope"], files=len(files),
              mipLevels=sum(item["mips"] for item in files), ddsBytes=report["ddsBytes"],
              bcPayloadBytes=report["bcPayloadBytes"], formats=formats, levels=levels,
              ddsMemoryLoadMsMean=sum(item["ddsMemoryLoadMsMean"] for item in files),
              currentSourceUniqueSha256=len({item["sha256"] for item in manifest}),
              uniqueDdsLevel3=dict(files=len(unique_files), ddsBytes=unique_raw_bytes,
                                   zstdBytes=unique_stored_bytes, savedFraction=1 - unique_stored_bytes / unique_raw_bytes),
              currentSourceManifest=manifest,
              manifestLimit="Hashes collected after native run; filenames and lengths matched. Does not attribute source assets/usage or prove deduplication by GPU content.")
args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps({key: value for key, value in result.items() if key not in ("currentSourceManifest",)}, ensure_ascii=False))
