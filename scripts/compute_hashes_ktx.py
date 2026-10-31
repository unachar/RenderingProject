import hashlib
from pathlib import Path

upstream = Path('External/KTX-Software/upstream')
for f in upstream.rglob('*'):
    if f.is_file() and 'external' not in f.parts and '.github' not in f.parts and 'docs' not in f.parts and 'tests' not in f.parts and 'tools' not in f.parts and 'cmake' not in f.parts and 'examples' not in f.parts and 'icons' not in f.parts and 'pkgdoc' not in f.parts and 'scripts' not in f.parts and 'utils' not in f.parts and 'vcpkg-triplets' not in f.parts:
        data = f.read_bytes()
        h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
        rel = f.relative_to(upstream).as_posix()
        print(f'  "{rel}": "{h}",')