"""Compare captured output pixels; this does not assess temporal artifacts or HDR."""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", type=Path)
    parser.add_argument("second", type=Path)
    parser.add_argument("--max-error", type=int, default=0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 0 <= args.max_error <= 255:
        parser.error("--max-error must be between 0 and 255")

    with Image.open(args.first) as image:
        first = np.asarray(image.convert("RGBA"), dtype=np.int16)
    with Image.open(args.second) as image:
        second = np.asarray(image.convert("RGBA"), dtype=np.int16)
    if first.shape != second.shape:
        raise ValueError(f"Capture sizes differ: {first.shape} vs {second.shape}")
    delta = np.abs(first - second)
    changed = np.any(delta != 0, axis=2)
    maximum = int(delta.max(initial=0))
    result = {
        "first": str(args.first.resolve()),
        "second": str(args.second.resolve()),
        "width": int(first.shape[1]),
        "height": int(first.shape[0]),
        "changedPixels": int(np.count_nonzero(changed)),
        "maxAbsoluteChannelError": maximum,
        "meanAbsoluteChannelError": float(delta.mean()),
        "tolerance": args.max_error,
        "passed": maximum <= args.max_error,
        "scope": "single captured output frame in 8-bit RGBA",
    }
    text = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
