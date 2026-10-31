#!/usr/bin/env python3
"""Source-contract tests for the SSR constant blocks.

The resolve-constants bug fixed in 970b4aae (root signature and
upload stopped at 26 of the declared 28 DWORDs, so the
checkerboard parity never reached the shader) is the class of
error this test exists to prevent: it pins the host struct, the
HLSL cbuffer, the root-signature constant count, and the upload
count to the same number for both the trace and resolve passes.
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
    module = read("source/rendering/screenspacereflections.cpp")
    header = read("source/rendering/screenspacereflections.h")
    trace = read("shader/hlsl/ScreenSpaceReflectionsTraceCS.hlsl")
    resolve = read("shader/hlsl/ScreenSpaceReflectionsResolveCS.hlsl")

    # Trace constants: 56 DWORDs everywhere.
    check("parameters[0].InitAsConstants(56, 0);" in module, "trace root sig must declare 56 constants")
    check("SetComputeRoot32BitConstants(0, 56, &traceConstants, 0)" in module,
          "trace upload must send 56 constants")
    check("float RoughnessLimit" in header and "float Padding[3]" in header,
          "host TraceConstants must end with RoughnessLimit + 3 padding (56 DWORDs)")
    check("float RoughnessLimit;" in trace and "float3 TracePadding;" in trace,
          "trace cbuffer must mirror the 56-DWORD tail")
    check(header.index("CheckerboardParity") < header.index("RoughnessLimit"),
          "host field order: parity before roughness limit")

    # Resolve constants: 28 DWORDs everywhere, parity at DWORD 26.
    check("resolveParameters[0].InitAsConstants(28, 0);" in module,
          "resolve root sig must declare 28 constants")
    check("SetComputeRoot32BitConstants(0, 28, &resolve, 0)" in module,
          "resolve upload must send 28 constants")
    check(re.search(r"int32_t CheckerboardParity;\s*float Padding;", module),
          "host ResolveConstants tail must be parity + padding")
    check(re.search(r"int CheckerboardParity;[^\n]*\n\s*float ResolvePadding;", resolve),
          "resolve cbuffer tail must be parity + padding")

    # Heap-slot bindings: trace UAVs at slots 3 (result) and 5 (hit
    # points) via separate single-range tables 6 and 7; resolve
    # reads trace SRV (2), hit SRV (4), previous accumulation
    # (6 + previous) and writes result UAV (1) / accumulation UAV
    # (8 + current).
    check("SetComputeRootDescriptorTable(6, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpu, 3, g_DescriptorIncrement))" in module,
          "trace result UAV must bind heap slot 3")
    check("SetComputeRootDescriptorTable(7, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpu, 5, g_DescriptorIncrement))" in module,
          "trace hit-points UAV must bind heap slot 5")
    check("6 + accumulationPrevious" in module and "8 + g_AccumulationCurrent" in module,
          "resolve ping-pong slots must stay 6+previous (SRV) and 8+current (UAV)")
    check("NumDescriptors = 10" in module, "module heap must stay 10 slots")

    print("ssr constants contract tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
