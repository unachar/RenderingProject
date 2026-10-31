"""Compare fixed-clock renderer captures by exact RGB pixel values."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("baseline", type=Path)
parser.add_argument("current", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--names", nargs="+", default=["morning.png", "noon.png", "evening.png", "night.png", "dlss.png"])
args = parser.parse_args()
captures = []
for name in args.names:
    with Image.open(args.baseline / name) as image:
        baseline = np.asarray(image.convert("RGB"), dtype=np.int16)
    with Image.open(args.current / name) as image:
        current = np.asarray(image.convert("RGB"), dtype=np.int16)
    if baseline.shape != current.shape:
        raise ValueError(f"Capture dimensions differ: {name}")
    difference = np.abs(current - baseline)
    captures.append(dict(name=name, identical=bool(np.array_equal(baseline, current)),
                         mae=float(difference.mean()), maximum=int(difference.max())))
result = dict(all_identical=all(item["identical"] for item in captures), captures=captures)
args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result))
raise SystemExit(0 if result["all_identical"] else 1)
