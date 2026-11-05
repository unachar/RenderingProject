#pragma once
#include "materialgraph.h"
#include <unordered_map>
#include <functional>

struct MaterialGraphSourceLocation
{
    uint64_t Node = 0;
    std::string File;
    size_t Line = 0;
};

struct MaterialGraphProgram
{
    bool Success = false;
    bool Animated = false;
    bool MaterialOutput = false;
    std::string Hlsl;
    std::string Error;
    uint64_t ShaderHash = 0;
    std::vector<std::array<float, 4>> Parameters;
    std::vector<uint64_t> ParameterNodes;
    std::vector<AssetHandle> Textures;
    std::vector<MaterialSamplerType> TextureSamplers;
    std::vector<MaterialGraphSourceLocation> Sources;
    std::vector<uint64_t> DependencyOrder;
};

class MaterialGraphCompiler
{
  public:
    // targetNode==0 compiles Material Output; preview compilation visits only the chosen node's inputs.
    static MaterialGraphProgram Generate(const MaterialGraph& graph, uint64_t targetNode = 0);
    // Production shaders supply parameter/texture descriptors through the material texture heap.
    static MaterialGraphProgram GenerateRuntime(const MaterialGraph& graph, uint64_t targetNode = 0);
};

// ============================================================================
// 描画時評価（GPU）用のマテリアルグラフ プログラム
//  ・グラフを小さな命令列にして、エンジンのシェーダー（MaterialGraphVM.hlsli）が画素ごとに解釈する
//  ・命令列は R32G32B32A32_FLOAT の 1 行テクスチャ（マテリアルのテクスチャ表）に置くので、
//    フォワード / ディファード / GPU 駆動 / Visibility 解決のどの経路でも同じプログラムが動く
//  ・パラメーターは命令列に埋め込む（値の変更でシェーダーの再コンパイルは不要）
// レイアウト（1 テクセル = float4）：
//   [0] 時刻, 命令の開始テクセル, 全テクセル数, フラグ（bit0 = Time を使う、bit1 = ソリッド座標がワールド空間）
//   [1] 出力レジスタ：Base Color, Metallic, Roughness, Normal（-1 = 未接続）
//   [2] 出力レジスタ：Emissive, Opacity, Opacity Mask, AO
//   [3] 出力レジスタ：Height, Normal Intensity, Transmission, -1
//   [3...] 命令：(opcode, 出力レジスタ, 命令のテクセル数, 引数テクセル数) + 引数（レジスタ番号 4 個ずつ）+ 即値
// ============================================================================
namespace MaterialGraphVm
{
    inline constexpr int kMaxTexels = 1024;  // プログラムテクスチャの幅
    inline constexpr int kMaxRegisters = 24; // MaterialGraphVM.hlsli の MG_VM_REGS と一致させる
    inline constexpr int kHeaderTexels = 4;
    inline constexpr int kOutputCount = 12;

    // テクスチャ（AssetHandle + サンプラー）→ マテリアルのテクスチャ表の番号（0〜511、失敗は -1）
    using TextureResolver = std::function<int(AssetHandle texture, MaterialSamplerType sampler)>;

    struct Program
    {
        bool Success = false;
        std::string Error;
        std::vector<std::array<float, 4>> Texels;
        bool UsesTime = false;
        bool UsesView = false;        // カメラの位置 / 距離 / 深度（Camera Distance・Distance Fade など）を使う
        bool WorldSolidSpace = false; // ソリッド / ノイズの既定座標をワールド空間にする（既定はオブジェクト空間）
        int TargetDims = 4;           // TargetNode を指定したとき：ノード出力の成分数（1〜4）
        int Instructions = 0;
        int Registers = 0;
        uint64_t Hash = 0;
    };

    // 時刻のテクセル（[0]）を作る
    std::array<float, 4> HeaderTexel(const Program& program, float time);
    struct Options
    {
        uint64_t TargetNode = 0;      // 0 = Material Output。それ以外はそのノードの出力を Base Color（Out0.x）に入れる
        bool IncludeDefaults = false; // 未接続の Material Output のピンも既定値で出力する（ベイク用）
        bool EmissiveOnly = false;    // 動的発光用：Emissive とその入力だけを評価する
    };
    Program Compile(const MaterialGraph& graph, const TextureResolver& resolveTexture);
    Program Compile(const MaterialGraph& graph, const TextureResolver& resolveTexture, const Options& options);
} // namespace MaterialGraphVm


