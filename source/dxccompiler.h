#pragma once

#include <windows.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "dxcompiler.lib")

// Runtime DXC adapter used while the project still contains legacy
// D3DCompileFromFile call sites. It compiles through IDxcCompiler3 and copies
// the resulting DXIL container into an ID3DBlob so existing PSO creation code
// does not need to change all at once.
namespace RenderingDxcDetail
{
    using Microsoft::WRL::ComPtr;

    struct CompilerState
    {
        ComPtr<IDxcUtils> Utils;
        ComPtr<IDxcCompiler3> Compiler;
        ComPtr<IDxcIncludeHandler> IncludeHandler;
        HRESULT Status = E_FAIL;

        CompilerState()
        {
            Status = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&Utils));
            if (FAILED(Status))
            {
                return;
            }

            Status = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&Compiler));
            if (FAILED(Status))
            {
                return;
            }

            Status = Utils->CreateDefaultIncludeHandler(&IncludeHandler);
        }
    };

    inline CompilerState& GetCompilerState()
    {
        static CompilerState state;
        return state;
    }

    inline std::wstring ToWide(const char* text)
    {
        if (!text)
        {
            return {};
        }

        const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (length <= 1)
        {
            return {};
        }

        std::wstring output(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text, -1, output.data(), length);
        output.pop_back();
        return output;
    }

    inline std::wstring PromoteLegacyProfile(const char* target)
    {
        std::wstring profile = ToWide(target);
        const size_t legacyMarker = profile.find(L"_5_");
        if (legacyMarker != std::wstring::npos)
        {
            const size_t stageEnd = profile.find(L'_');
            if (stageEnd != std::wstring::npos)
            {
                profile = profile.substr(0, stageEnd + 1) + L"6_0";
            }
        }
        return profile;
    }

    inline HRESULT CreateLegacyBlob(const void* data, SIZE_T size, ID3DBlob** output)
    {
        if (!output)
        {
            return E_POINTER;
        }
        *output = nullptr;

        ComPtr<ID3DBlob> blob;
        HRESULT result = D3DCreateBlob(size, &blob);
        if (FAILED(result))
        {
            return result;
        }

        if (size > 0 && data)
        {
            memcpy(blob->GetBufferPointer(), data, size);
        }
        *output = blob.Detach();
        return S_OK;
    }

    inline void SetDiagnosticBlob(const std::string& diagnostics, ID3DBlob** output)
    {
        if (!output)
        {
            return;
        }
        *output = nullptr;

        if (diagnostics.empty())
        {
            return;
        }

        ComPtr<ID3DBlob> blob;
        if (FAILED(D3DCreateBlob(diagnostics.size() + 1, &blob)))
        {
            return;
        }

        memcpy(blob->GetBufferPointer(), diagnostics.c_str(), diagnostics.size() + 1);
        *output = blob.Detach();
    }

    inline void AddArgument(std::vector<std::wstring>& storage, const wchar_t* value)
    {
        storage.emplace_back(value);
    }

    inline void AddArgument(std::vector<std::wstring>& storage, const std::wstring& value)
    {
        storage.emplace_back(value);
    }
}

inline bool DxcRuntimeCompilerIsAvailable()
{
    return SUCCEEDED(RenderingDxcDetail::GetCompilerState().Status);
}

