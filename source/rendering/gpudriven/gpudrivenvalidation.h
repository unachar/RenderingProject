#pragma once
#include <cstdint>
class GPUDrivenValidation
{
public:
    static bool Enabled();
    static void BeforeFrame(uint64_t frame);
    static void AfterFrame(uint64_t frame);
    static bool Finish();
};
