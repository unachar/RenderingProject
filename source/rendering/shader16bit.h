#pragma once

// ネイティブ 16bit シェーダー演算（DXC -enable-16bit-types / SM6.2+）でのコンパイル。
// - GPU が Native16BitShaderOps に対応し、DXC が利用できる場合だけ 16bit 型を有効にする。
// - それ以外は従来の D3DCompileFromFile（DXC 互換ラッパー）で RP_NATIVE_16BIT=0 としてコンパイルし、
//   Half16.hlsli の hfloat 系の型は float に戻る（結果は従来と同じ）。
// - 環境変数 DX12_DISABLE_16BIT_SHADERS=1 で強制的に無効化できる。
// RTX 30 系（GA10x）の FP16 スループットは FP32 と同等のため、主な効果はレジスタ使用量の削減による
// 占有率の向上と、中間値の帯域削減。

#include "pch.h"
#include <dxcapi.h>
#include "dxcshadercache.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include <filesystem>
#include <string>
#include <vector>

namespace Shader16Bit
{
    inline bool DisabledByEnvironment()
    {
        static const bool disabled = [] {
            char value[8]{};
            return GetEnvironmentVariableA("DX12_DISABLE_16BIT_SHADERS", value, sizeof(value)) > 0 && atoi(value) != 0;
        }();
        return disabled;
    }

    inline bool IsAvailable()
    {
        return !DisabledByEnvironment() && GraphicsDevice::GetFeatureCapabilities().Native16BitShaderOps;
    }

