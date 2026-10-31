#!/usr/bin/env python3
"""Add ColorManagement shader compilation target to Directory.Build.targets"""

import re

# Read the original file
with open('Directory.Build.targets', 'r', encoding='utf-8') as f:
    lines = f.readlines()

# Find the line with CopyNrdLicense target end and add ColorManagement target after it
new_lines = []
i = 0
while i < len(lines):
    line = lines[i]
    new_lines.append(line)
    
    # Check if this is the end of CopyNrdLicense target
    if '</Target>' in line and i > 0 and 'CopyNrdLicense' in lines[i-1]:
        # Add ColorManagement target after this
        new_lines.append('\n')
        new_lines.append('  <Target Name="CompileColorManagementShader" BeforeTargets="ClCompile"\n')
        new_lines.append('          DependsOnTargets="CompileColorManagementShaderColorManagementCS"\n')
        new_lines.append('          Condition="$(EnableOcio) == \'true\' and $(Platform) == \'x64\'" />\n')
        new_lines.append('\n')
        new_lines.append('  <Target Name="CompileColorManagementShaderColorManagementCS"\n')
        new_lines.append('          Inputs="$(MSBuildThisFileDirectory)shader\\hlsl\\ColorManagement.hlsl;$(MSBuildThisFileDirectory)shader\\hlsl\\ColorManagementCS.hlsl;$(MSBuildThisFileFullPath);$(MSBuildThisFileDirectory)Directory.Build.props"\n')
        new_lines.append('          Outputs="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\ColorManagementCS.cso"\n')
        new_lines.append('          Condition="$(EnableOcio) == \'true\' and $(Platform) == \'x64\'">\n')
        new_lines.append('    <MakeDir Directories="$(MSBuildThisFileDirectory)shader\\hlsl\\build" />\n')
        new_lines.append('    <Exec Command=""$(DirectXShaderCompilerPackageRoot)build\\native\\bin\\x64\\dxc.exe" -T cs_6_5 -E main -O3 -Fo "$(MSBuildThisFileDirectory)shader\\hlsl\\build\\ColorManagementCS.cso" "$(MSBuildThisFileDirectory)shader\\hlsl\\ColorManagementCS.hlsl"" />\n')
        new_lines.append('  </Target>\n')
        new_lines.append('\n')
    
    i += 1

# Join and add ColorManagementCS.cso to FileWrites
content = ''.join(new_lines)

# Add ColorManagementCS.cso to FileWrites
content = content.replace(
    '<FileWrites Include="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\DlssPrepareCS.cso;',
    '<FileWrites Include="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\ColorManagementCS.cso;$(MSBuildThisFileDirectory)shader\\hlsl\\build\\DlssPrepareCS.cso;'
)

with open('Directory.Build.targets', 'w', encoding='utf-8') as f:
    f.write(content)

print("Done!")