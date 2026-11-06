#pragma once
#include <filesystem>

class NiagaraWindow
{
  public:
    static void Open(const std::filesystem::path& path);
    static void Draw(bool* open);
    static bool HasFocus();
    static bool CanQuit();
    static void Shutdown();
};
