#!/usr/bin/env python3
"""Source-contract tests for Stage 1C shadow material binning.

The binning spans a host struct, an HLSL cbuffer mirror, a shared
Statistics byte-address buffer, and the indirect-argument layout.
These are exactly the agreements a compiler on either side cannot
check; this test pins them by reading the sources:

- view Constants are 272 bytes on both sides, ShadowBinning at
  offset 256, set by PrepareShadowView (shadows) only;
- Statistics bytes: 28 = alpha-bin count, 96 = shadow total, and
  the two-sided reservation reads both;
- BuildArguments emits two 20-byte commands when binning;
- the raster side has the binned mesh shader + dual-draw wiring.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..")


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    traversal_cpp = read("source/virtualgeometry/virtualgeometrytraversal.cpp")
    traversal_h = read("source/virtualgeometry/virtualgeometrytraversal.h")
    traversal_hlsl = read("shader/hlsl/VirtualGeometryTraversalCS.hlsl")
    mesh_renderer = read("source/rendering/meshshader/meshshaderrenderer.cpp")

    # Host/HLSL view-constants agreement.
    check("static_assert(sizeof(Constants) == 272" in traversal_cpp,
          "host Constants must be 272 bytes")
    check("offsetof(Constants, ShadowBinning) == 256" in traversal_cpp,
          "ShadowBinning must sit at offset 256")
    check(re.search(r"uint ShadowBinning;[^\n]*\n\s*uint3 BinningPadding;", traversal_hlsl),
          "HLSL cbuffer mirror must declare ShadowBinning + padding")
    check("shadow.ShadowBinning = 1" in traversal_cpp,
          "PrepareShadowView must enable binning for shadow views")

    # Statistics byte contract.
    check("InterlockedAdd(28," in traversal_hlsl or "ReserveShadowDown" in traversal_hlsl,
          "alpha-bin reservation must use Statistics byte 28")
    check("ReserveShadowTotal" in traversal_hlsl and "96" in traversal_hlsl,
          "shadow total reservation must exist (Statistics byte 96)")
    check(re.search(r"Statistics\.Load\(4\) \+ Statistics\.Load\(28\)", traversal_hlsl),
          "two-sided capacity check must sum opaque (4) + alpha (28)")

    # Dual indirect commands in BuildArguments.
    check("ShadowBinning != 0" in traversal_hlsl,
          "BuildArguments must branch on the binning flag")
    check("20 + 0" in traversal_hlsl or "byteOffset = 20" in traversal_hlsl or
          "IndirectCommands.Store(20" in traversal_hlsl or "Store(20," in traversal_hlsl,
          "second command must be written at byte offset 20")

    # Raster side: binned MS include + dual PSO + dual ExecuteIndirect.
    binned = read("shader/hlsl/VirtualGeometryShadowBinnedMS.hlsl")
    check("VIRTUAL_GEOMETRY_BINNED" in binned and "VirtualGeometryMS.hlsl" in binned,
          "binned shadow MS must wrap the mesh shader with the bin define")
    check("NativeShadowBinnedPSO" in mesh_renderer and "NativeShadowDepthPSO" in mesh_renderer,
          "renderer must create both binned PSOs")
    check(mesh_renderer.count("ExecuteIndirect") >= 2,
          "renderer must issue indirect draws")

    print("traversal binning contract tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
