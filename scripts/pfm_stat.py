#!/usr/bin/env python3
"""Region statistics for a PFM image (Stage 2H verification).

Prints per-channel mean / min / max over the whole image or a
--region=x,y,w,h box (top-first pixel coordinates). The furnace
check uses it: the mirror sphere's region mean must approach the
wall emission (3.0) if the integrator's GGX sampling conserves
energy.

Usage: pfm_stat.py image.pfm [--region=x,y,w,h]
"""
import sys

from pfm_compare import read_pfm


def main(argv):
    region = None
    paths = []
    for arg in argv[1:]:
        if arg.startswith("--region="):
            parts = arg.split("=", 1)[1].split(",")
            if len(parts) != 4:
                print(__doc__)
                return 2
            region = tuple(int(p) for p in parts)
        else:
            paths.append(arg)
    if len(paths) != 1:
        print(__doc__)
        return 2
    try:
        width, height, channels, data = read_pfm(paths[0])
    except (ValueError, OSError) as exc:
        print(f"pfm_stat: {exc}")
        return 2
    if region is None:
        region = (0, 0, width, height)
    rx, ry, rw, rh = region
    if rx < 0 or ry < 0 or rw <= 0 or rh <= 0 or rx + rw > width or ry + rh > height:
        print(f"pfm_stat: region {region} outside {width}x{height}")
        return 2
    sums = [0.0] * channels
    mins = [float("inf")] * channels
    maxs = [float("-inf")] * channels
    count = rw * rh
    for y in range(ry, ry + rh):
        base = (y * width + rx) * channels
        for x in range(rw):
            for c in range(channels):
                v = data[base + x * channels + c]
                sums[c] += v
                mins[c] = min(mins[c], v)
                maxs[c] = max(maxs[c], v)
    means = [s / count for s in sums]
    fmt = lambda values: ", ".join(f"{v:.6g}" for v in values)
    print(f"{paths[0]} region {region}: mean ({fmt(means)})  "
          f"min ({fmt(mins)})  max ({fmt(maxs)})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
