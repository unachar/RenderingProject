#pragma once
#include <cstdint>

namespace TemporalLightingValidation
{
bool Enabled();
void BeforeFrame(uint64_t frame);
void AfterFrame(uint64_t frame);
bool Finish();
} // namespace TemporalLightingValidation
