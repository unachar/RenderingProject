#include "pch.h"
#include "graphicslog.h"
#include <chrono>
#include <cstdlib>

void WriteGraphicsLog(const char* format, ...)
{
    FILE* fp = nullptr;
    fopen_s(&fp, "init_log.txt", "a");
    if (!fp)
    {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    static const auto start = std::chrono::steady_clock::now();
    static const bool startupTiming = [] {
        char value[8]{};
        return GetEnvironmentVariableA("DX12_STARTUP_TIMING", value, sizeof(value)) > 0 && atoi(value) != 0;
    }();
    if (startupTiming)
    {
        const double elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        fprintf(fp, "[startup %.3f ms] ", elapsed);
    }
    vfprintf(fp, format, arguments);
    va_end(arguments);
    fclose(fp);
}
