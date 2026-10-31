import hashlib
from pathlib import Path

upstream = Path('External/OpenColorIO/upstream')
for f in upstream.rglob('*'):
    if f.is_file():
        rel = f.relative_to(upstream)
        parts = rel.parts
        if '.github' not in parts and 'docs' not in parts and 'tests' not in parts and 'ASWF' not in parts and 'ext' not in parts and 'vendor' not in parts and 'ci' not in parts and 'docker' not in parts and 'nuke' not in parts and 'openfx' not in parts and 'troubleshooting' not in parts and 'dev' not in parts and 'apps' not in str(rel) and 'bindings' not in str(rel) and 'cmake' not in str(rel):
            data = f.read_bytes()
            h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
            rel_str = rel.as_posix()
            print(f'  "{rel_str}": "{h}",')