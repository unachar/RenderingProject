#!/usr/bin/env python3
"""Contract test: FidelityFX SPD Hi-Z constants agreement.

HiZSpdCS reads SpdConstants (b0) as two uint2: WorkGroupOffset,
then NumWorkGroupsAndMips. The host uploads exactly 4 DWORDs
{0, 0, groupsX * groupsY, spdMips} at root parameter 4.
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


def main():
    shader = read("shader/hlsl/HiZSpdCS.hlsl")
    check("cbuffer SpdConstants : register(b0)" in shader, "SpdConstants must stay at b0")
    offset = shader.find("uint2 WorkGroupOffset;")
    mips = shader.find("uint2 NumWorkGroupsAndMips;")
    check(0 <= offset < mips, "SpdConstants field order must stay WorkGroupOffset, NumWorkGroupsAndMips")

    host = read("source/rendering/occlusionculling.cpp")
    check("const UINT constants[4] = {0, 0, groupsX * groupsY, spdMips};" in host,
          "SPD constants initializer drifted")
    check("SetComputeRoot32BitConstants(4, 4, constants, 0)" in host,
          "SPD constants must upload 4 DWORDs at root parameter 4")
    check("SetComputeRootUnorderedAccessView(3, g_SpdCounter->GetGPUVirtualAddress())" in host,
          "SPD global counter must stay a root UAV at parameter 3")

    if FAILURES:
        for failure in FAILURES:
            print(f"FAIL: {failure}")
        return 1
    print("spd layout contract tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
