#pragma once
#include <cstdint>

namespace GiValidation
{
    bool Enabled();
    void BeforeFrame(uint64_t frame);
    void AfterFrame(uint64_t frame);
    bool Finish();
} // 名前空間 GiValidation の終わり。
