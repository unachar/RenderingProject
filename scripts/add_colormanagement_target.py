#!/usr/bin/env python3
"""Add ColorManagement shader compilation target to Directory.Build.targets"""

import re

# Read the original file
with open('Directory.Build.targets', 'r', encoding='utf-8') as f:
    content = f.read()

# Find the CopyNrdLicense target and add ColorManagement target after it
old_pattern = r'(<Target Name="CopyNrdLicense" AfterTargets="Build" Condition="\$\'\(EnableNrd\)\' == \'true\' and \$\'\(Platform\)\' == \'x64\'">\s*<Copy SourceFiles="\$\(NrdSdkRoot\)LICENSE.txt" DestinationFiles="\$\(OutDir\)NVIDIA_NRD_LICENSE.txt" SkipUnchangedFiles="true" />\s*</Target>\s*)(<Target Name="CompileRaytracingShader")'

new_content = r'''\1
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

\2'''

content = re.sub(old_pattern, new_content, content, flags=re.DOTALL)

# Add ColorManagementCS.cso to FileWrites
filewrites_pattern = r'(<FileWrites Include="\$\(MSBuildThisFileDirectory\)shader\\hlsl\\build\\DlssPrepareCS\.cso;)'
filewrites_replacement = r'<FileWrites Include="$(MSBuildThisFileDirectory)shader\hlsl\build\ColorManagementCS.cso;\1'
content = re.sub(filewrites_pattern, filewrites_replacement, content, count=1)

# Write the modified content
with open('Directory.Build.targets', 'w', encoding='utf-8') as f:
    f.write(content)

print("Done!")