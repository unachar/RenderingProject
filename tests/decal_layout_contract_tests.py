#!/usr/bin/env python3
"""Contract test: DBuffer decal data layout (Stage 1D).

The host packs GpuDecal (112 bytes, static-asserted) and uploads
it verbatim as the DecalData constant buffer. The field order
must agree: WorldToLocal / ExtentsOpacity / Textures / Fade on
the host, DecalWorldToLocal / DecalExtentsOpacity /
DecalTextures / DecalFade in the shader.
"""
import os
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..")
FAILURES = []


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as handle:
        return handle.read()


def check(condition, message):
    if not condition:
        FAILURES.append(message)


def ordered(text, names, label):
    positions = [text.find(name) for name in names]
    check(all(p >= 0 for p in positions), f"{label}: missing one of {names}")
    check(positions == sorted(positions), f"{label}: field order drifted: {names}")


def main():
    header = read("source/rendering/surfacedecals.h")
    check('static_assert(sizeof(GpuDecal) == 112' in header, "GpuDecal must stay 112 bytes")
    ordered(header, ["WorldToLocal[16]", "ExtentsOpacity[4]", "Textures[4]", "Fade[4]"], "GpuDecal")

    raster = read("shader/hlsl/DecalRasterPS.hlsl")
    check("cbuffer DecalData : register(b1)" in raster, "DecalData must stay at b1")
    ordered(raster,
            ["float4x4 DecalWorldToLocal;", "float4 DecalExtentsOpacity;", "uint4 DecalTextures;",
             "float4 DecalFade;"],
            "DecalData")

    if FAILURES:
        for failure in FAILURES:
            print(f"FAIL: {failure}")
        return 1
    print("decal layout contract tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