inline HRESULT WINAPI DxcCompileFromFileCompat(
    LPCWSTR fileName,
    const D3D_SHADER_MACRO* defines,
    ID3DInclude* includeHandler,
    LPCSTR entryPoint,
    LPCSTR target,
    UINT flags1,
    UINT /*flags2*/,
    ID3DBlob** code,
    ID3DBlob** errorMessages)
{
    using namespace RenderingDxcDetail;

    if (code)
    {
        *code = nullptr;
    }
    if (errorMessages)
    {
        *errorMessages = nullptr;
    }
    if (!fileName || !entryPoint || !target || !code)
    {
        SetDiagnosticBlob("DXC: invalid compile arguments.\n", errorMessages);
        return E_INVALIDARG;
    }

    if (includeHandler && includeHandler != D3D_COMPILE_STANDARD_FILE_INCLUDE)
    {
        SetDiagnosticBlob(
            "DXC: custom ID3DInclude handlers are not supported by the compatibility adapter.\n",
            errorMessages);
        return E_NOTIMPL;
    }

    CompilerState& state = GetCompilerState();
    if (FAILED(state.Status) || !state.Utils || !state.Compiler || !state.IncludeHandler)
    {
        SetDiagnosticBlob("DXC: failed to initialize IDxcUtils/IDxcCompiler3.\n", errorMessages);
        return FAILED(state.Status) ? state.Status : E_FAIL;
    }

    ComPtr<IDxcBlobEncoding> source;
    HRESULT result = state.Utils->LoadFile(fileName, nullptr, &source);
    if (FAILED(result) || !source)
    {
        SetDiagnosticBlob("DXC: failed to load the HLSL source file.\n", errorMessages);
        return FAILED(result) ? result : E_FAIL;
    }

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = source->GetBufferPointer();
    sourceBuffer.Size = source->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    BOOL encodingKnown = FALSE;
    UINT32 codePage = DXC_CP_UTF8;
    if (SUCCEEDED(source->GetEncoding(&encodingKnown, &codePage)) && encodingKnown)
    {
        sourceBuffer.Encoding = codePage;
    }

    const std::wstring wideEntryPoint = ToWide(entryPoint);
    const std::wstring wideTarget = PromoteLegacyProfile(target);

    std::vector<std::wstring> argumentStorage;
    argumentStorage.reserve(32);
    AddArgument(argumentStorage, fileName);
    AddArgument(argumentStorage, L"-E");
    AddArgument(argumentStorage, wideEntryPoint);
    AddArgument(argumentStorage, L"-T");
    AddArgument(argumentStorage, wideTarget);
    AddArgument(argumentStorage, L"-HV");
    AddArgument(argumentStorage, L"2021");

    const std::filesystem::path sourcePath(fileName);
    if (!sourcePath.parent_path().empty())
    {
        AddArgument(argumentStorage, L"-I");
        AddArgument(argumentStorage, sourcePath.parent_path().wstring());
    }

    AddArgument(
        argumentStorage,
        (flags1 & D3DCOMPILE_PACK_MATRIX_ROW_MAJOR) != 0 ? L"-Zpr" : L"-Zpc");

    if ((flags1 & D3DCOMPILE_DEBUG) != 0)
    {
        AddArgument(argumentStorage, L"-Zi");
        AddArgument(argumentStorage, L"-Qembed_debug");
    }

    if ((flags1 & D3DCOMPILE_SKIP_OPTIMIZATION) != 0)
    {
        AddArgument(argumentStorage, L"-Od");
    }
    else
    {
        const UINT optimizationLevel = flags1 & D3DCOMPILE_OPTIMIZATION_LEVEL3;
        if (optimizationLevel == D3DCOMPILE_OPTIMIZATION_LEVEL0)
        {
            AddArgument(argumentStorage, L"-O0");
        }
        else if (optimizationLevel == D3DCOMPILE_OPTIMIZATION_LEVEL2)
        {
            AddArgument(argumentStorage, L"-O2");
        }
        else if (optimizationLevel == D3DCOMPILE_OPTIMIZATION_LEVEL3)
        {
            AddArgument(argumentStorage, L"-O3");
        }
        else
        {
            AddArgument(argumentStorage, L"-O1");
        }
    }

    if ((flags1 & D3DCOMPILE_WARNINGS_ARE_ERRORS) != 0)
    {
        AddArgument(argumentStorage, L"-WX");
    }
    if ((flags1 & D3DCOMPILE_ENABLE_STRICTNESS) != 0)
    {
        AddArgument(argumentStorage, L"-Ges");
    }
    if ((flags1 & D3DCOMPILE_IEEE_STRICTNESS) != 0)
    {
        AddArgument(argumentStorage, L"-Gis");
    }
    if ((flags1 & D3DCOMPILE_ALL_RESOURCES_BOUND) != 0)
    {
        AddArgument(argumentStorage, L"-all_resources_bound");
    }
    if ((flags1 & D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES) != 0)
    {
        AddArgument(argumentStorage, L"-enable_unbounded_descriptor_tables");
    }

    if (defines)
    {
        for (const D3D_SHADER_MACRO* define = defines; define->Name; ++define)
        {
            std::wstring definition = ToWide(define->Name);
            if (define->Definition && define->Definition[0] != '\0')
            {
                definition += L"=";
                definition += ToWide(define->Definition);
            }
            AddArgument(argumentStorage, L"-D");
            AddArgument(argumentStorage, definition);
        }
    }

    std::vector<LPCWSTR> arguments;
    arguments.reserve(argumentStorage.size());
    for (const std::wstring& argument : argumentStorage)
    {
        arguments.push_back(argument.c_str());
    }

    ComPtr<IDxcResult> compileResult;
    result = state.Compiler->Compile(
        &sourceBuffer,
        arguments.data(),
        static_cast<UINT32>(arguments.size()),
        state.IncludeHandler.Get(),
        IID_PPV_ARGS(&compileResult));
    if (FAILED(result) || !compileResult)
    {
        SetDiagnosticBlob("DXC: IDxcCompiler3::Compile failed.\n", errorMessages);
        return FAILED(result) ? result : E_FAIL;
    }

    std::string diagnostics;
    ComPtr<IDxcBlobUtf8> diagnosticsBlob;
    if (SUCCEEDED(compileResult->GetOutput(
        DXC_OUT_ERRORS,
        IID_PPV_ARGS(&diagnosticsBlob),
        nullptr)) &&
        diagnosticsBlob && diagnosticsBlob->GetStringLength() > 0)
    {
        diagnostics.assign(
            diagnosticsBlob->GetStringPointer(),
            diagnosticsBlob->GetStringLength());
    }
    SetDiagnosticBlob(diagnostics, errorMessages);

    HRESULT compileStatus = E_FAIL;
    result = compileResult->GetStatus(&compileStatus);
    if (FAILED(result))
    {
        return result;
    }
    if (FAILED(compileStatus))
    {
        return compileStatus;
    }

    ComPtr<IDxcBlob> object;
    result = compileResult->GetOutput(
        DXC_OUT_OBJECT,
        IID_PPV_ARGS(&object),
        nullptr);
    if (FAILED(result) || !object)
    {
        if (diagnostics.empty())
        {
            SetDiagnosticBlob("DXC: compilation succeeded but no DXIL object was produced.\n", errorMessages);
        }
        return FAILED(result) ? result : E_FAIL;
    }

    return CreateLegacyBlob(object->GetBufferPointer(), object->GetBufferSize(), code);
}
