"""Wrap a real SDK-encoded mesh in glTF and create malformed variants."""
import base64
import copy
import json
from pathlib import Path
import struct

root = Path(__file__).resolve().parents[1] / 'bin/validation/draco'
metadata = json.loads((root / 'fixture.json').read_text())
semantics = ['POSITION', 'NORMAL', 'TEXCOORD_0', 'COLOR_0', 'JOINTS_0', 'WEIGHTS_0']
ids = [13, 7, 22, 42, 55, 66]
shapes = ['VEC3', 'VEC3', 'VEC2', 'VEC4', 'VEC4', 'VEC4']
types = [5126, 5126, 5126, 5121, 5121, 5121]
accessors = [{'componentType': t, 'type': s, 'count': metadata['points']} for t, s in zip(types, shapes)]
accessors[0].update(min=[0, 0, 0], max=[1, 1, 0])
for index in (3, 5):
    accessors[index]['normalized'] = True
accessors.append({'componentType': 5123, 'type': 'SCALAR', 'count': 6})
primitive = {'attributes': dict(zip(semantics, range(6))), 'indices': 6,
             'extensions': {'KHR_draco_mesh_compression': {'bufferView': 0, 'attributes': dict(zip(semantics, ids))}}}
document = {
    'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
    'nodes': [{'name': 'Root', 'children': [1, 2]}, {'name': 'Mesh', 'mesh': 0, 'skin': 0}, {'name': 'Joint'}],
    'meshes': [{'name': 'Draco fixture', 'primitives': [primitive]}], 'skins': [{'joints': [2]}],
    'buffers': [{'byteLength': metadata['bytes'], 'uri': 'fixture.drc'}],
    'bufferViews': [{'buffer': 0, 'byteLength': metadata['bytes']}], 'accessors': accessors,
    'extensionsUsed': ['KHR_draco_mesh_compression'], 'extensionsRequired': ['KHR_draco_mesh_compression'],
}
(root / 'fixture.gltf').write_text(json.dumps(document), encoding='utf-8')
for name in ('bad-id', 'bad-count', 'bad-type', 'bad-stream', 'bad-duplicate-id'):
    bad = copy.deepcopy(document)
    extension = bad['meshes'][0]['primitives'][0]['extensions']['KHR_draco_mesh_compression']
    if name == 'bad-id':
        extension['attributes']['POSITION'] = 999999
    elif name == 'bad-count':
        bad['accessors'][0]['count'] += 1
    elif name == 'bad-type':
        bad['accessors'][2]['componentType'] = 5123
    elif name == 'bad-stream':
        bad['bufferViews'][0]['byteLength'] = 12
    else:
        extension['attributes']['NORMAL'] = extension['attributes']['POSITION']
    (root / (name + '.gltf')).write_text(json.dumps(bad), encoding='utf-8')

# Dense baseline with the same surface attributes and skin contract.
raw = copy.deepcopy(document)
raw.pop('extensionsUsed')
raw.pop('extensionsRequired')
raw['meshes'][0]['primitives'][0].pop('extensions')
raw['buffers'] = []
raw['bufferViews'] = []
raw['accessors'].append({'componentType': 5126, 'type': 'MAT4', 'count': 1})
raw['skins'][0]['inverseBindMatrices'] = 7
data = bytearray()
values = [([(0,0,0), (1,0,0), (0,1,0), (1,1,0)], 'f'),
          ([(0,0,1)] * 4, 'f'), ([(0,0), (1,0), (0,1), (1,1)], 'f'),
          ([(255,0,0,255), (0,255,0,255), (0,0,255,255), (255,255,0,255)], 'B'),
          ([(0,0,0,0)] * 4, 'B'), ([(255,0,0,0)] * 4, 'B'),
          ([(0,), (1,), (2,), (1,), (3,), (2,)], 'H'),
          ([(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)], 'f')]
for index, (rows, component) in enumerate(values):
    while len(data) % 4:
        data.append(0)
    offset = len(data)
    for row in rows:
        data.extend(struct.pack('<' + component * len(row), *row))
    raw['bufferViews'].append({'buffer': 0, 'byteOffset': offset, 'byteLength': len(data) - offset})
    raw['accessors'][index].update(bufferView=index, count=len(rows))
raw['buffers'].append({'byteLength': len(data), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(data).decode()})
(root / 'dense.gltf').write_text(json.dumps(raw), encoding='utf-8')
optional = copy.deepcopy(raw)
optional['skins'][0].pop('inverseBindMatrices')
(root / 'dense-optional-bind.gltf').write_text(json.dumps(optional), encoding='utf-8')
workspace = root.parents[2]
template = json.loads((workspace / 'asset/Scenes/LightTest.scene').read_text(encoding='utf-8'))
entities = [entity for entity in template['entities'] if 'camera' in entity or 'light' in entity]
for index, (kind, path) in enumerate([
        ('staticModel', 'External/Draco/upstream/testdata/Box/glTF_Binary/Box_Draco.glb'),
        ('animationModel', 'bin/validation/draco/fixture.gltf'),
        ('animationModel', 'bin/validation/draco/dense-optional-bind.gltf')]):
    entities.append({'id': 1000 + index, 'name': {'name': 'Draco smoke ' + kind},
                     'transform': {'position': [-2 + 3 * index, 0, 0], 'rotation': [0, 0, 0], 'scale': [1, 1, 1], 'parent': -1},
                     'material': {'baseColor': [1, 1, 1, 1], 'useTexture': False},
                     kind: {'path': path, 'convert': True, 'playing': False}})
(root / 'runtime.scene').write_text(json.dumps({'format': 'DX12Scene', 'version': 1, 'entities': entities}), encoding='utf-8')
print('Generated Draco unique-ID, UV/color/normalized skin and malformed fixtures')
