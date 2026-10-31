import hashlib
from pathlib import Path

upstream = Path('External/fastgltf/upstream')
for f in upstream.rglob('*'):
    if f.is_file() and '.github' not in f.parts and 'cmake' not in f.parts and 'docs' not in f.parts and 'examples' not in f.parts and 'tests' not in f.parts and 'src' not in f.parts:
        data = f.read_bytes()
        h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
        rel = f.relative_to(upstream).as_posix()
        print(f'  "{rel}": "{h}",')