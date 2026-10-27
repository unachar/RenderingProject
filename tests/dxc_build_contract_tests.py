"""Check the evaluated shader compiler settings in every project configuration."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--verify-outputs", action="store_true", help="also require compiled DXIL outputs")
args = parser.parse_args()
outputs = set()
VSWHERE = Path(r"C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
installation = subprocess.check_output(
    [str(VSWHERE), "-latest", "-products", "*", "-requires",
     "Microsoft.Component.MSBuild", "-property", "installationPath"], text=True
).strip()
msbuild = Path(installation) / "MSBuild/Current/Bin/MSBuild.exe"

for platform in ("x64", "Win32"):
    for configuration in ("Debug", "Release"):
        result = subprocess.run(
            [str(msbuild), "DirectX12.vcxproj", "-nologo",
             f"-p:Configuration={configuration}", f"-p:Platform={platform}",
             "-getProperty:FxCompileToolExe,FXCToolPath", "-getItem:FxCompile,ClCompile"],
            cwd=ROOT, capture_output=True, text=True, check=True,
        )
        settings = json.loads(result.stdout)
        properties = settings["Properties"]
        assert properties["FxCompileToolExe"] == "dxc.exe", properties
        compiler = Path(properties["FXCToolPath"]) / properties["FxCompileToolExe"]
        assert compiler.is_file(), compiler
        shaders = settings["Items"]["FxCompile"]
        active = [shader for shader in shaders if shader.get("ExcludedFromBuild") != "true"]
        assert active, "No shader compilation items were evaluated"
        for shader in active:
            assert int(shader["ShaderModel"].split(".")[0]) >= 6, shader["Identity"]
            assert shader["ShaderType"] in ("Pixel", "Vertex", "Compute"), shader["Identity"]
            assert shader["ObjectFileOutput"].endswith(".cso"), shader["Identity"]
            outputs.add(Path(shader["ObjectFileOutput"]))
        print(f"{configuration}|{platform}: DXC, SM6, {len(active)} shaders PASS")

# Include compiled vendor sources such as ImGui's embedded shaders in this check.
sources = {Path(item["FullPath"]) for item in settings["Items"]["ClCompile"]}
sources.update((ROOT / "source").rglob("*.h"))
for source in sources:
    text = source.read_text(encoding="utf-8-sig")
    code = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    assert not re.search(r"\bD3DCompile(?:FromFile)?\s*\(", code), source
print("Runtime and compiled vendor sources: no legacy shader compiler calls PASS")

if args.verify_outputs:
    for output in sorted(outputs):
        data = output.read_bytes()
        assert len(data) >= 32 and data[:4] == b"DXBC", output
        chunk_count = struct.unpack_from("<I", data, 28)[0]
        offsets = struct.unpack_from(f"<{chunk_count}I", data, 32)
        assert any(data[offset:offset + 4] == b"DXIL" for offset in offsets), output
    print(f"Compiled outputs: {len(outputs)} DXIL containers PASS")
