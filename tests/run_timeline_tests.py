"""Compile actual timeline property function bodies with CPU-only dependency doubles."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile


def extract(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
        end += 1
    return text[start:end]


parser = argparse.ArgumentParser()
parser.add_argument('--baseline-ref')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
variants = [('refactored', (root / 'source/systems/timelinesystem.cpp').read_text(encoding='utf-8-sig'))]
if args.baseline_ref:
    original = subprocess.check_output(['git', 'show', f'{args.baseline_ref}:source/timelinesystem.cpp'], cwd=root).decode('utf-8-sig')
    variants.insert(0, ('baseline', original))
enum = extract((root / 'source/entities/timelinecomponent.h').read_text(), 'enum class TimelineProperty') + ';'
with tempfile.TemporaryDirectory(prefix='timeline-tests-') as directory:
    directory = Path(directory)
    for label, text in variants:
        bodies = extract(text, 'void ApplyProperty(') + '\n' + extract(text, 'XMFLOAT4 TimeLineSystem::ReadProperty(')
        (directory / 'production_bodies.h').write_text(bodies)
        (directory / 'production_enum.h').write_text(enum)
        executable = directory / label
        subprocess.run([
            os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(directory), str(root / 'tests/timeline/timeline_tests.cpp'),
            '-o', str(executable),
        ], check=True)
        print(label + ':', flush=True)
        subprocess.run([str(executable)], check=True)
