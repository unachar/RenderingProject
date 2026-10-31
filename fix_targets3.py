import re

# Read the original file
with open('original_targets_utf8.xml', 'r', encoding='utf-8') as f:
    content = f.read()

# Insert ColorManagement targets after CompileNrdRaytracingShadersNrdComposeAmbientCS (before CompilePhase5Shaders)
insert_marker = '''  <Target Name="CompilePhase5Shaders" BeforeTargets="ClCompile"'''

new_targets = '''  <Target Name="CompileColorManagementShader" BeforeTargets="ClCompile"
          DependsOnTargets="CompileColorManagementShaderColorManagementCS"
          Condition="'$(EnableOcio)' == 'true' and '$(Platform)' == 'x64'" />

  <Target Name="CompileColorManagementShaderColorManagementCS"
          Inputs="$(MSBuildThisFileDirectory)shader\\hlsl\\ColorManagement.hlsl;$(MSBuildThisFileDirectory)shader\\hlsl\\ColorManagementCS.hlsl;$(MSBuildThisFileFullPath);$(MSBuildThisFileDirectory)Directory.Build.props"
          Outputs="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\ColorManagementCS.cso"
          Condition="'$(EnableOcio)' == 'true' and '$(Platform)' == 'x64'">
    <MakeDir Directories="$(MSBuildThisFileDirectory)shader\\hlsl\\build" />
    <Exec Command=""$(DirectXShaderCompilerPackageRoot)build\\native\\bin\\x64\\dxc.exe" -T cs_6_5 -E main -O3 -Fo "$(MSBuildThisFileDirectory)shader\\hlsl\\build\\ColorManagementCS.cso" "$(MSBuildThisFileDirectory)shader\\hlsl\\ColorManagementCS.hlsl"" />
  </Target>

'''

content = content.replace(insert_marker, new_targets + insert_marker)

# Add ColorManagementCS.cso to FileWrites
filewrites_marker = '''<FileWrites Include="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\DlssPrepareCS.cso;'''
filewrites_new = '''<FileWrites Include="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\ColorManagementCS.cso;$(MSBuildThisFileDirectory)shader\\hlsl\\build\\DlssPrepareCS.cso;'''

content = content.replace(filewrites_marker, filewrites_new)

# Write to Directory.Build.targets
with open('Directory.Build.targets', 'w', encoding='utf-8') as f:
    f.write(content)

print('Done!')