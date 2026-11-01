"""Create deterministic glTF inputs without changing user assets."""
import base64
import json
from pathlib import Path
import struct
import copy

root = Path(__file__).resolve().parents[1] / 'bin/validation/fastgltf'
root.mkdir(parents=True, exist_ok=True)
data = bytearray()
views = []
accessors = []


def accessor(values, component, shape, stride=None, normalized=False):
    while len(data) % 4:
        data.append(0)
    offset = len(data)
    for value in values:
        row = value if isinstance(value, (tuple, list)) else (value,)
        packed = struct.pack('<' + component * len(row), *row)
        data.extend(packed)
        if stride:
            data.extend(bytes(stride - len(packed)))
    view = {'buffer': 0, 'byteOffset': offset, 'byteLength': len(data) - offset}
    if stride:
        view['byteStride'] = stride
    views.append(view)
    item = {'bufferView': len(views) - 1, 'componentType': {'f': 5126, 'B': 5121, 'H': 5123}[component],
            'count': len(values), 'type': shape}
    if normalized:
        item['normalized'] = True
    accessors.append(item)
    return len(accessors) - 1


position = accessor([(0, 0, 0), (1, 0, 0), (0, 1, 0)], 'f', 'VEC3', stride=16)
accessors[position].update(min=[0, 0, 0], max=[1, 1, 0])
normal = accessor([(0, 0, 1)] * 3, 'f', 'VEC3')
tangent = accessor([(1, 0, 0, 1)] * 3, 'f', 'VEC4')
uv = accessor([(0, 0), (1, 0), (0, 1)], 'f', 'VEC2')
color = accessor([(255, 128, 0, 255)] * 3, 'B', 'VEC4', normalized=True)
indices = accessor([0, 1, 2], 'H', 'SCALAR')
joints = accessor([(0, 0, 0, 0)] * 3, 'B', 'VEC4')
weights = accessor([(1, 0, 0, 0)] * 3, 'f', 'VEC4')
inverse = accessor([(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)], 'f', 'MAT4')
delta = accessor([(0, 0, .25)] * 3, 'f', 'VEC3')
times = accessor([0, 1], 'f', 'SCALAR')
accessors[times].update(min=[0], max=[1])
translations = accessor([(0, 0, 0), (0, 2, 0)], 'f', 'VEC3')
sparse_index = accessor([2], 'B', 'SCALAR')
sparse_position = accessor([(0, 1, 0)], 'f', 'VEC3')
packed_position = accessor([(0, 0, 0), (1, 0, 0), (0, 1, 0)], 'f', 'VEC3')
png = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLttAAAAABJRU5ErkJggg=='
primitive = {'attributes': {'POSITION': position, 'NORMAL': normal, 'TANGENT': tangent,
                             'TEXCOORD_0': uv, 'COLOR_0': color}, 'indices': indices, 'material': 0}
document = {
    'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
    'nodes': [{'name': 'Root', 'children': [1, 2], 'translation': [1, 2, 3]},
              {'name': 'MeshNode', 'mesh': 0}, {'name': 'Joint'}],
    'meshes': [{'name': 'Triangle', 'primitives': [primitive]}],
    'materials': [{'name': 'Paint', 'pbrMetallicRoughness': {'baseColorFactor': [.25, .5, .75, .8],
                    'metallicFactor': .3, 'roughnessFactor': .6, 'baseColorTexture': {'index': 0}},
                   'alphaMode': 'BLEND', 'doubleSided': True}],
    'textures': [{'source': 0}], 'images': [{'uri': 'data:image/png;base64,' + png}],
    'buffers': [{'byteLength': len(data), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(data).decode()}],
    'bufferViews': views, 'accessors': accessors,
}
(root / 'material.gltf').write_text(json.dumps(document), encoding='utf-8')
sparse = copy.deepcopy(document)
sparse['accessors'][position]['sparse'] = {
    'count': 1, 'indices': {'bufferView': accessors[sparse_index]['bufferView'], 'componentType': 5121},
    'values': {'bufferView': accessors[sparse_position]['bufferView']}}
