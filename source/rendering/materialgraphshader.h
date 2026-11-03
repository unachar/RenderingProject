#pragma once
#include "materialgraphcompiler.h"
#include <d3dcompiler.h>
#include <wrl.h>

struct MaterialGraphShader
{
    Microsoft::WRL::ComPtr<ID3DBlob> Vertex;
    Microsoft::WRL::ComPtr<ID3DBlob> Pixel;
    std::string Error;
    uint64_t Hash = 0;

    bool Success() const
    {
        return Vertex && Pixel;
    }
};

class MaterialGraphShaderCompiler
{
  public:
    // CPU-only: safe to run on a shader worker. Uses the engine's thread-local DXC and DXIL cache.
    static MaterialGraphShader Compile(const MaterialGraphProgram& program);
};

// ============================================================================
// MaterialGraphGpu：マテリアルグラフを GPU で評価する（エディター用）
//  ・ノードプレビュー / Material Output のプレビュー（GPU のプレビュー表に描き、ImGui で表示）
//  ・テクスチャへのベイク、Convert To Texture / Solid の「作成」（GPU で評価 → 読み戻し → PNG 保存）
//  ・命令列は MaterialGraphVm::Compile（エンジンの描画時評価と同じ）。シェーダーは MaterialGraphGpuCS.hlsl
//  ・記録は RenderFrame::EndDraw（Execute）。対応外のノードを含むときは呼び出し側が CPU 評価に戻す
// ============================================================================
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace MaterialGraphGpu
{
    using Texels = std::vector<std::array<float, 4>>;

    enum class Mode : uint32_t
    {
        NodePlane = 0,
        NodeSphere = 1,
        OutputSphere = 2,
        Bake = 3,
        BakeNode = 4,
        DynamicEmission = 5,
    };

    // ---- プレビュー（UI スレッド）
    struct PreviewRequest
    {
        const void* Owner = nullptr; // グラフ
        uint64_t Node = 0;
        const Texels* Program = nullptr;
        uint64_t ProgramHash = 0;
        Mode PreviewMode = Mode::NodePlane;
        uint32_t Flags = 0;      // bit0 色, bit1 法線, bit4-6 成分数
        uint32_t OutputPins = 0; // Output：bit0 Normal, bit1 Opacity Mask, bit2 Transmission
        float Clip = 0.3333f;
        int Resolution = 64;
        bool Animated = false;
        float Time = 0.0f;
    };
    struct PreviewImage
    {
        uint64_t TextureId = 0; // ImTextureID（GPU ディスクリプタ）
        float U0 = 0, V0 = 0, U1 = 0, V1 = 0;
        bool Ready = false; // 1 回以上描いた
    };
    // このフレームで表示するプレビューを登録する（内容が変わった / 動くときだけ GPU で描き直す）
    bool RequestPreview(const PreviewRequest& request, PreviewImage& image);
    void ForgetOwner(const void* owner);

    // ---- ベイク（UI スレッドで Submit、ワーカースレッドで Wait）
    struct Job;
    struct BakeDesc
    {
        Texels Program;
        Mode JobMode = Mode::Bake;
        int Resolution = 512;
        float Time = 0.0f;
        uint32_t Flags = 0;   // Bake：bit0-1 アルファ, bit2 旧グラフの色の法線 / BakeNode：1 = 法線
        uint32_t SlotMask = 0; // Bake：焼くスロット / BakeNode：bit4-6 成分数
    };
    std::shared_ptr<Job> Submit(BakeDesc desc);
    // 完了まで待ち、スロットごとの値（線形 RGBA）を返す。失敗 / タイムアウトは false（CPU 評価に戻す）
    bool Wait(const std::shared_ptr<Job>& job, double timeoutSeconds, std::array<Texels, 6>& slices);

    // ---- 発光の動的ベイク（GI / 反射 / Lumen 用）
    //  カメラ距離・Time などで変わる Emissive を、オブジェクト（エンティティ + スロット）ごとに RGBA16F へ焼く。
    //  毎フレーム要求し、入力（プログラム / カメラ / 時刻）が変わったときだけ GPU で焼き直す。
    //  Execute（フレームの最後）で記録するので、結果は次のフレームの GI から使われる（1 フレーム遅れ）。
    //  テクスチャは焼いていない間 PIXEL_SHADER_RESOURCE（RayReflections の発光テクスチャと同じ前提）
    struct DynamicEmissionRequest
    {
        uint64_t Key = 0;              // (エンティティ << 32) | スロット
        const Texels* Program = nullptr;
        uint64_t ProgramHash = 0;
        bool UsesTime = false;
        bool UsesView = false;
        float Time = 0.0f;
        std::array<float, 3> Camera{}; // 実カメラのワールド座標
        float Distance = 0.0f;         // カメラ → オブジェクトの距離
        float Depth = 0.0f;            // オブジェクトのビュー深度
    };
    struct DynamicEmissionTexture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Texture;
        uint64_t Revision = 0; // 焼き直すたびに増える（GI / Lumen のキャッシュ判定用）
    };
    // 1 回以上焼いたテクスチャがあれば true（まだ無ければ false：呼び出し側は従来のベイク値を使う）
    bool RequestDynamicEmission(const DynamicEmissionRequest& request, DynamicEmissionTexture& result);

    // RenderFrame::EndDraw から呼ぶ（UI の後、ImGui の描画の前）
    void Execute(ID3D12GraphicsCommandList* commandList);
    bool IsAvailable();
    bool PrepareStartupAssets();
    const char* LastError();
    void Shutdown();
} // namespace MaterialGraphGpu


