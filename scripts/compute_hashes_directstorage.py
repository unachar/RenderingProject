import hashlib
from pathlib import Path

upstream = Path('External/DirectStorage/upstream')
for f in upstream.rglob('*'):
    if f.is_file():
        rel = f.relative_to(upstream)
        parts = rel.parts
        if '.git' not in parts and 'Docs' not in parts and 'Samples' not in parts:
            data = f.read_bytes()
            h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
            rel_str = rel.as_posix()
            print(f'  "{rel_str}": "{h}",')