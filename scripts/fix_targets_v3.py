#!/usr/bin/env python3
"""Fix Directory.Build.targets - add ColorManagement target"""

import xml.etree.ElementTree as ET

# Register namespaces
ET.register_namespace('', 'http://schemas.microsoft.com/developer/msbuild/2003')

# Parse the original file
tree = ET.parse('original_targets.xml')
root = tree.getroot()

# Find the Project element
ns = {'msbuild': 'http://schemas.microsoft.com/developer/msbuild/2003'}

# Find the CopyNrdLicense target
for target in root.findall('.//msbuild:Target[@Name="CopyNrdLicense"]', ns):
    # Get the parent
    parent = target.getparent() if hasattr(target, 'getparent') else None
    # Find the index of the next target (CompileRaytracingShader)
    # We need to insert after CopyNrdLicense and before CompileRaytracingShader
    pass

# Simpler approach: read as text and do careful replacement
with open('Directory.Build.targets', 'r', encoding='utf-8') as f:
    content = f.read()

# Restore original first
with open('original_targets.xml', 'r', encoding='utf-8') as f:
    content = f.read()

# Add ColorManagement target after CopyNrdLicense target
# Find the end of CopyNrdLicense target
marker = '''  </Target>
  <Target Name="CompileRaytracingShader" BeforeTargets="ClCompile"'''

replacement = '''  </Target>
  <Target Name="CompileColorManagementShader" BeforeTargets="ClCompile"
          DependsOnTargets="CompileColorManagementShaderColorManagementCS"
          Condition="$(EnableOcio) == 'true' and $(Platform) == 'x64'" />

  <Target Name="CompileColorManagementShaderColorManagementCS"
          Inputs="$(MSBuildThisFileDirectory)shader\hlsl\ColorManagement.hlsl;$(MSBuildThisFileDirectory)shader\hlsl\ColorManagementCS.hlsl;$(MSBuildThisFileFullPath);$(MSBuildThisFileDirectory)Directory.Build.props"
          Outputs="$(MSBuildThisFileDirectory)shader\hlsl\build\ColorManagementCS.cso"
          Condition="$(EnableOcio) == 'true' and $(Platform) == 'x64'">
    <MakeDir Directories="$(MSBuildThisFileDirectory)shader\hlsl\build" />
    <Exec Command=""$(DirectXShaderCompilerPackageRoot)build\native\bin\x64\dxc.exe" -T cs_6_5 -E main -O3 -Fo "$(MSBuildThisFileDirectory)shader\hlsl\build\ColorManagementCS.cso" "$(MSBuildThisFileDirectory)shader\hlsl\ColorManagementCS.hlsl"" />
  </Target>

<Target Name="CompileRaytracingShader" BeforeTargets="ClCompile"'''

with open('Directory.Build.targets', 'r', encoding='utf-8') as f:
    content = f.read()

# Restore original first
with open('original_targets.xml', 'r', encoding='utf-8') as f:
    content = f.read()

# Do the replacement
content = content.replace(
    '<Target Name="CompileRaytracingShader" BeforeTargets="ClCompile"',
    '''  </Target>
  <Target Name="CompileColorManagementShader" BeforeTargets="ClCompile"
          DependsOnTargets="CompileColorManagementShaderColorManagementCS"
          Condition="$(EnableOcio) == \'true\' and $(Platform) == \'x64'" />

  <Target Name="CompileColorManagementShaderColorManagementCS"
          Inputs="$(MSBuildThisFileDirectory)shader\hlsl\ColorManagement.hlsl;$(MSBuildThisFileDirectory)shader\hlsl\ColorManagementCS.hlsl;$(MSBuildThisFileFullPath);$(MSBuildThisFileDirectory)Directory.Build.props"
          Outputs="$(MSBuildThisFileDirectory)shader\hlsl\build\ColorManagementCS.cso"
          Condition="$(EnableOcio) == \'true\' and $(Platform) == \'x64\'">
    <MakeDir Directories="$(MSBuildThisFileDirectory)shader\hlsl\build" />
    <Exec Command=""$(DirectXShaderCompilerPackageRoot)build\native\bin\x64\dxc.exe" -T cs_6_5 -E main -O3 -Fo "$(MSBuildThisFileDirectory)shader\hlsl\build\ColorManagementCS.cso" "$(MSBuildThisFileDirectory)shader\hlsl\ColorManagementCS.hlsl"" />
  </Target>

<Target Name="CompileRaytracingShader" BeforeTargets="ClCompile"'''
)

with open('Directory.Build.targets', 'w', encoding='utf-8') as f:
    f.write(content)

print("Added ColorManagement targets")

# Add ColorManagementCS.cso to FileWrites
with open('Directory.Build.targets', 'r', encoding='utf-8') as f:
    content = f.read()

content = content.replace(
    '<FileWrites Include="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\DlssPrepareCS.cso;',
    '<FileWrites Include="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\ColorManagementCS.cso;$(MSBuildThisFileDirectory)shader\\hlsl\\build\\DlssPrepareCS.cso;'
)

with open('Directory.Build.targets', 'w', encoding='utf-8') as f:
    f.write(content)

print("Done!")