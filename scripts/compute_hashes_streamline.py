import hashlib
from pathlib import Path

upstream = Path('External/Streamline/upstream')
for f in upstream.rglob('*'):
    if f.is_file():
        rel = f.relative_to(upstream)
        parts = rel.parts
        if '.git' not in parts and '.agents' not in parts and '.claude-plugin' not in parts and 'docs' not in parts and 'scripts' not in parts and 'tools' not in parts and 'external' not in parts and 'shaders' not in parts and 'NIS_shaders.h' not in str(rel) and 'DroidSans-Mono.h' not in str(rel) and 'imgui.h' not in str(rel) and 'imguiTypes.h' not in str(rel) and '.pdf' not in str(rel):
            data = f.read_bytes()
            h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
            rel_str = rel.as_posix()
            print(f'  "{rel_str}": "{h}",')