    inline std::wstring Widen(const char* text)
    {
        if (!text || !*text)
        {
            return {};
        }
        const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        std::wstring result(static_cast<size_t>((std::max)(length, 1)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length);
        result.resize(wcslen(result.c_str()));
        return result;
    }

    inline HRESULT CreateBlob(const void* data, size_t size, ID3DBlob** blob)
    {
        if (!blob)
        {
            return E_POINTER;
        }
        *blob = nullptr;
        ComPtr<ID3DBlob> created;
        const HRESULT result = D3DCreateBlob(size, &created);
        if (FAILED(result))
        {
            return result;
        }
        if (size)
        {
            memcpy(created->GetBufferPointer(), data, size);
        }
        *blob = created.Detach();
        return S_OK;
    }

    using DxcCreateInstanceFunction = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

    inline DxcCreateInstanceFunction DxcFactory()
    {
        static const DxcCreateInstanceFunction factory = []() -> DxcCreateInstanceFunction {
            HMODULE module = GetModuleHandleW(L"dxcompiler.dll");
            if (!module)
            {
                module = LoadLibraryW(L"dxcompiler.dll");
            }
            return module ? reinterpret_cast<DxcCreateInstanceFunction>(GetProcAddress(module, "DxcCreateInstance"))
                          : nullptr;
        }();
        return factory;
    }

    // ターゲットのシェーダーモデルを 6.2 以上へ引き上げる（"cs_6_0" → "cs_6_2"）。
    inline std::string PromoteTarget(const char* target)
    {
        std::string promoted = target ? target : "";
        if (promoted.size() >= 6 && promoted[promoted.size() - 3] == '6' && promoted[promoted.size() - 2] == '_' &&
            promoted.back() >= '0' && promoted.back() < '2')
        {
            promoted.back() = '2';
        }
        return promoted;
    }

    inline HRESULT CompileNative(const wchar_t* path, const D3D_SHADER_MACRO* defines, const char* entry,
                                 const char* target, UINT flags, ID3DBlob** code, ID3DBlob** errors)
    {
        const auto factory = DxcFactory();
        if (!factory)
        {
            return E_NOINTERFACE;
        }
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        ComPtr<IDxcIncludeHandler> includes;
        if (FAILED(factory(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
            FAILED(factory(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) ||
            FAILED(utils->CreateDefaultIncludeHandler(&includes)))
        {
            return E_NOINTERFACE;
        }
        ComPtr<IDxcBlobEncoding> source;
        HRESULT result = utils->LoadFile(path, nullptr, &source);
        if (FAILED(result))
        {
            return result;
        }
        const DxcBuffer buffer{source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP};

        const std::wstring entryName = Widen(entry);
        const std::wstring targetName = Widen(PromoteTarget(target).c_str());
        const std::wstring includeDirectory =
            std::filesystem::path(path).parent_path().lexically_normal().wstring();
        std::vector<std::wstring> storage;
        storage.reserve(16);
        storage.push_back(L"RP_NATIVE_16BIT=1");
        for (const D3D_SHADER_MACRO* macro = defines; macro && macro->Name; ++macro)
        {
            storage.push_back(Widen(macro->Name) + L"=" + (macro->Definition ? Widen(macro->Definition) : L"1"));
        }
        std::vector<LPCWSTR> arguments = {path, L"-E", entryName.c_str(), L"-T", targetName.c_str(),
                                          L"-enable-16bit-types"};
        if (!includeDirectory.empty())
        {
            arguments.push_back(L"-I");
            arguments.push_back(includeDirectory.c_str());
        }
        if (flags & D3DCOMPILE_SKIP_OPTIMIZATION)
        {
            arguments.push_back(L"-Od");
        }
        else
        {
            arguments.push_back(L"-O3");
        }
        if (flags & D3DCOMPILE_DEBUG)
        {
            arguments.push_back(L"-Zi");
            arguments.push_back(L"-Qembed_debug");
        }
        for (const auto& define : storage)
        {
            arguments.push_back(L"-D");
            arguments.push_back(define.c_str());
        }

        const auto cachePath = DxcShaderCache::CachePath(compiler.Get(), includes.Get(), buffer, arguments);
        std::vector<char> cachedObject;
        std::string cachedDiagnostics;
        if (DxcShaderCache::Load(cachePath, cachedObject, cachedDiagnostics))
        {
            if (errors && !cachedDiagnostics.empty())
            {
                CreateBlob(cachedDiagnostics.c_str(), cachedDiagnostics.size() + 1, errors);
            }
            return CreateBlob(cachedObject.data(), cachedObject.size(), code);
        }

        ComPtr<IDxcResult> compiled;
        result = compiler->Compile(&buffer, arguments.data(), static_cast<UINT32>(arguments.size()), includes.Get(),
                                   IID_PPV_ARGS(&compiled));
        if (FAILED(result) || !compiled)
        {
            return FAILED(result) ? result : E_FAIL;
        }
        std::string diagnostics;
        ComPtr<IDxcBlobUtf8> diagnosticBlob;
        if (SUCCEEDED(compiled->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&diagnosticBlob), nullptr)) && diagnosticBlob &&
            diagnosticBlob->GetStringLength() > 0)
        {
            diagnostics.assign(diagnosticBlob->GetStringPointer(), diagnosticBlob->GetStringLength());
            if (errors)
            {
                CreateBlob(diagnostics.c_str(), diagnostics.size() + 1, errors);
            }
        }
        HRESULT status = E_FAIL;
        if (FAILED(compiled->GetStatus(&status)) || FAILED(status))
        {
            return FAILED(status) ? status : E_FAIL;
        }
        ComPtr<IDxcBlob> object;
        if (FAILED(compiled->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object ||
            object->GetBufferSize() == 0)
        {
            return E_FAIL;
        }
        DxcShaderCache::Store(cachePath, object->GetBufferPointer(), object->GetBufferSize(), diagnostics);
        return CreateBlob(object->GetBufferPointer(), object->GetBufferSize(), code);
    }

    // 16bit 対応ならネイティブ 16bit でコンパイルし、失敗・非対応時は従来経路へ退避する。
    // usedNative には実際に 16bit 型が有効になったかを返す。
    inline HRESULT CompileFromFile(const wchar_t* path, const D3D_SHADER_MACRO* defines, const char* entry,
                                   const char* target, UINT flags, ID3DBlob** code, ID3DBlob** errors,
                                   bool* usedNative = nullptr)
    {
        if (usedNative)
        {
            *usedNative = false;
        }
        if (code)
        {
            *code = nullptr;
        }
        if (errors)
        {
            *errors = nullptr;
        }
        if (IsAvailable())
        {
            ComPtr<ID3DBlob> nativeErrors;
            const HRESULT native = CompileNative(path, defines, entry, target, flags, code, &nativeErrors);
            if (SUCCEEDED(native) && code && *code)
            {
                if (usedNative)
                {
                    *usedNative = true;
                }
                if (errors)
                {
                    *errors = nativeErrors.Detach();
                }
                return native;
            }
            WriteGraphicsLog("16bit shader: native compile failed (%ls:%s), using 32bit path: %s\n", path,
                             entry ? entry : "",
                             nativeErrors ? static_cast<const char*>(nativeErrors->GetBufferPointer()) : "");
            if (code && *code)
            {
                (*code)->Release();
                *code = nullptr;
            }
        }
        std::vector<D3D_SHADER_MACRO> fallback;
        for (const D3D_SHADER_MACRO* macro = defines; macro && macro->Name; ++macro)
        {
            fallback.push_back(*macro);
        }
        fallback.push_back({"RP_NATIVE_16BIT", "0"});
        fallback.push_back({nullptr, nullptr});
        return D3DCompileFromFile(path, fallback.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0,
                                  code, errors);
    }
} // 名前空間 Shader16Bit の終わり。
