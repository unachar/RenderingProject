#!/usr/bin/env python3
"""Compare two PFM images (Stage 2H reference-capture validation).

Reads the PFM variant written by ReferenceImageIO (PF color or Pf
grayscale, little-endian when the scale factor is negative, rows
stored bottom-first) and reports RMSE, relative RMSE (RMSE divided
by the reference mean), the maximum absolute error, and a global
SSIM over Reinhard-tonemapped luminance (the structural metric the
Stage 2H design asks for alongside RMSE). Exit code 0 when the
--max-rel-rmse / --min-ssim gates pass (default: report only,
always 0 unless dimensions mismatch).

Usage: pfm_compare.py reference.pfm candidate.pfm [--max-rel-rmse X]
                      [--min-ssim Y]
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
    return {"rmse": rmse, "rel_rmse": rel_rmse, "max_err": max_err, "mean_ref": mean_ref,
            "ssim": _tonemapped_ssim(ref, cand, rw, rh, rc)}


def _luminance(values, channels):
    if channels == 1:
        return list(values)
    out = []
    for i in range(0, len(values), 3):
        r, g, b = values[i], values[i + 1], values[i + 2]
        out.append(0.2126 * r + 0.7152 * g + 0.0722 * b)
    return out


def _tonemapped_ssim(ref, cand, width, height, channels):
    # Global SSIM on Reinhard-tonemapped luminance: HDR values are
    # unbounded, so structure is judged after x / (1 + x), matching
    # the preview writer's curve family.
    a = [max(v, 0.0) for v in _luminance(ref, channels)]
    b = [max(v, 0.0) for v in _luminance(cand, channels)]
    a = [v / (1.0 + v) for v in a]
    b = [v / (1.0 + v) for v in b]
    n = len(a)
    mean_a = sum(a) / n
    mean_b = sum(b) / n
    var_a = sum((v - mean_a) ** 2 for v in a) / n
    var_b = sum((v - mean_b) ** 2 for v in b) / n
    cov = sum((x - mean_a) * (y - mean_b) for x, y in zip(a, b)) / n
    c1 = 0.01 ** 2
    c2 = 0.03 ** 2
    numerator = (2 * mean_a * mean_b + c1) * (2 * cov + c2)
    denominator = (mean_a ** 2 + mean_b ** 2 + c1) * (var_a + var_b + c2)
    return numerator / denominator if denominator > 0 else 1.0


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    max_rel = None
    min_ssim = None
    for a in argv:
        if a.startswith("--max-rel-rmse="):
            max_rel = float(a.split("=", 1)[1])
        if a.startswith("--min-ssim="):
            min_ssim = float(a.split("=", 1)[1])
    if len(args) != 3:
        print(__doc__)
        return 2
    try:
        result = compare(args[1], args[2])
    except (ValueError, struct.error) as exc:
        print(f"pfm_compare: {exc}")
        return 2
    print(f"RMSE {result['rmse']:.6g}  rel-RMSE {result['rel_rmse']:.4%}  "
          f"max-err {result['max_err']:.6g}  SSIM {result['ssim']:.6f}  "
          f"(reference mean {result['mean_ref']:.6g})")
    if max_rel is not None and result["rel_rmse"] > max_rel:
        print(f"FAIL: rel-RMSE {result['rel_rmse']:.4%} exceeds {max_rel:.4%}")
        return 1
    if min_ssim is not None and result["ssim"] < min_ssim:
        print(f"FAIL: SSIM {result['ssim']:.6f} below {min_ssim:.6f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
