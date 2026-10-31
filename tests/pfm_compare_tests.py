#!/usr/bin/env python3
"""Self-tests for scripts/pfm_compare.py (Stage 2H tooling)."""
import math
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
import pfm_compare  # noqa: E402


def write_pfm(path, width, height, pixels_top_first):
    # PFM stores rows bottom-first with a negative (little-endian) scale.
    rows = [pixels_top_first[y * width * 3:(y + 1) * width * 3] for y in range(height)]
    rows.reverse()
    flat = [v for row in rows for v in row]
    with open(path, "wb") as handle:
        handle.write(b"PF\n%d %d\n-1.0\n" % (width, height))
        handle.write(struct.pack("<%df" % len(flat), *flat))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        ref_path = os.path.join(tmp, "ref.pfm")
        same_path = os.path.join(tmp, "same.pfm")
        off_path = os.path.join(tmp, "off.pfm")
        pixels = [0.25, 0.5, 0.75, 1.0, 0.0, 0.5]  # 2x1 RGB
        write_pfm(ref_path, 2, 1, pixels)
        write_pfm(same_path, 2, 1, pixels)
        result = pfm_compare.compare(ref_path, same_path)
        assert result["rmse"] == 0.0 and result["max_err"] == 0.0, result
        assert abs(result["ssim"] - 1.0) < 1e-9, result

        shifted = [v + 0.1 for v in pixels]
        write_pfm(off_path, 2, 1, shifted)
        result = pfm_compare.compare(ref_path, off_path)
        assert abs(result["rmse"] - 0.1) < 1e-6, result
        assert abs(result["max_err"] - 0.1) < 1e-6, result
        mean = sum(pixels) / len(pixels)
        assert abs(result["rel_rmse"] - 0.1 / mean) < 1e-5, result

        # Row order matters: a vertically flipped candidate must differ.
        flip_path = os.path.join(tmp, "flip.pfm")
        img = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0,  # top row
               0.0, 0.0, 1.0, 1.0, 1.0, 1.0]  # bottom row
        write_pfm(ref_path, 2, 2, img)
        write_pfm(flip_path, 2, 2, img[6:] + img[:6])
        result = pfm_compare.compare(ref_path, flip_path)
        assert result["rmse"] > 0.5, result
        assert result["ssim"] < 0.5, result
    print("pfm_compare tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
