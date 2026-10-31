import hashlib
from pathlib import Path

upstream = Path('External/tracy/upstream')
for f in upstream.rglob('*'):
    if f.is_file():
        rel = f.relative_to(upstream)
        parts = rel.parts
        if '.github' not in parts and '.omp' not in parts and '.vscode' not in parts and 'capture' not in parts and 'cmake' not in parts and 'csvexport' not in parts and 'doc' not in parts and 'dtl' not in parts and 'examples' not in parts and 'extra' not in parts and 'getopt' not in parts and 'icon' not in parts and 'import' not in parts and 'manual' not in parts and 'merge' not in parts and 'monitor' not in parts and 'profiler' not in parts and 'server' not in parts and 'tests' not in parts and 'update' not in parts and 'util' not in parts and 'TracyClient.F90' not in str(rel):
            data = f.read_bytes()
            h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
            rel_str = rel.as_posix()
            print(f'  "{rel_str}": "{h}",')