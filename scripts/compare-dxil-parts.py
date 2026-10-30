"""Compare DXIL container payloads, optionally allowing only reflection removal."""
import argparse
import json
import struct
from pathlib import Path


def read_parts(path):
    data = Path(path).read_bytes()
    if len(data) < 32 or data[:4] != b"DXBC":
        raise ValueError(f"Invalid container: {path}")
    count = struct.unpack_from("<I", data, 28)[0]
    offsets = struct.unpack_from(f"<{count}I", data, 32)
    parts = {}
    for offset in offsets:
        name, size = struct.unpack_from("<4sI", data, offset)
        if offset + 8 + size > len(data) or name in parts:
            raise ValueError(f"Invalid or duplicate container part: {path}")
        parts[name] = data[offset + 8:offset + 8 + size]
    return data, parts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--strip-reflection", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    baseline, before = read_parts(args.baseline)
    candidate, after = read_parts(args.candidate)
    if args.strip_reflection:
        if b"STAT" not in before or b"STAT" in after:
            raise ValueError("Expected reflection removal did not occur")
        del before[b"STAT"]
    if before != after:
        changed = sorted(name.decode("ascii") for name in before.keys() | after.keys()
                         if before.get(name) != after.get(name))
        raise ValueError(f"Container payloads changed: {changed}")
    result = {"passed": True, "baseline_bytes": len(baseline), "candidate_bytes": len(candidate),
              "reflection_removal": args.strip_reflection,
              "identical_parts": sorted(name.decode("ascii") for name in before)}
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
