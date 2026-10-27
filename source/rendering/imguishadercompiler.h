#pragma once

#include <windows.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstring>

#pragma comment(lib, "dxcompiler.lib")
#pragma comment(lib, "d3dcompiler.lib") // ID3DBlob allocation only.

namespace ImGuiShaderCompiler
{
    // ImGui embeds its two small shaders in the backend source rather than files.
    inline HRESULT Compile(const char* source, const wchar_t* profile, ID3DBlob** shader)
    {
        if (!shader)
        {
            return E_POINTER;
        }
        *shader = nullptr;
        if (!source || !profile)
        {
            return E_INVALIDARG;
        }

        Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
        HRESULT result = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
        if (FAILED(result))
        {
            return result;
        }
        const DxcBuffer buffer{source, std::strlen(source), DXC_CP_UTF8};
        const wchar_t* arguments[] = {L"-E", L"main", L"-T", profile, L"-O3", L"-HV", L"2021"};
        Microsoft::WRL::ComPtr<IDxcResult> compiled;
        result = compiler->Compile(&buffer, arguments, ARRAYSIZE(arguments), nullptr, IID_PPV_ARGS(&compiled));
        if (FAILED(result))
        {
            return result;
        }
        if (!compiled)
        {
            return E_FAIL;
        }

        Microsoft::WRL::ComPtr<IDxcBlobUtf8> diagnostics;
        if (SUCCEEDED(compiled->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&diagnostics), nullptr)) && diagnostics)
        {
            OutputDebugStringA(diagnostics->GetStringPointer());
        }
        HRESULT status = E_FAIL;
        result = compiled->GetStatus(&status);
        if (FAILED(result))
        {
            return result;
        }
        if (FAILED(status))
        {
            return status;
        }

        Microsoft::WRL::ComPtr<IDxcBlob> object;
        result = compiled->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
        if (FAILED(result))
        {
            return result;
        }
        if (!object)
        {
            return E_FAIL;
        }
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        result = D3DCreateBlob(object->GetBufferSize(), &blob);
        if (FAILED(result))
        {
            return result;
        }
        std::memcpy(blob->GetBufferPointer(), object->GetBufferPointer(), object->GetBufferSize());
        *shader = blob.Detach();
        return S_OK;
    }
}
