import hashlib
from pathlib import Path

upstream = Path('External/RTXTF/upstream')
for f in upstream.rglob('*'):
    if f.is_file():
        rel = f.relative_to(upstream)
        parts = rel.parts
        if '.git' not in parts and '.gitlab-ci.yml' not in str(rel) and '.gitattributes' not in str(rel) and '.gitignore' not in str(rel) and '.gitmodules' not in str(rel) and 'docs' not in parts and 'assets' not in parts and 'external' not in parts and 'samples' not in parts and 'support' not in parts:
            data = f.read_bytes()
            h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
            rel_str = rel.as_posix()
            print(f'  "{rel_str}": "{h}",')