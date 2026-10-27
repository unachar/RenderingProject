#include <windows.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl.h>
#include <future>
#include <fstream>
#include <iostream>
#include <latch>
#include <string>
#include <vector>
using namespace std;
#include "dxccompiler.h"

int main()
{
    const auto source = filesystem::absolute("build/thumbnail-tests/concurrent.hlsl");
    ofstream(source) << "[numthreads(1,1,1)] void main() {}\n";
    promise<void> start;
    latch ready(2);
    auto gate = start.get_future().share();
    const auto compile = [&source, &ready, gate] {
        auto* state = &GetCompilerState();
        ready.count_down();
        gate.wait();
        Microsoft::WRL::ComPtr<ID3DBlob> code;
        const auto result = DxcCompileFromFileCompat(source.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
                                                     "cs_6_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr);
        return make_pair(state, SUCCEEDED(result) && code && code->GetBufferSize() > 0);
    };
    auto first = async(launch::async, compile);
    auto second = async(launch::async, compile);
    auto* uiCompiler = &GetCompilerState();
    ready.wait();
    start.set_value();
    const auto firstResult = first.get();
    const auto secondResult = second.get();
    if (!firstResult.second || !secondResult.second || firstResult.first == secondResult.first ||
        firstResult.first == uiCompiler || secondResult.first == uiCompiler)
    {
        cerr << "Concurrent thumbnail compilers must succeed with separate compiler state\n";
        return 1;
    }
    cout << "Concurrent thumbnail DXC compilation and compiler isolation PASS\n";
}
