#pragma once
#include <cstdint>

namespace VirtualGeometryValidation
{
bool Enabled();
void BeforeFrame(uint64_t frame);
void AfterFrame(uint64_t frame);
bool Finish();
} // namespace VirtualGeometryValidation
