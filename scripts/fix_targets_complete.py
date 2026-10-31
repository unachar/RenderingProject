#!/usr/bin/env python3
"""Fix Directory.Build.targets - complete rewrite with ColorManagement"""

# Read original file
with open('original_targets.xml', 'rb') as f:
    raw = f.read()
    if raw.startswith(b'\xff\xfe') or raw.startswith(b'\xfe\xff') or raw.startswith(b'\xef\xbb\xbf'):
        content = raw.decode('utf-8-sig')
    else:
        content = raw.decode('utf-8')

# Split into lines
lines = content.splitlines(keepends=True)

# Find the line with CompileRaytracingShader target
insert_index = -1
for i, line in enumerate(lines):
    if '<Target Name="CompileRaytracingShader" BeforeTargets="ClCompile"' in line:
        insert_index = i
        break

if insert_index == -1:
    print("ERROR: Could not find CompileRaytracingShader target")
    exit(1)

# Insert ColorManagement targets before CompileRaytracingShader
insert_lines = [
    '  </Target>\n',
    '  <Target Name="CompileColorManagementShader" BeforeTargets="ClCompile"\n',
    '          DependsOnTargets="CompileColorManagementShaderColorManagementCS"\n',
    '          Condition="$(EnableOcio) == \'true\' and $(Platform) == \'x64\'" />\n',
    '\n',
    '  <Target Name="CompileColorManagementShaderColorManagementCS"\n',
    '          Inputs="$(MSBuildThisFileDirectory)shader\\hlsl\\ColorManagement.hlsl;$(MSBuildThisFileDirectory)shader\\hlsl\\ColorManagementCS.hlsl;$(MSBuildThisFileFullPath);$(MSBuildThisFileDirectory)Directory.Build.props"\n',
    '          Outputs="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\ColorManagementCS.cso"\n',
    '          Condition="$(EnableOcio) == \'true\' and $(Platform) == \'x64\'" />\n',
    '\n',
]

# Insert the new lines
for i, new_line in enumerate(reversed(new_lines)):
    lines.insert(insert_index, new_line)

# Now add ColorManagementCS.cso to FileWrites
# Find the FileWrites line
for i, line in enumerate(lines):
    if '<FileWrites Include="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\DlssPrepareCS.cso;' in line:
        lines[i] = line.replace(
            '<FileWrites Include="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\DlssPrepareCS.cso;',
            '<FileWrites Include="$(MSBuildThisFileDirectory)shader\\hlsl\\build\\ColorManagementCS.cso;$(MSBuildThisFileDirectory)shader\\hlsl\\build\\DlssPrepareCS.cso;'
        )
        break

# Write the modified content
with open('Directory.Build.targets', 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines))

print("Done!")