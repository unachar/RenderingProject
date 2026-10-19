#include <windows.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl.h>
#include <string>
#include <vector>
using namespace std;
#include "dxccompiler.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <stdexcept>

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            throw runtime_error(message);
        }
    }

    void Write(const filesystem::path& path, const char* text)
    {
        ofstream(path) << text;
    }

    size_t CacheEntries()
    {
        size_t count = 0;
        error_code error;
        for (const auto& entry : filesystem::directory_iterator("EditorCache/Shaders", error))
        {
            if (entry.path().extension() == ".bin")
            {
                ++count;
            }
        }
        return count;
    }

    ComPtr<ID3DBlob> Compile(const char* entry = "main", const D3D_SHADER_MACRO* defines = nullptr,
                             UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3)
    {
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        const HRESULT result = DxcCompileFromFileCompat(L"test.hlsl", defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry,
                                                        "cs_6_0", flags, 0, &code, &errors);
        if (FAILED(result) && errors)
        {
            cerr << static_cast<const char*>(errors->GetBufferPointer());
        }
        Check(SUCCEEDED(result) && code, "shader compilation failed");
        return code;
    }
} // 無名名前空間

int main(int argc, char** argv)
{
    try
    {
        if (argc == 3 && string(argv[1]) == "--reuse")
        {
            filesystem::current_path(argv[2]);
            Compile();
            return 0;
        }
        const auto directory =
            filesystem::absolute("bin/validation/shader-cache-tests") / to_string(GetCurrentProcessId());
        filesystem::create_directories(directory);
        filesystem::current_path(directory);
        Write("value.hlsli", "#define VALUE 1\n");
        Write("nested.hlsli", "#include \"value.hlsli\"\n");
        Write("test.hlsl", "#include \"nested.hlsli\"\n#ifndef SCALE\n#define SCALE 1\n#endif\n"
                           "RWStructuredBuffer<uint> output : register(u0);\n"
                           "[numthreads(1,1,1)] void main() { output[0] = VALUE * SCALE; }\n"
                           "[numthreads(1,1,1)] void alternate() { output[0] = VALUE + SCALE; }\n");
        auto cold = Compile();
        Check(CacheEntries() == 1, "cold compile must persist one shader cache entry");
        const auto cacheFile = filesystem::directory_iterator("EditorCache/Shaders")->path();
        const auto savedTime = filesystem::last_write_time(cacheFile) - chrono::hours(24);
        filesystem::last_write_time(cacheFile, savedTime);
        auto warm = Compile();
        Check(CacheEntries() == 1, "identical compilation must reuse its cache entry");
        Check(filesystem::last_write_time(cacheFile) == savedTime, "cache hit must skip compilation and rewriting");
        Check(cold->GetBufferSize() == warm->GetBufferSize() &&
                  memcmp(cold->GetBufferPointer(), warm->GetBufferPointer(), cold->GetBufferSize()) == 0,
              "cached DXIL must match compiled DXIL");

        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        wstring command = L"\"" + wstring(executable) + L"\" --reuse \"" + directory.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        Check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &startup, &process),
              "second cache reader process must start");
        const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
        if (wait != WAIT_OBJECT_0)
        {
            TerminateProcess(process.hProcess, 1);
        }
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        Check(wait == WAIT_OBJECT_0 && exitCode == 0, "second process must compile successfully");
        Check(filesystem::last_write_time(cacheFile) == savedTime, "second process must reuse persistent DXIL");

        Write("value.hlsli", "#define VALUE 2\n");
        Compile();
        Check(CacheEntries() == 2, "nested include edits must invalidate cache");
        const D3D_SHADER_MACRO defines[] = {{"SCALE", "3"}, {nullptr, nullptr}};
        Compile("main", defines);
        Compile("alternate");
        Compile("main", nullptr, D3DCOMPILE_SKIP_OPTIMIZATION);
        Check(CacheEntries() == 5, "defines, entry point and compiler flags need distinct entries");

        for (const auto& entry : filesystem::directory_iterator("EditorCache/Shaders"))
        {
            Write(entry.path(), "damaged cache");
        }
        Compile();
        Check(CacheEntries() == 5, "corrupt cache must be replaced through normal compilation");

        // キャッシュへの書き込みは必須ではないが、描画を成功させるにはDXILが必要になる。
        filesystem::create_directory("cache-unavailable");
        filesystem::copy_file("test.hlsl", "cache-unavailable/test.hlsl");
        filesystem::copy_file("nested.hlsli", "cache-unavailable/nested.hlsli");
        filesystem::copy_file("value.hlsli", "cache-unavailable/value.hlsli");
        filesystem::current_path("cache-unavailable");
        Write("EditorCache", "file blocks cache directory creation");
        Compile();
        filesystem::current_path("..");

        filesystem::remove("value.hlsli");
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        const HRESULT missing = DxcCompileFromFileCompat(L"test.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                         "main", "cs_6_0", 0, 0, &code, &errors);
        Check(FAILED(missing) && !code && errors, "missing include must fail rather than reuse stale DXIL");
        cout << "PASS: persistent shader cache, nested includes, variants, corruption and missing dependencies\n";
        return 0;
    }
    catch (const exception& error)
    {
        cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
