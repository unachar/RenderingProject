"""Check source registration and shader-facing layouts; this is not a C++ build."""
from collections import Counter
from pathlib import Path
import argparse
import re
import subprocess
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
NS = {'ms': 'http://schemas.microsoft.com/developer/msbuild/2003'}
GROUPS = ['assets', 'core', 'ecs', 'editor', 'entities', 'models',
          'physics', 'rendering', 'scene', 'systems']


def require(condition, message):
    if not condition:
        raise SystemExit('FAIL: ' + message)


def git_text(ref, path):
    return subprocess.check_output(['git', 'show', f'{ref}:{path}'], cwd=ROOT).decode('utf-8-sig')


def struct_body(text, name):
    # These GPU structs contain data members and initializers, with no string literals.
    text = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/', '', text)
    match = re.search(r'\bstruct\s+' + name + r'\s*\{', text)
    require(match is not None, 'missing struct ' + name)
    depth = 1
    end = match.end()
    while depth:
        require(end < len(text), 'unclosed struct ' + name)
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
        end += 1
    return re.sub(r'\s+', '', text[match.end():end - 1])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline-ref', default='69efcf7b2b82ea27f5588ae6dfd749f7d8c1979b')
    args = parser.parse_args()
    expected = {str(path.relative_to(ROOT)).replace('/', '\\')
                for path in (ROOT / 'source').rglob('*')
                if path.suffix in ['.cpp', '.h']}
    for filename in ['DirectX12.vcxproj', 'DirectX12.vcxproj.filters']:
        tree = ET.parse(ROOT / filename)
        items = [item.attrib['Include']
                 for tag in ['ClCompile', 'ClInclude']
                 for item in tree.findall('.//ms:' + tag, NS)
                 if item.attrib.get('Include', '').startswith('source\\')]
        require(set(items) == expected, filename + ': source registration mismatch')
        require(all(count == 1 for count in Counter(items).values()), filename + ': duplicate source')
        if filename.endswith('.filters'):
            filters = {item.attrib['Include'] for item in tree.findall('.//ms:Filter', NS)
                       if 'Include' in item.attrib}
            for item in tree.findall('.//ms:ClCompile', NS) + tree.findall('.//ms:ClInclude', NS):
                if not item.attrib.get('Include', '').startswith('source\\'):
                    continue
                for child in item.findall('ms:Filter', NS):
                    require(child.text in filters, 'undefined Visual Studio filter: ' + child.text)
    props = (ROOT / 'Directory.Build.props').read_text()
    for group in GROUPS:
        require('$(MSBuildThisFileDirectory)source\\' + group + ';' in props,
                'missing include directory: ' + group)
    require('%(AdditionalIncludeDirectories)' in props, 'inherited include paths removed')
    legacy = re.compile(r'\b(?:RendererCore|RendererDraw|RendererResource|RendererShader|rendererResource)\b')
    for path in (ROOT / 'source').rglob('*'):
        if path.suffix in ['.h', '.cpp']:
            require(not legacy.search(path.read_text(encoding='utf-8-sig')), 'legacy class reference: ' + str(path))
    print('PASS: project/filter registration, include directories and retired class references')

    comparisons = [
        ('source/rendererstate.h', 'source/rendering/rendertypes.h', ['Vertex', 'ConstantBuffer3D']),
        ('source/rendererdraw.cpp', 'source/rendering/postprocesspass.cpp', ['PostProcessConstants']),
        ('source/rendererresource.cpp', 'source/rendering/materialbindings.cpp',
         ['MaterialPartShaderConstants', 'PBRConstants']),
        ('source/rendererresource.cpp', 'source/rendering/lightingresources.cpp', ['ShadowConstants']),
    ]
    for old, new, names in comparisons:
        old_text = git_text(args.baseline_ref, old)
        new_text = (ROOT / new).read_text(encoding='utf-8-sig')
        for name in names:
            require(struct_body(old_text, name) == struct_body(new_text, name), 'GPU declaration changed: ' + name)
    print('PASS: six GPU struct declarations match the baseline (sizeof/alignment requires MSVC)')
    tracked = subprocess.check_output(['git', 'ls-tree', '-r', '--name-only', args.baseline_ref, 'shader'], cwd=ROOT).decode().splitlines()
    for filename in tracked:
        original = subprocess.check_output(['git', 'show', f'{args.baseline_ref}:{filename}'], cwd=ROOT)
        require((ROOT / filename).read_bytes() == original, 'shader changed: ' + filename)
    require(set(tracked) == {str(p.relative_to(ROOT)) for p in (ROOT / 'shader').rglob('*') if p.is_file()},
            'shader file set changed')
    print('PASS: shader files match the baseline byte for byte')


if __name__ == '__main__':
    main()
