#pragma once
#include <cstdint>

class RenderProfiler;

namespace RenderBenchmark
{
bool IsEnabled();
void BeforeFrame(uint64_t frame);
void AfterFrame();
bool ShouldStop(uint64_t completedFrames);
bool Finish();
}
