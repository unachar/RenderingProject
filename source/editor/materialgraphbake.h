#pragma once
// ============================================================================
// MaterialGraphBake：マテリアルグラフのピクセルごとの式を、エンジンのマテリアルテクスチャへ焼く
//  ・エンジン（ラスター / GPU 駆動 / Visibility / レイトレーシング）はテクスチャ + 定数のマテリアルなので、
//    UV 演算 / Noise / Voronoi / Blend / Normal From Height などを含む全ノードをここで画像にする
//  ・Base Color(+Opacity/Mask) / Normal / Metallic Roughness(R=M,G=R) / AO / Emissive / Height を出力
//  ・ワーカースレッドで評価 → PNG を一時ファイルに書いて置き換える（エンジン側はファイル再読み込み）
//  ・出力先：<マテリアルのフォルダ>/<マテリアル名>_GraphBake/<マテリアル名>_<スロット>.png
// ============================================================================
#include "materialgraph.h"
#include "materialgraphevaluator.h"
#include "materialgraphpreview.h"
#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace MaterialGraphBake
{
    constexpr size_t kSlotCount = static_cast<size_t>(MaterialTextureSlot::Count);

    struct Request
    {
        MaterialGraph Graph;
        uint32_t Mask = 0; // 焼くスロット（bit = MaterialTextureSlot）
        MaterialGraphPreview::AlphaSource Alpha = MaterialGraphPreview::AlphaSource::BaseColor;
        int Resolution = 512;
        float Time = 0.0f;
        uint64_t Signature = 0;
        std::filesystem::path Directory; // 出力フォルダ（asset フォルダ内）
        std::string Stem;                // ファイル名の先頭（マテリアル名）
        std::unordered_map<AssetHandle, std::filesystem::path> TexturePaths; // グラフが参照するテクスチャ
        int Threads = 0;                 // 0 = 自動（論理コア数の半分）
    };

    struct Result
    {
        bool Success = false;
        std::string Error;
        std::vector<std::string> Warnings;
        uint64_t Signature = 0;
        uint32_t Mask = 0;
        std::array<std::filesystem::path, kSlotCount> Files;
        float EmissivePeak = 1.0f;
        double Seconds = 0.0;
        int Resolution = 0;
    };

    // 評価結果（線形値）。スロットごとに RGBA（使わない成分は 0 / 1）
    struct Images
    {
        int Resolution = 0;
        uint32_t Mask = 0;
        std::array<std::vector<std::array<float, 4>>, kSlotCount> Pixels;
        float EmissivePeak = 1.0f;
    };

    // CPU 評価のみ（D3D / ファイル I/O なし）。threads <= 0 なら自動
    bool Evaluate(const Request& request, const MaterialGraphEval::TextureSampler& sampler, Images& images,
                  std::string& error);

    // ワーカースレッドで呼ぶ：テクスチャ読み込み → 評価 → PNG 書き出し
    Result Run(const Request& request);

    const char* SlotName(MaterialTextureSlot slot);
} // namespace MaterialGraphBake
