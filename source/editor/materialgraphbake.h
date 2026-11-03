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
#include "materialgraphshader.h"
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
        float EmissiveScale = 0.0f;      // > 0：Emissive をこの値で正規化（ライブ更新はベイク時の最大値に合わせる）
        // GPU で評価する場合（MaterialGraphGpu::Submit 済み）。失敗したら CPU で評価し直す
        std::shared_ptr<MaterialGraphGpu::Job> Gpu;
    };

    struct Result
    {
        bool Success = false;
        bool Gpu = false; // GPU で評価した
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

    // ---- Convert To Texture：1 つのノードの入力を UV 0〜1 で焼いて PNG にする
    struct NodeRequest
    {
        MaterialGraph Graph;
        uint64_t Node = 0; // Convert To Texture ノード
        int Resolution = 512;
        float Time = 0.0f;
        MaterialSamplerType Sampler = MaterialSamplerType::Linear; // Color = sRGB で保存 / Normal = n*0.5+0.5 / Linear = そのまま
        std::filesystem::path File;
        std::unordered_map<AssetHandle, std::filesystem::path> TexturePaths;
        std::shared_ptr<MaterialGraphGpu::Job> Gpu; // GPU で評価する場合（失敗したら CPU）
    };
    struct NodeResult
    {
        bool Success = false;
        std::string Error;
        uint64_t Node = 0;
        std::filesystem::path File;
        double Seconds = 0.0;
    };
    NodeResult RunNode(const NodeRequest& request);

    // 時間（Time）に依存するスロット（bit = MaterialTextureSlot）
    uint32_t TimeDependentSlots(const MaterialGraph& graph, MaterialGraphPreview::AlphaSource alpha);
} // namespace MaterialGraphBake

// ============================================================================
// MaterialGraphLive：Time を使うマテリアルを実行中に動かす
//  ・ベイク済みテクスチャ（同じ SRV）へ、ワーカースレッドで評価した最新の時刻の画像を書き込み続ける
//  ・エディターで開いたマテリアルは Register、開いていないものはシーンのマテリアルから自動で見つける
// ============================================================================
namespace MaterialGraphLive
{
    // files：スロットごとのベイク済みテクスチャのパス（空 = 未ベイク）
    void Register(AssetHandle material, const MaterialGraph& graph,
                  const std::array<std::filesystem::path, MaterialGraphBake::kSlotCount>& files, float emissivePeak,
                  MaterialGraphPreview::AlphaSource alpha);
    void Unregister(AssetHandle material);
    // 毎フレーム（メインスレッド）
    void Update(double time);
    void Shutdown();
    // UI 用：更新中のスロット（0 = 動いていない）と直近の評価時間
    uint32_t ActiveSlots(AssetHandle material);
    double LastSeconds(AssetHandle material);
} // namespace MaterialGraphLive


