"""Execute the production HLSL scan with a cooperative C++ barrier scheduler.

Requires Python 3 and a C++20 compiler (CXX, default g++). This verifies integer
results, arbitrary lane arrival order and the shared-add work bound, not GPU time.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
shader = (ROOT / 'shader/hlsl/VirtualGeometryBinningCS.hlsl').read_text(encoding='utf-8')
start = shader.index('uint ExclusiveScan(')
end = shader.index('\n[numthreads', start)
scan = shader[start:end].replace('uint ExclusiveScan(', 'Task ExclusiveScan(')
scan = scan.replace('[unroll]', '').replace('return ', 'co_return ')
threads = int(re.search(r'#define PREFIX_THREADS (\d+)', shader)[1])
program = r'''
#include <algorithm>
#include <array>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>
using uint = uint32_t;
constexpr uint PREFIX_THREADS = THREAD_COUNT;
uint64_t additions = 0;
struct Word {
    uint value = 0;
    operator uint() const { return value; }
    Word& operator=(uint v) { value = v; return *this; }
    Word& operator+=(uint v) { ++additions; value += v; return *this; }
};
std::array<Word, PREFIX_THREADS> gs_Scan;
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
SCAN_BODY
int main() {
    std::mt19937 random(0x125a);
    std::vector<uint> schedule(PREFIX_THREADS);
    std::iota(schedule.begin(), schedule.end(), 0u);
    for (uint test = 0; test < 96; ++test) {
        std::array<uint, PREFIX_THREADS> input{}, expected{};
        uint total = 0;
        for (uint i = 0; i < PREFIX_THREADS; ++i) {
            input[i] = test == 0 ? 0 : test == 1 ? 1 : test == 2 ? (i == 511 ? 17 : 0) : random();
            expected[i] = total;
            total += input[i]; // Also exercise uint wraparound, as HLSL does.
        }
        additions = 0;
        std::vector<Task> tasks;
        for (uint lane = 0; lane < PREFIX_THREADS; ++lane) tasks.push_back(ExclusiveScan(input[lane], lane));
        uint rounds = 0;
        while (!tasks[0].handle.done()) {
            std::shuffle(schedule.begin(), schedule.end(), random);
            for (uint lane : schedule) {
                if (tasks[lane].handle.done()) throw std::runtime_error("nonuniform barrier arrival");
                tasks[lane].handle.resume();
            }
            if (++rounds > 100) throw std::runtime_error("scan did not complete");
        }
        for (uint lane = 0; lane < PREFIX_THREADS; ++lane) {
            if (!tasks[lane].handle.done() || tasks[lane].handle.promise().value != expected[lane])
                throw std::runtime_error("production scan differs from serial uint prefix");
            tasks[lane].handle.destroy();
        }
        if (additions > 2 * PREFIX_THREADS)
            throw std::runtime_error("scan exceeds linear shared-add work budget");
    }
    std::cout << "PASS: 96 production scan cases, shuffled lanes, uint overflow, linear work\n";
}
'''.replace('THREAD_COUNT', str(threads)).replace('SCAN_BODY', scan)
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'scan.cpp'
    binary = Path(directory) / 'scan'
    source.write_text(program, encoding='utf-8')
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
