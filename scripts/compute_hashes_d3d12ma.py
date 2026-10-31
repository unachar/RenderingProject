import hashlib
from pathlib import Path

upstream = Path('External/D3D12MemoryAllocator/upstream')
for f in upstream.rglob('*'):
    if f.is_file():
        rel = f.relative_to(upstream)
        parts = rel.parts
        if '.github' not in parts and 'bin' not in parts and 'docs' not in parts and 'tools' not in parts and 'Tests.cpp' not in str(rel) and 'Tests.h' not in str(rel) and 'Doxyfile' not in str(rel) and '.editorconfig' not in parts:
            data = f.read_bytes()
            h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
            rel_str = rel.as_posix()
            print(f'  "{rel_str}": "{h}",')