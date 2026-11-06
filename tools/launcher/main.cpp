#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int showCommand)
{
    std::vector<wchar_t> modulePath(32768);
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size())
    {
        return ERROR_BAD_PATHNAME;
    }

    const std::wstring path(modulePath.data(), length);
    const std::wstring directory = path.substr(0, path.find_last_of(L"\\/"));
    const std::wstring executable = directory + L"\\runtime\\DirectX12.exe";
    std::wstring commandLine = L"\"" + executable + L"\"";
    if (arguments != nullptr && arguments[0] != L'\0')
    {
        commandLine += L" ";
        commandLine += arguments;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = static_cast<WORD>(showCommand);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(),
                        &startup, &process))
    {
        const DWORD error = GetLastError();
        const std::wstring message =
            L"Could not start:\n" + executable + L"\n\nWindows error: " + std::to_wstring(error);
        MessageBoxW(nullptr, message.c_str(), L"DirectX12", MB_OK | MB_ICONERROR);
        return static_cast<int>(error);
    }

    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = ERROR_PROCESS_ABORTED;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}
