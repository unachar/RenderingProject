"""Run CPU RenderGraph tests; does not validate Windows, MSVC or actual GPU work."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--baseline-ref', help='Also run the same tests against this git commit')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CXX', 'g++')
if not shutil.which(compiler):
    raise SystemExit('A C++17 compiler is required; set CXX if necessary.')

with tempfile.TemporaryDirectory(prefix='rendergraph-tests-') as temporary:
    temporary = Path(temporary)
    variants = [('refactored', root / 'source/rendering')]
    if args.baseline_ref:
        baseline = temporary / 'baseline'
        baseline.mkdir()
        for filename in ['rendergraph.cpp', 'rendergraph.h']:
            result = subprocess.run(
                ['git', 'show', f'{args.baseline_ref}:source/{filename}'],
                cwd=root, check=True, capture_output=True)
            (baseline / filename).write_bytes(result.stdout)
        variants.insert(0, ('baseline', baseline))
    for label, source in variants:
        executable = temporary / (label + '-tests')
        subprocess.run([
            compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(root / 'tests/rendergraph'), '-I' + str(source),
            str(source / 'rendergraph.cpp'),
            str(root / 'tests/rendergraph/rendergraph_tests.cpp'),
            '-o', str(executable),
        ], check=True)
        print(label + ':', flush=True)
        subprocess.run([str(executable)], check=True)
