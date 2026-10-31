#!/usr/bin/env python3
"""Source-contract tests for the Stage 2H reference integrator.

Pins the host/shader agreements of ReferenceIntegrator: the
30-DWORD capture constants (root signature + both uploads), the
scene-buffer order (host SceneBufferIndex vs shader registers
t1..t8), the 4-wide UAV table (beauty, albedo AOV, normal AOV,
direct split), and the fixture builder's material/light layouts
against the shader's readers.
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
    host = read("source/rendering/referenceintegrator.cpp")
    shader = read("shader/hlsl/ReferencePathTraceCS.hlsl")
    fixture = read("source/rendering/referencefixture.cpp")

    check("parameters[0].InitAsConstants(30, 0);" in host, "root sig must declare 30 constants")
    check(host.count("SetComputeRoot32BitConstants(0, 30,") == 2,
          "both beauty and AOV uploads must send 30 constants")
    check("D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0" in host, "UAV table must span u0..u3")

    # Scene buffer order: host enum vs shader register order.
    enum_block = host[host.index("enum SceneBufferIndex"):host.index("kSceneBufferCount")]
    host_order = re.findall(r"k(\w+)\s*(?:= 0)?\s*[,=]", enum_block + ",")
    host_order = [name for name in host_order if name != "SceneBufferCount"]
    shader_regs = re.findall(r"StructuredBuffer<\w+> (\w+) : register\(t(\d)\)", shader)
    shader_order = [name for name, _ in sorted(shader_regs, key=lambda p: int(p[1]))]
    expected = ["InstanceTransforms0", "InstanceTransforms1", "InstanceInfo", "MaterialData",
                "GeometryInfo", "Positions", "Indices", "Lights"]
    check(shader_order == expected, f"shader register order drifted: {shader_order}")
    check(host_order == ["Transforms0", "Transforms1", "InstanceInfo", "Materials",
                         "GeometryInfo", "Positions", "Indices", "Lights"],
          f"host scene-buffer order drifted: {host_order}")

    # UAV register roles.
    for reg, name in [(0, "Accumulation"), (1, "AovAlbedo"), (2, "AovNormal"), (3, "DirectAccumulation")]:
        check(f"{name} : register(u{reg})" in shader, f"{name} must be u{reg}")

    # Fixture layouts the shader reads: 3 float4 per material, MIS link in entry 3.y.
    check("MarkMaterialLight" in fixture or "MarkMaterialLight" in read("source/rendering/referencefixture.h"),
          "fixture must expose the material->light link writer")

    print("reference integrator contract tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
