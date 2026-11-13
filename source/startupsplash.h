#pragma once

#include <Windows.h>
#include <atomic>
#include <thread>

class StartupSplash
{
public:
    explicit StartupSplash(HINSTANCE instance, bool enabled);
    ~StartupSplash();

    void SetProgress(int percent);
    void Complete();
    void Close();

private:
    void Run(HINSTANCE instance, HANDLE readyEvent);

    HANDLE stopEvent_ = nullptr;
    std::thread thread_;
    std::atomic<int> percent_{0};
};
