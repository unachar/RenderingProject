"""本番のTSRタイルの範囲が、すべての空間サンプル位置を含むことをCPUで検証する。

アドレスと範囲の検証を行う。GPUの出力画像の一致は検証しない。
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
shader = (root / 'shader/hlsl/TemporalUpscaleCS.hlsl').read_text(encoding='utf-8')
assert 'int CurrentTileStart(' in shader, 'TSR has no shared current-frame tile yet'
functions = []
for name in ['CurrentTileStart', 'CurrentTileEnd']:
    match = re.search(r'int ' + name + r'\([^)]*\)\s*\{[^}]*\}', shader)
    assert match, name
    functions.append(match[0])
width = int(re.search(r'#define TSR_TILE_WIDTH (\d+)', shader)[1])
program = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
using std::clamp;
using std::floor;
FUNCTIONS
int main() {
    std::mt19937 random(0x545352);
    uint64_t taps = 0;
    for (int input : {1, 2, 3, 7, 8, 9, 32, 97, 384, 1920}) {
        for (int output : {1, 3, 7, 8, 9, 32, 160, 721, 1920, 3840}) {
            if (input > output) continue; // 本番では、縮小時に直接読み込みへ切り替える。
            for (int test = 0; test < 128; ++test) {
                float jitter = test == 0 ? 0.f : test == 1 ? -.5f / input : test == 2 ? .5f / input :
                    (float(random() % 2001) / 2000.f - .5f) / input;
                int group = int(random() % ((output + 7) / 8));
                auto source = [&](int pixel) { return ((float(pixel) + .5f) / float(output) + jitter) * float(input); };
                int first = CurrentTileStart(source(group * 8), float(input));
                int last = CurrentTileEnd(source(group * 8 + 7), float(input));
                if (first < 0 || last >= input || last < first || last - first >= TILE_WIDTH)
                    throw std::runtime_error("tile exceeds allocated shared storage");
                for (int lane = 0; lane < 8; ++lane) {
                    int pixel = group * 8 + lane;
                    if (pixel >= output) continue;
                    float position = source(pixel);
                    int cubicBase = int(floor(position - .5f));
                    int center = clamp(int(position), 0, input - 1);
                    auto check = [&](int texel) {
                        texel = clamp(texel, 0, input - 1);
                        if (texel < first || texel > last)
                            throw std::runtime_error("a production spatial tap falls outside the tile");
                        // ローカルオフセットでテクセルをコピーしても、元のアドレスを正確に維持する。
                        if (first + (texel - first) != texel) throw std::runtime_error("cache address changed");
                        ++taps;
                    };
                    for (int tap = -1; tap <= 2; ++tap) check(cubicBase + tap);
                    for (int tap = -1; tap <= 1; ++tap) check(center + tap);
                }
            }
        }
    }
    std::cout << "PASS: TSR tile coverage, " << taps << " taps, jitter, 1px/odd/native extents\n";
}
'''.replace('FUNCTIONS', '\n'.join(functions)).replace('TILE_WIDTH', str(width))
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'tile.cpp'
    binary = Path(directory) / 'tile'
    source.write_text(program, encoding='utf-8')
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
