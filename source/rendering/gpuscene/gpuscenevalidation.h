#pragma once
#include <cstdint>
namespace GpuSceneValidation
{
void BeforeFrame(uint64_t frame);
void AfterFrame(uint64_t frame);
void BeforeScene();
bool UseLegacyCulling();
void RecordModelCull(bool resident);
uint32_t Failures();
} // namespace GpuSceneValidation
