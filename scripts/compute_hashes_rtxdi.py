import hashlib
from pathlib import Path

upstream = Path('External/RTXDI/upstream')
for f in upstream.rglob('*'):
    if f.is_file():
        rel = f.relative_to(upstream)
        parts = rel.parts
        if 'Assets' not in parts and 'Doc' not in parts and 'Samples' not in parts and 'Support' not in parts and '.git' not in parts and 'External/donut' not in str(rel) and 'External/NRD' not in str(rel) and 'External/cxxopts' not in str(rel) and 'External/NVAPI' not in str(rel):
            data = f.read_bytes()
            h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
            rel_str = rel.as_posix()
            print(f'  "{rel_str}": "{h}",')