(root / 'sparse.gltf').write_text(json.dumps(sparse), encoding='utf-8')
sparse_only = copy.deepcopy(document)
sparse_only['accessors'][position].pop('bufferView')
sparse_only['accessors'][position]['sparse'] = {
    'count': 3, 'indices': {'bufferView': accessors[indices]['bufferView'], 'componentType': 5123},
    'values': {'bufferView': accessors[packed_position]['bufferView']}}
(root / 'sparse-only.gltf').write_text(json.dumps(sparse_only), encoding='utf-8')
external = copy.deepcopy(document)
external['buffers'][0]['uri'] = '外部%20buffer.bin'
(root / '外部 buffer.bin').write_bytes(data)
(root / 'external.gltf').write_text(json.dumps(external), encoding='utf-8')
for name in ('bad-accessor', 'bad-sparse', 'bad-cycle', 'unsupported'):
    invalid = copy.deepcopy(sparse)
    if name == 'bad-accessor':
        invalid['accessors'][position]['count'] = 1000000000
    elif name == 'bad-sparse':
        invalid['accessors'][position]['sparse']['indices']['byteOffset'] = 9999999
    elif name == 'bad-cycle':
        invalid['nodes'][0]['children'] = [0]
    else:
        invalid['extensionsUsed'] = ['EXAMPLE_unsupported']
    (root / (name + '.gltf')).write_text(json.dumps(invalid), encoding='utf-8')
primitive['attributes'].update(JOINTS_0=joints, WEIGHTS_0=weights)
primitive['targets'] = [{'POSITION': delta}]
document['meshes'][0]['weights'] = [.5]
document['nodes'][1]['skin'] = 0
document['skins'] = [{'joints': [2], 'inverseBindMatrices': inverse}]
document['animations'] = [{'name': 'Move', 'samplers': [{'input': times, 'output': translations}],
                           'channels': [{'sampler': 0, 'target': {'node': 2, 'path': 'translation'}}]}]
(root / 'animated.gltf').write_text(json.dumps(document), encoding='utf-8')
bad_morph = copy.deepcopy(document)
bad_morph_bytes = bytearray(data)
delta_view = views[accessors[delta]['bufferView']]
struct.pack_into('<f', bad_morph_bytes, delta_view['byteOffset'], float('nan'))
bad_morph['buffers'][0]['uri'] = 'data:application/octet-stream;base64,' + base64.b64encode(bad_morph_bytes).decode()
(root / 'bad-morph-nan.gltf').write_text(json.dumps(bad_morph), encoding='utf-8')
workspace = root.parents[2]
template = json.loads((workspace / 'asset/Scenes/LightTest.scene').read_text(encoding='utf-8'))
entities = [entity for entity in template['entities'] if 'camera' in entity or 'light' in entity]
for index, (kind, path) in enumerate([
        ('staticModel', 'External/RTXNS/external/donut/thirdparty/cgltf/fuzz/data/Box.glb'),
        ('animationModel', 'External/Draco/upstream/testdata/CesiumMan/glTF_Binary/CesiumMan.glb')]):
    entity = {'id': 1000 + index, 'name': {'name': 'fastgltf smoke ' + kind},
              'transform': {'position': [-2 + 3 * index, 0, 0], 'rotation': [0, 0, 0], 'scale': [1, 1, 1], 'parent': -1},
              'material': {'baseColor': [1, 1, 1, 1], 'useTexture': True},
              kind: {'path': path, 'convert': True}}
    if kind == 'animationModel':
        entity[kind].update(animationPaths=[path], animations=['Walk'], current='Walk', playing=True, speed=1)
    entities.append(entity)
(root / 'runtime.scene').write_text(json.dumps({'format': 'DX12Scene', 'version': 1, 'entities': entities}), encoding='utf-8')
print('Generated material/skin/morph/animation glTF fixtures')
