import hashlib
from pathlib import Path

upstream = Path('External/FidelityFX-FSR2/upstream')
for f in upstream.rglob('*'):
    if f.is_file():
        rel = f.relative_to(upstream)
        parts = rel.parts
        if '.git' not in parts and 'docs' not in parts and 'build' not in parts and 'media' not in parts and 'tools' not in parts and 'libs' not in parts and 'Common' not in parts and 'DX12' not in parts and 'VK' not in parts and 'GpuParticles' not in parts and 'GpuParticleShaders' not in parts:
            data = f.read_bytes()
            h = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
            rel_str = rel.as_posix()
            print(f'  "{rel_str}": "{h}",')