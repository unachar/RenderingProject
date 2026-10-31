import hashlib
from pathlib import Path

upstream = Path('External/KTX-Software/upstream/external/basis_universal')
for f in upstream.rglob('*'):
    if f.is_file() and 'test_files' not in f.parts and '.github' not in f.parts and 'bin' not in f.parts and 'build' not in f.parts and 'cmd_help' not in f.parts and 'contrib' not in f.parts and 'encoder' not in f.parts and 'encoder_lib' not in f.parts and 'example' not in f.parts and 'example_capi' not in f.parts and 'example_transcoding' not in f.parts and 'khronos' not in f.parts and 'LICENSES' not in f.parts and 'OpenCL' not in f.parts and 'python' not in f.parts and 'shader_deblocking' not in f.parts and 'webgl' not in f.parts and 'zstd' not in f.parts:
        data = f.read_bytes()
        h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
        rel = f.relative_to(upstream).as_posix()
        print(f'  "{rel}": "{h}",')