"""Compare captured output pixels; this does not assess temporal artifacts or HDR."""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def rgb_metrics(first: np.ndarray, second: np.ndarray) -> dict:
    """Encoded 8-bit RGB metrics; these are not linear HDR quality metrics."""
    first = np.asarray(first, dtype=np.float64)
    second = np.asarray(second, dtype=np.float64)
    if (first.shape != second.shape or first.ndim != 3 or first.shape[2] != 3
            or min(first.shape[:2]) == 0):
        raise ValueError("Metrics require matching nonempty HxWx3 RGB captures")
    for pixels in (first, second):
        if not np.isfinite(pixels).all() or np.any(pixels < 0) or np.any(pixels > 255):
            raise ValueError("RGB capture values must be finite and between 0 and 255")

    # Summed areas avoid materializing an HxWx11x11x3 array for local SSIM.
    size = min(11, *first.shape[:2])

    def local_mean(pixels: np.ndarray) -> np.ndarray:
        integral = np.pad(pixels.cumsum(axis=0).cumsum(axis=1),
                          ((1, 0), (1, 0), (0, 0)))
        total = (integral[size:, size:] - integral[:-size, size:]
                 - integral[size:, :-size] + integral[:-size, :-size])
        return total / (size * size)

    first_mean = local_mean(first)
    second_mean = local_mean(second)
    first_variance = np.maximum(local_mean(first * first) - first_mean**2, 0)
    second_variance = np.maximum(local_mean(second * second) - second_mean**2, 0)
    covariance = local_mean(first * second) - first_mean * second_mean
    c1 = (0.01 * 255)**2
    c2 = (0.03 * 255)**2
    similarity = (((2 * first_mean * second_mean + c1) * (2 * covariance + c2))
                  / ((first_mean**2 + second_mean**2 + c1)
                     * (first_variance + second_variance + c2)))
    return {
        "rgbRootMeanSquareError": float(np.sqrt(np.mean((first - second)**2))),
        "rgbSsim": float(np.mean(similarity)),
        "ssimWindowSize": size,
        "ssimMethod": "uniform valid windows, population covariance, mean over RGB",
        "metricDomain": "encoded 8-bit RGB, alpha excluded; not linear HDR",
    }


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
    result.update(rgb_metrics(first[..., :3], second[..., :3]))
    text = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
