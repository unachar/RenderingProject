import re

with open('Directory.Build.targets', 'r', encoding='utf-8') as f:
    content = f.read()

# Add ColorManagement target after CopyNrdLicense target
content = content.replace(
    '<Target Name="CompileRaytracingShader" BeforeTargets="ClCompile"',
    '''  </Target>
  <Target Name="CompileColorManagementShader" BeforeTargets="ClCompile"
          DependsOnTargets="CompileColorManagementShaderColorManagementCS"
          Condition="$(EnableOcio) == 'true' and $(Platform) == 'x64'" />

  <Target Name="CompileColorManagementShaderColorManagementCS"
          Inputs="$(MSBuildThisFileDirectory)shader\\hlsl\\ColorManagement.hlsl;$(MSBuildThisFileDirectory)shader\\hlsl\\ColorManagementCS.hlsl;$(MSBuildThisFileFullPath);$(MSBuildThisFileDirectory)Directory.Build.props"
          Outputs="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\ColorManagementCS.cso"
          Condition="$(EnableOcio) == 'true' and $(Platform) == 'x64'">
    <MakeDir Directories="$(MSBuildThisFileDirectory)shader\\hlsl\\build" />
    <Exec Command=""$(DirectXShaderCompilerPackageRoot)build\native\bin\x64\dxc.exe" -T cs_6_5 -E main -O3 -Fo "$(MSBuildThisFileDirectory)shader\\hlsl\\build\\ColorManagementCS.cso" "$(MSBuildThisFileDirectory)shader\\hlsl\\ColorManagementCS.hlsl"" />
  </Target>

<Target Name="CompileRaytracingShader" BeforeTargets="ClCompile"''')

with open('Directory.Build.targets', 'w', encoding='utf-8') as f:
    f.write(content)

print('Added ColorManagement targets')

# Add ColorManagementCS.cso to FileWrites
with open('Directory.Build.targets', 'r', encoding='utf-8') as f:
    content = f.read()

content = content.replace(
    '<FileWrites Include="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\DlssPrepareCS.cso;',
    '<FileWrites Include="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\ColorManagementCS.cso;$(MSBuildThisFileDirectory)shader\\hlsl\\build\\DlssPrepareCS.cso;'
)

with open('Directory.Build.targets', 'w', encoding='utf-8') as f:
    f.write(content)

print('Done!')