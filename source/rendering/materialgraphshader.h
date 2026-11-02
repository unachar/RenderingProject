#pragma once
#include "materialgraphcompiler.h"
#include <d3dcompiler.h>
#include <wrl.h>

struct MaterialGraphShader
{
    Microsoft::WRL::ComPtr<ID3DBlob> Vertex;
    Microsoft::WRL::ComPtr<ID3DBlob> Pixel;
    std::string Error;
    uint64_t Hash = 0;

    bool Success() const
    {
        return Vertex && Pixel;
    }
};

class MaterialGraphShaderCompiler
{
  public:
    // CPU-only: safe to run on a shader worker. Uses the engine's thread-local DXC and DXIL cache.
    static MaterialGraphShader Compile(const MaterialGraphProgram& program);
};
