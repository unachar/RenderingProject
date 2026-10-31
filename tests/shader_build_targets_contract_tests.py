#!/usr/bin/env python3
"""Linux port of scripts/test-shader-build-targets.ps1.

The PowerShell check only runs on Windows; this mirror keeps the
same gates runnable anywhere: tracked DXC target count, no
duplicate names, per-target Exec/Outputs shape, -O3 preserved,
the build file itself listed as a dependency, the ray-telemetry
input group defined, and every declared input file present.
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.join(os.path.dirname(__file__), "..")
FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)


def main():
    targets_path = os.path.join(ROOT, "Directory.Build.targets")
    tree = ET.parse(targets_path)
    project = tree.getroot()
    ns = project.tag[1:].split("}")[0] if project.tag.startswith("{") else ""
    q = (lambda name: f"{{{ns}}}{name}") if ns else (lambda name: name)

    targets = project.findall(q("Target"))
    by_name = {}
    for target in targets:
        by_name.setdefault(target.get("Name"), []).append(target)
    duplicates = [name for name, group in by_name.items() if len(group) > 1]
    check(not duplicates, f"duplicate target names: {duplicates}")

    shader_targets = [t for t in targets if t.get("Inputs") and t.get("Name") != "CompileShadowCacheShaders"]
    check(len(shader_targets) == 45, f"expected 45 tracked DXC outputs, got {len(shader_targets)}")

    shadow_cache = by_name.get("CompileShadowCacheShaders", [])
    check(len(shadow_cache) == 1, "CompileShadowCacheShaders missing")
    if shadow_cache:
        execs = shadow_cache[0].findall(q("Exec"))
        outputs = (shadow_cache[0].get("Outputs") or "").split(";")
        check(len(execs) == 2 and len(outputs) == 2, "shadow cache must track both VS and PS outputs")

    ray_inputs = None
    for group in project.findall(q("PropertyGroup")):
        node = group.find(q("RayTelemetryShaderInputs"))
        if node is not None and node.text and node.text.strip():
            ray_inputs = node.text
    check(ray_inputs is not None, "RayTelemetryShaderInputs must be defined")

    root_abs = os.path.abspath(ROOT)
    targets_abs = os.path.abspath(targets_path)
    for target in shader_targets:
        name = target.get("Name")
        execs = target.findall(q("Exec"))
        check(len(execs) == 1, f"{name}: expected exactly one Exec")
        outputs = target.get("Outputs") or ""
        check(";" not in outputs, f"{name}: Outputs must be a single file")
        inputs = target.get("Inputs") or ""
        check("$(MSBuildThisFileFullPath)" in inputs, f"{name}: build file missing from Inputs")
        if execs:
            check(" -O3 " in (execs[0].get("Command") or ""), f"{name}: -O3 missing")
        expanded = inputs.replace("$(RayTelemetryShaderInputs)", ray_inputs or "")
        for item in expanded.split(";"):
            path = item.replace("$(MSBuildThisFileDirectory)", root_abs + os.sep)
            path = path.replace("$(MSBuildThisFileFullPath)", targets_abs)
            path = path.replace("\\", os.sep)  # Inputs use Windows separators
            if not os.path.exists(path):
                condition = target.get("Condition") or ""
                if "External\\NRD\\sdk\\" in path.replace("/", "\\") and "$(EnableNrd)" in condition:
                    continue
                check(False, f"{name}: missing dependency {path}")

    if FAILURES:
        for failure in FAILURES:
            print(f"FAIL: {failure}")
        return 1
    print("shader build targets contract tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
