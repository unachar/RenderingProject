import xml.etree.ElementTree as ET

NS = 'http://schemas.microsoft.com/developer/msbuild/2003'
ET.register_namespace('', NS)

# Parse the original file
tree = ET.parse('original_targets_utf8.xml')
root = tree.getroot()

# Find CompilePhase5Shaders target
compile_phase5 = None
for target in root.findall(f'{{{NS}}}Target'):
    if target.get('Name') == 'CompilePhase5Shaders':
        compile_phase5 = target
        break

# Create new targets with namespace
color_mgmt = ET.SubElement(root, f'{{{NS}}}Target')
color_mgmt.set('Name', 'CompileColorManagementShader')
color_mgmt.set('BeforeTargets', 'ClCompile')
color_mgmt.set('DependsOnTargets', 'CompileColorManagementShaderColorManagementCS')
color_mgmt.set('Condition', "'$(EnableOcio)' == 'true' and '$(Platform)' == 'x64'")

color_mgmt_cs = ET.SubElement(root, f'{{{NS}}}Target')
color_mgmt_cs.set('Name', 'CompileColorManagementShaderColorManagementCS')
color_mgmt_cs.set('Inputs', r'$(MSBuildThisFileDirectory)shader\hlsl\ColorManagement.hlsl;$(MSBuildThisFileDirectory)shader\hlsl\ColorManagementCS.hlsl;$(MSBuildThisFileFullPath);$(MSBuildThisFileDirectory)Directory.Build.props')
color_mgmt_cs.set('Outputs', r'$(MSBuildThisFileDirectory)shader\hlsl\build\ColorManagementCS.cso')
color_mgmt_cs.set('Condition', "'$(EnableOcio)' == 'true' and '$(Platform)' == 'x64'")

make_dir = ET.SubElement(color_mgmt_cs, f'{{{NS}}}MakeDir')
make_dir.set('Directories', r'$(MSBuildThisFileDirectory)shader\hlsl\build')

exec_elem = ET.SubElement(color_mgmt_cs, f'{{{NS}}}Exec')
exec_elem.set('Command', r'"$(DirectXShaderCompilerPackageRoot)build\native\bin\x64\dxc.exe" -T cs_6_5 -E main -O3 -Fo "$(MSBuildThisFileDirectory)shader\hlsl\build\ColorManagementCS.cso" "$(MSBuildThisFileDirectory)shader\hlsl\ColorManagementCS.hlsl"')

# Reorder: move new targets before CompilePhase5Shaders
children = list(root)
idx_phase5 = children.index(compile_phase5)
root.remove(color_mgmt)
root.remove(color_mgmt_cs)
root.insert(idx_phase5, color_mgmt_cs)
root.insert(idx_phase5, color_mgmt)

# Add ColorManagementCS.cso to FileWrites
for itemgroup in root.findall(f'{{{NS}}}ItemGroup'):
    for fw in itemgroup.findall(f'{{{NS}}}FileWrites'):
        includes = fw.get('Include', '')
        if 'DlssPrepareCS.cso' in includes:
            fw.set('Include', r'$(MSBuildThisFileDirectory)shader\hlsl\build\ColorManagementCS.cso;' + includes)
            break

# Write
tree.write('Directory.Build.targets', encoding='utf-8', xml_declaration=True)
print('Done!')