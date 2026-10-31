import hashlib
from pathlib import Path

upstream = Path('External/STBN/upstream')
for f in upstream.rglob('*'):
    if f.is_file() and 'Assets' not in f.parts and 'External' not in f.parts and 'Tools' not in f.parts and 'Support' not in f.parts and '.git' not in f.parts:
        data = f.read_bytes()
        h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
        rel = f.relative_to(upstream).as_posix()
        print(f'  "{rel}": "{h}",')