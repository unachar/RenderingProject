"""Optional grammar checks, not a substitute for MSVC compilation or linking."""
from collections import Counter
from pathlib import Path
import argparse
import re
import subprocess

from tree_sitter import Language, Parser
import tree_sitter_cpp


ROOT = Path(__file__).resolve().parents[1]
PARSER = Parser(Language(tree_sitter_cpp.language()))
RENAMES = {'renderercore.h': 'graphicsdevice.h', 'renderercore.cpp': 'graphicsdevice.cpp',
           'renderer.cpp': 'rendererstate.cpp', 'renderershader.h': 'shaderpipelines.h',
           'renderershader.cpp': 'shaderpipelines.cpp', 'rendererutils.h': 'shaderpaths.h',
           'rendererutils.cpp': 'shaderpaths.cpp'}


def analyze(files):
    errors = Counter()
    selections = []
    for name, data in files.items():
        nodes = [PARSER.parse(data).root_node]
        while nodes:
            node = nodes.pop()
            nodes.extend(node.children)
            if node.is_error or node.is_missing:
                errors[(name, node.type, data[node.start_byte:node.end_byte])] += 1
            if node.type == 'conditional_expression' and name not in ['d3dx12.h', 'dxccompiler.h']:
                selections.append((name, node.start_point.row + 1))
    return errors, selections


def cyclic_pairs(files):
    headers = {name: data for name, data in files.items() if name.endswith('.h')}
    edges = {name: set(re.findall(r'^\s*#include\s*"([^/\\"]+)"', data.decode('utf-8-sig'), re.M)) & headers.keys()
             for name, data in headers.items()}
    reachable_pairs = set()
    for start in edges:
        visited = set()
        nodes = list(edges[start])
        while nodes:
            node = nodes.pop()
            if node in visited:
                continue
            visited.add(node)
            nodes.extend(edges.get(node, []))
        reachable_pairs.update((start, other) for other in visited if other != start)
    return {tuple(sorted(pair)) for pair in reachable_pairs if (pair[1], pair[0]) in reachable_pairs}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline-ref', default='69efcf7b2b82ea27f5588ae6dfd749f7d8c1979b')
    args = parser.parse_args()
    paths = subprocess.check_output(['git', 'ls-tree', '-r', '--name-only', args.baseline_ref, 'source'], cwd=ROOT).decode().splitlines()
    baseline = {Path(path).name: subprocess.check_output(['git', 'show', f'{args.baseline_ref}:{path}'], cwd=ROOT)
                for path in paths if Path(path).suffix in ['.h', '.cpp']}
    current = {path.name: path.read_bytes() for path in (ROOT / 'source').rglob('*') if path.suffix in ['.h', '.cpp']}
    old_errors, _ = analyze(baseline)
    new_errors, selections = analyze(current)
    additional = new_errors - old_errors
    if additional or selections:
        raise SystemExit(f'FAIL: additional grammar errors={list(additional)}, ternary operators={selections}')
    # Normalize old header names for dependency comparison.
    normalized = {}
    for name, data in baseline.items():
        for old, new in RENAMES.items():
            data = data.replace(('"' + old + '"').encode(), ('"' + new + '"').encode())
        normalized[RENAMES.get(name, name)] = data
    additional_cycles = cyclic_pairs(current) - cyclic_pairs(normalized)
    if additional_cycles:
        raise SystemExit('FAIL: additional cyclic header pairs=' + str(sorted(additional_cycles)))
    print(f'PASS: no additional grammar error nodes (baseline={sum(old_errors.values())}, current={sum(new_errors.values())})')
    print('PASS: no self-code ternary operators; no additional cyclic header pairs')
    # The baseline had definitions absent from this class's public/private declarations.
    header = current['instancingsystem.h'].decode('utf-8-sig')
    data = current['instancingsystem.cpp']
    nodes = [PARSER.parse(data).root_node]
    while nodes:
        node = nodes.pop()
        nodes.extend(node.children)
        if node.type != 'function_definition':
            continue
        body = node.child_by_field_name('body')
        if body is None:
            continue
        signature = data[node.start_byte:body.start_byte].decode()
        match = re.search(r'InstancingSystem::(\w+)\s*\(', signature)
        if match and not re.search(r'\b' + match[1] + r'\s*\(', header):
            raise SystemExit('FAIL: missing InstancingSystem declaration: ' + match[1])
    print('PASS: InstancingSystem implementations have corresponding header declarations')


if __name__ == '__main__':
    main()
