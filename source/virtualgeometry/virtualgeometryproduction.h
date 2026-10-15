#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace VirtualGeometryProduction
{
// Recording is opt-in and never changes the scene or rendering mode.
bool BeginBenchmark(const std::filesystem::path& directory, std::string& error);
void EndBenchmark();
void RecordBenchmark();
bool Recording();
const std::string& LastError();
// These hooks only run destructive fixture setup when the explicit validation flag is set.
bool ValidationEnabled();
void BeforeFrame(uint64_t frame);
void AfterFrame(uint64_t frame);
bool Finish();
}
