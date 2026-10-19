#pragma once
#include "virtualgeometryasset.h"
#include <span>

namespace VirtualGeometrySelection
{
    struct View
    {
        std::array<float, 3> Camera{};   // オブジェクト空間のカメラ。
        float WorldScale = 1;            // オブジェクト変換の最大スケールの保守的な上限。
        float MinimumWorldScale = 1;     // 最小特異値の保守的な下限。
        float ProjectionScalePixels = 1; // 0.5 × ビューポートの高さ × abs(projection._22)。
        // 平行光源の影ビューでは、距離減衰なしでテクセル単位の誤差を測る。
        bool Orthographic = false;
    };

    struct Settings
    {
        float TargetErrorPixels = 1;
        int ForceHierarchyLevel = -1;
    };

    struct Result
    {
        std::vector<uint32_t> Clusters;
        std::string Error;
    };

    // 検証用の CPU 参照実装。実行時の選択は GPU で行う。
    Result Select(const VirtualGeometryAsset& asset, const View& view, const Settings& settings);

    struct ResidentResult : Result
    {
        std::vector<uint32_t> RequestedPages;
    };

    struct TemporalResult : ResidentResult
    {
        std::vector<uint8_t> DesiredRefine;
    };

    TemporalResult SelectResidentTemporal(const VirtualGeometryAsset& asset, const View& view, const Settings& settings,
                                          std::span<const uint32_t> clusterPages,
                                          std::span<const uint8_t> residentPages,
                                          std::span<const uint8_t> previousRefine, bool historyValid, float fraction);
    ResidentResult SelectResident(const VirtualGeometryAsset& asset, const View& view, const Settings& settings,
                                  std::span<const uint32_t> clusterPages, std::span<const uint8_t> residentPages);
} // 名前空間 VirtualGeometrySelection の終わり。
