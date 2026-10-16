#pragma once
#include <cstdint>

class RenderingValidation
{
public:
    static bool IsEnabled();
    static void BeforeFrame(uint64_t frame);
    static void AfterFrame(uint64_t frame);
    static bool Finish();
};

