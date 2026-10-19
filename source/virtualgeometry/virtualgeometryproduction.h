#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace VirtualGeometryProduction
{
    // 記録は明示的に有効化した場合だけ行い、シーンや描画モードは変更しない。
    bool BeginBenchmark(const std::filesystem::path& directory, std::string& error);
    void EndBenchmark();
    void RecordBenchmark();
    bool Recording();
    const std::string& LastError();
    // これらのフックは、明示的な検証フラグがある場合だけ既存状態を置き換えるテスト準備を行う。
    bool ValidationEnabled();
    void BeforeFrame(uint64_t frame);
    void AfterFrame(uint64_t frame);
    bool Finish();
} // 名前空間 VirtualGeometryProduction の終わり。
