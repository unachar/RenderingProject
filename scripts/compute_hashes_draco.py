import hashlib
from pathlib import Path

upstream = Path('External/Draco/upstream')
for f in upstream.rglob('*'):
    if f.is_file():
        rel = f.relative_to(upstream)
        parts = rel.parts
        if '.github' not in parts and 'cmake' not in parts and 'docs' not in parts and 'javascript' not in parts and 'maya' not in parts and 'testdata' not in parts and 'third_party' not in parts and 'unity' not in parts and '_test.cc' not in str(rel) and '_test.h' not in str(rel):
            data = f.read_bytes()
            h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
            rel_str = rel.as_posix()
            print(f'  "{rel_str}": "{h}",')