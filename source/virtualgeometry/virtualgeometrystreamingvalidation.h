#pragma once
#include <cstdint>

namespace VirtualGeometryStreamingValidation
{
    bool Enabled();
    void BeforeFrame(uint64_t frame);
    void AfterFrame(uint64_t frame);
    bool Finish();
} // 名前空間 VirtualGeometryStreamingValidation の終わり。
