"""Compare accepted real-renderer AA captures; camera-motion changes are not a temporal-quality reference."""
import argparse
import csv
import json
from pathlib import Path
import struct

import numpy as np
from PIL import Image


def texture(path):
    data = path.read_bytes()
    width, height, pitch, fmt = struct.unpack_from("<4I", data)
    dtype, channels = {10: ("<f2", 4), 41: ("<f4", 1)}[fmt]
    row_bytes = width * channels * np.dtype(dtype).itemsize
    if pitch < row_bytes or len(data) != 16 + height * pitch:
        raise ValueError(f"Invalid texture footprint: {path}")
    rows = np.frombuffer(data, np.uint8, offset=16).reshape(height, pitch)
    result = rows[:, :row_bytes].copy().view(dtype).reshape(height, width, channels)
    if not np.isfinite(result).all():
        raise ValueError(f"Nonfinite captured texture: {path}")
    return result.astype(np.float32)


def timing(directory):
    rows = list(csv.DictReader((directory / "timings.csv").open()))
    if len(rows) < 32 or len({r["Serial"] for r in rows}) != len(rows):
        raise ValueError("Insufficient or duplicated GPU timing samples")
    result = {"samples": len(rows)}
    for name in ("CpuRenderMs", "GpuRenderMs", "GBufferGpuMs"):
        values = np.array([float(row[name]) for row in rows])
        if not np.isfinite(values).all() or (values < 0).any():
            raise ValueError("Invalid timing value")
        result[name] = dict(mean=float(values.mean()), median=float(np.median(values)),
                            p25=float(np.percentile(values, 25)), p75=float(np.percentile(values, 75)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("off", type=Path)
    parser.add_argument("on", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    directories = [args.off, args.on]
    runs = [json.loads((p / "run.json").read_text(encoding="utf-8-sig")) for p in directories]
    for run, mode in zip(runs, ("Off", "On")):
        if run["Mode"] != mode or not all(run[k] for k in ("Succeeded", "SourceRestored", "CsoRestored")):
            raise ValueError(f"Run is not accepted/restored: {mode}")
    for name in ("SourceBeforeSha256", "CsoBeforeSha256", "Configuration"):
        if runs[0][name] != runs[1][name]:
            raise ValueError(f"Mismatched baseline: {name}")
    frame_results, masks = [], []
    luminances = [[], []]
    weights = np.array([.2126, .7152, .0722], dtype=np.float32)
    for frame in (96, *range(100, 116)):
        material = [texture(p / f"material-{frame}.bin") for p in directories]
        depth = [texture(p / f"depth-{frame}.bin")[:, :, 0] for p in directories]
        scene = [texture(p / f"scene-{frame}.bin")[:, :, :3] for p in directories]
        png = [np.asarray(Image.open(p / f"scene-{frame}.png").convert("RGB")) for p in directories]
        if any(image.shape[:2] != (640, 960) for image in material + scene + png):
            raise ValueError("Fixture extent is not 960x640")
        visible = (depth[0] > 0) & (depth[1] > 0)
        classes = material[0][:, :, 3]
        left = np.indices(classes.shape)[1] < classes.shape[1] // 2
        curved = visible & (classes == 11) & left
        control_masks = {"flat_pbr": visible & (classes == 11) & ~left,
                         "toon": visible & (classes == 4), "unlit": visible & (classes == 10)}
        controls = {}
        for name, mask in control_masks.items():
            if mask.sum() < 1000:
                raise ValueError(f"Missing visible control: {name}")
            delta = np.abs(scene[1][mask] - scene[0][mask])
            controls[name] = dict(pixels=int(mask.sum()), hdr_rgb_identical=bool(np.array_equal(scene[0][mask], scene[1][mask])),
                                  png_rgb_identical=bool(np.array_equal(png[0][mask], png[1][mask])),
                                  material_identical=bool(np.array_equal(material[0][mask], material[1][mask])),
                                  hdr_maximum_absolute_difference=float(delta.max()))
        roughness = [image[:, :, 1][curved] for image in material]
        if len(roughness[0]) < 1000:
            raise ValueError("Missing curved PBR pixels")
        result = dict(frame=frame, geometry_depth_identical=bool(np.array_equal(depth[0], depth[1])),
                      material_classes_identical=bool(np.array_equal(material[0][:, :, 3], material[1][:, :, 3])),
                      other_material_channels_identical=bool(np.array_equal(material[0][:, :, [0, 2, 3]], material[1][:, :, [0, 2, 3]])),
                      curved_roughness_mean_off=float(roughness[0].mean()),
                      curved_roughness_mean_on=float(roughness[1].mean()),
                      curved_pixels=int(curved.sum()),
                      curved_roughness_changed_pixels=int((roughness[0] != roughness[1]).sum()), controls=controls)
        frame_results.append(result)
        if frame >= 100:
            masks.append(curved)
            for index in range(2):
                luminances[index].append(scene[index] @ weights)

    interior = np.logical_and.reduce(masks)
    for _ in range(2):
        padded = np.pad(interior, 1)
        interior &= padded[:-2, 1:-1] & padded[2:, 1:-1] & padded[1:-1, :-2] & padded[1:-1, 2:]
    if interior.sum() < 1000:
        raise ValueError("Insufficient common sphere interior for camera-phase statistics")
    motion = {}
    for name, images in zip(("off", "on"), luminances):
        values = np.stack(images)[:, interior]
        rms = float(np.sqrt(np.mean(np.diff(values, axis=0) ** 2)))
        mean = float(values.mean())
        motion[name] = dict(common_interior_pixels=int(interior.sum()), mean_luminance=mean,
                            maximum_luminance=float(values.max()),
                            screen_space_frame_change_rms=rms,
                            frame_change_rms_over_mean=rms / max(mean, 1e-8),
                            per_frame_luminance_sum=values.sum(axis=1, dtype=np.float64).tolist())
    invariants = all(r["geometry_depth_identical"] and r["material_classes_identical"] and
                     r["other_material_channels_identical"] and
                     r["curved_roughness_mean_on"] > r["curved_roughness_mean_off"] and
                     r["curved_roughness_changed_pixels"] > 1000 and
                     all(c["hdr_rgb_identical"] and c["png_rgb_identical"] and c["material_identical"]
                         for c in r["controls"].values()) for r in frame_results)
    report = dict(invariants_pass=invariants, off=str(args.off.resolve()), on=str(args.on.resolve()),
                  configuration=runs[0]["Configuration"], frames=frame_results,
                  timings={"off": timing(args.off), "on": timing(args.on)}, camera_phase_statistics=motion,
                  limitations="One isolated fixture run per mode. GPU timings exclude capture readbacks but are not a controlled FPS speedup test. "
                              "Camera motion is not reprojected or compared with supersampled ground truth; frame-change RMS is descriptive, "
                              "not proof of flicker reduction. Mean and peak brightness may change with filtering.")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({k: report[k] for k in ("invariants_pass", "timings", "camera_phase_statistics")}, indent=2))
    if not invariants:
        raise SystemExit("AA control/material invariants differ; inspect report before accepting")


if __name__ == "__main__":
    main()
