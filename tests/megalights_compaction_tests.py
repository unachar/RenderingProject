"""レーンの到達順を並べ替え、本番の粗いライト候補の圧縮ループを実行する。

交差判定の結果はテストデータから与え、実際のHLSLの順位計算、共有メモリへの書き込み、
バリアをC++20のコルーチンで実行する。GPUシェーダー全体のコンパイルは行わない。
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
shader = (root / 'shader/hlsl/MegaLightsGridCS.hlsl').read_text(encoding='utf-8')
begin = shader.index('    for (uint base = 0u; base < lightCount; base += 64u)')
end = shader.index('\n    if (groupIndex == 0u)', begin + 1)
loop = shader[begin:end]
# 本番の圧縮処理を維持し、交差判定の結果だけを差し替える。
loop = loop.replace('MLSphereIntersectsAabb(MLLightViewSphere(Lights[lightIndex]), boundsMin, boundsMax)',
                    'hits[lightIndex]')
local = 'uint listCount = 0u;' if 'uint listCount = 0u;' in shader[:begin] else ''
result = 'listCount' if local else 'gs_ListCount'
program = r'''
#include <algorithm>
#include <array>
#include <bit>
#include <coroutine>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>
using uint = uint32_t;
std::array<uint, 4096> gs_List;
uint gs_HitBits[2], gs_ListCount;
std::array<bool, 4096> hits;
uint countbits(uint value) { return std::popcount(value); }
void InterlockedOr(uint& target, uint value) { target |= value; }
struct Task {
    struct promise_type {
        uint value = 0;
        Task get_return_object() { return {std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_value(uint v) { value = v; }
        void unhandled_exception() { std::terminate(); }
    };
    std::coroutine_handle<promise_type> handle;
};
#define GroupMemoryBarrierWithGroupSync() co_await std::suspend_always{}
Task Build(uint groupIndex, uint lightCount) {
LOCAL_COUNT
PRODUCTION_LOOP
    co_return RESULT;
}
int main() {
    std::mt19937 random(0x4d4c);
    std::array<uint, 64> schedule;
    std::iota(schedule.begin(), schedule.end(), 0u);
    for (uint count : {0u, 1u, 31u, 32u, 33u, 63u, 64u, 65u, 255u, 256u, 2000u, 4095u, 4096u}) {
        for (uint mode = 0; mode < 4; ++mode) {
            std::vector<uint> expected;
            for (uint i = 0; i < count; ++i) {
                hits[i] = mode == 0 ? false : mode == 1 ? true : mode == 2 ? i % 32 == 0 : random() % 3 == 0;
                if (hits[i]) expected.push_back(i);
            }
            gs_List.fill(0xffffffffu);
            gs_ListCount = 0;
            std::vector<Task> tasks;
            for (uint lane = 0; lane < 64; ++lane) tasks.push_back(Build(lane, count));
            uint rounds = 0;
            while (!tasks[0].handle.done()) {
                std::shuffle(schedule.begin(), schedule.end(), random);
                for (uint lane : schedule) tasks[lane].handle.resume();
                if (++rounds > 300) throw std::runtime_error("compaction did not complete");
            }
            for (auto task : tasks) {
                if (!task.handle.done() || task.handle.promise().value != expected.size())
                    throw std::runtime_error("lane list count mismatch");
                task.handle.destroy();
            }
            if (!std::equal(expected.begin(), expected.end(), gs_List.begin()))
                throw std::runtime_error("light order or membership differs from serial reference");
            if (rounds > 3 * ((count + 63) / 64) + 1)
                throw std::runtime_error("compaction exceeds three barriers per chunk");
        }
    }
    std::cout << "PASS: 52 production compaction cases, shuffled lanes, exact light order, barrier budget\n";
}
'''.replace('LOCAL_COUNT', local).replace('PRODUCTION_LOOP', loop).replace('RESULT', result)
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'lights.cpp'
    binary = Path(directory) / 'lights'
    source.write_text(program, encoding='utf-8')
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
