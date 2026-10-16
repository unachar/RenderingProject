#pragma once
#include <cstdint>
class GPUDrivenValidation
{
public:
    static bool Enabled();
    static bool IsGpuValidationRun();
    static void BeforeFrame(uint64_t frame);
    static void AfterFrame(uint64_t frame);
    static bool Finish();
    static void Trace(const char* stage);
};

