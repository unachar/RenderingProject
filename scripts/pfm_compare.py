#!/usr/bin/env python3
"""Compare two PFM images (Stage 2H reference-capture validation).

Reads the PFM variant written by ReferenceImageIO (PF color or Pf
grayscale, little-endian when the scale factor is negative, rows
stored bottom-first) and reports RMSE, relative RMSE (RMSE divided
by the reference mean), and the maximum absolute error. Exit code
0 when relative RMSE is within --max-rel-rmse (default: report
only, always 0 unless dimensions mismatch).

Usage: pfm_compare.py reference.pfm candidate.pfm [--max-rel-rmse X]
"""
import math
import struct
import sys


def read_pfm(path):
    with open(path, "rb") as handle:
        magic = handle.readline().strip()
        if magic not in (b"PF", b"Pf"):
            raise ValueError(f"{path}: not a PFM file")
        channels = 3 if magic == b"PF" else 1
        dims = handle.readline().split()
        width, height = int(dims[0]), int(dims[1])
        scale = float(handle.readline().strip())
        endian = "<" if scale < 0 else ">"
        count = width * height * channels
        data = struct.unpack(f"{endian}{count}f", handle.read(4 * count))
    # Rows are stored bottom-first; flip to top-first for comparison.
    rows = [data[y * width * channels:(y + 1) * width * channels] for y in range(height)]
    rows.reverse()
    return width, height, channels, [v for row in rows for v in row]


def compare(reference_path, candidate_path):
    rw, rh, rc, ref = read_pfm(reference_path)
    cw, ch, cc, cand = read_pfm(candidate_path)
    if (rw, rh, rc) != (cw, ch, cc):
        raise ValueError(f"dimension mismatch: {rw}x{rh}x{rc} vs {cw}x{ch}x{cc}")
    sum_sq = 0.0
    sum_ref = 0.0
    max_err = 0.0
    for a, b in zip(ref, cand):
        err = a - b
        sum_sq += err * err
        sum_ref += a
        max_err = max(max_err, abs(err))
    n = len(ref)
    rmse = math.sqrt(sum_sq / n)
    mean_ref = sum_ref / n
    rel_rmse = rmse / mean_ref if abs(mean_ref) > 1e-12 else float("inf")
    return {"rmse": rmse, "rel_rmse": rel_rmse, "max_err": max_err, "mean_ref": mean_ref}


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    max_rel = None
    for a in argv:
        if a.startswith("--max-rel-rmse="):
            max_rel = float(a.split("=", 1)[1])
    if len(args) != 3:
        print(__doc__)
        return 2
    try:
        result = compare(args[1], args[2])
    except (ValueError, struct.error) as exc:
        print(f"pfm_compare: {exc}")
        return 2
    print(f"RMSE {result['rmse']:.6g}  rel-RMSE {result['rel_rmse']:.4%}  "
          f"max-err {result['max_err']:.6g}  (reference mean {result['mean_ref']:.6g})")
    if max_rel is not None and result["rel_rmse"] > max_rel:
        print(f"FAIL: rel-RMSE {result['rel_rmse']:.4%} exceeds {max_rel:.4%}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
