#!/usr/bin/env python3
"""Source-contract tests for the Stage 2J probe tail of the
post-process constant block.

Two layouts describe the same buffer: the host PostProcessConstants
(postprocesspass.cpp) packs 11 float4 of quality data as
Quality[11], while common.hlsl's PostProcessParams names those
same 11 slots (GradeParams0 .. QualityReserved). The probe fields
must land at the same offset (byte 400) in both, in the same
order, or the lighting shaders read garbage for the probe boxes
and bindless indices.
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


PROBE_TAIL = ["ProbeBoxCenter", "ProbeBoxExtents", "Probe2BoxCenter",
              "Probe2BoxExtents", "ProbeTextureIndices"]


def main():
    host = read("source/rendering/postprocesspass.cpp")
    hlsl = read("shader/hlsl/common.hlsl")

    # Host: Quality[11] then the probe tail in order.
    check("XMFLOAT4 Quality[11]" in host, "host must pack Quality[11]")
    host_positions = [host.index(f"XMFLOAT4 {name}") for name in PROBE_TAIL]
    check(host_positions == sorted(host_positions), "host probe tail order drifted")
    check(host.index("XMFLOAT4 Quality[11]") < host_positions[0],
          "host probe tail must follow Quality[11]")

    # HLSL: exactly the 11 named float4 slots between
    # RaytracingSunDirection and ProbeBoxCenter, tail in order.
    start = hlsl.rindex("cbuffer PostProcessParams")
    block = hlsl[start:start + 4000]
    middle = block[block.index("RaytracingSunDirection"):block.index("ProbeBoxCenter")]
    named_slots = re.findall(r"float4 (\w+);", middle)
    check(len(named_slots) == 11, f"HLSL middle must name 11 float4 slots, got {len(named_slots)}: {named_slots}")
    hlsl_positions = [block.index(f"float4 {name};") for name in PROBE_TAIL]
    check(hlsl_positions == sorted(hlsl_positions), "HLSL probe tail order drifted")

    # Both layouts: 4 + 8 + 2 + 11 + 5 = 30 float4 = 480 bytes total.
    check("static_assert(sizeof(PostProcessConstants) <= RendererState::g_kPP_CB_ALIGNED_SIZE)" in host,
          "host size guard must stay")

    print("postprocess probe contract tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
