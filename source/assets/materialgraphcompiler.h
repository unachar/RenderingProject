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
    // ---- 2026-11 追加（旧プログラムとの互換：命令の開始位置は [0].y に入っているので、ヘッダーを伸ばしても古いシェーダーは動く）
    //   [4 .. 4 + kExtHeaderTexels)  追加の出力レジスタ（陰影ピン：ExtOutput。-1 = 未接続）
    //   [kFixedHeaderTexels ..)      動的テクセル（毎フレーム CPU が書く：Beat Phase / Audio Spectrum）
    inline constexpr int kExtOutputCount = 20; // MaterialGraphVM.hlsli の MG_VM_EXT_COUNT と一致させる
    inline constexpr int kExtHeaderTexels = kExtOutputCount / 4;
    inline constexpr int kFixedHeaderTexels = kHeaderTexels + kExtHeaderTexels;

    // Material Output の追加ピンが渡すレジスタ（MaterialGraphVM.hlsli の MG_EXT_* と同じ番号）
    enum ExtOutput : int
    {
        ToonRamp = 0, // (threshold, softness, steps, strength * mask)
        ToonColor1,   // rgb = 影色 1
        ToonColor2,   // rgb = 影色 2
        ToonColor3,   // rgb = 影色 3（最も明るい帯）, w = wrap（素材クラス）
        RimParams,    // (width, power, strength, directional mask)
        RimColor,     // rgb
        HairParams,   // (anisotropy, roughness, width, shift)
        HairTangent,  // xyz = 接線, w = strength * mask
        HairColor,    // rgb
        FaceParams,   // (sdfA, sdfB, threshold, smoothness)
        FaceForward,  // xyz = 顔の正面, w = flip
        FaceRight,    // xyz = 顔の右, w = strength
        FaceShadowColor, // rgb
        Mekakure,     // scalar（0〜1。1 = 透かさない）
        ClearCoat,    // (strength, roughness, 0, 1)
        Subsurface,   // rgb
        Anisotropy,   // xyz = 方向, w = 異方性
        ExtOutputUsed
    };
    static_assert(ExtOutputUsed <= kExtOutputCount);

    // 毎フレーム CPU が書き換えるテクセル
    struct DynamicSlot
    {
        enum Kind
        {
            Beat,  // 1 テクセル：(phase, beat index, beat time, pulse)
            Audio, // 8 テクセル：32 バンド（平滑化済み）
            Clock  // 1 テクセル：x = 音声クロック（秒。Beat Phase の Time を繋いだ式が使う）
        };
        Kind Type = Beat;
        int Texel = 0; // 絶対位置
        float Bpm = 120.0f;
        float Offset = 0.0f;
        float Subdivision = 1.0f;
        float Smoothing = 0.5f;
    };

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
        // ---- 2026-11 追加
        int InstructionStart = kHeaderTexels;   // 命令の開始テクセル（ヘッダー + 追加出力 + 動的テクセルの後）
        std::vector<DynamicSlot> Dynamics;      // 毎フレーム書き換えるテクセル
        bool UsesSceneDepth = false;            // Scene Depth / Depth Fade（描画側が不透明の深度を読める必要がある）
        bool UsesSceneColor = false;            // Scene Color（読み書きの競合を避けるため、描画順序の管理が要る）
        bool UsesFrontFacing = false;
        bool UsesCurvature = false;
        bool UsesAudio = false;                 // 音声クロック / スペクトルが要る（AudioMaterialBridge）
        bool UsesLight = false;                 // メインライトの方向を使う（Eye Highlight など）
        bool PerLightToon = false;              // ライトごとの陰影（Toon Shade / Rim / Hair / Face）が接続されている
        int ExtMask = 0;                        // 使われている ExtOutput の bit
    };

    // 動的テクセルの入力（毎フレーム）
    struct DynamicInputs
    {
        double AudioTime = 0.0;        // 音声クロック（秒）。無いときは AudioValid = false
        bool AudioValid = false;
        double WallTime = 0.0;         // 音声が無いときの代わり
        const float* Bands = nullptr;  // 32 バンド（0〜1）。nullptr = 無音
        float DeltaTime = 1.0f / 60.0f;
    };
    struct DynamicState
    {
        std::vector<std::array<float, 32>> Smoothed; // Audio スロットごとの平滑化の状態
    };
    // ヘッダー（時刻）と動的テクセルを書き換える。書いた範囲 [0, 戻り値) のテクセルを GPU へ送る（0 = 何も変えていない）
    int UpdateDynamics(const Program& program, std::vector<std::array<float, 4>>& texels, DynamicState& state, const DynamicInputs& inputs,
                       float time);

    // 時刻のテクセル（[0]）を作る
    std::array<float, 4> HeaderTexel(const Program& program, float time);
    struct Options
    {
        uint64_t TargetNode = 0;      // 0 = Material Output。それ以外はそのノードの出力を Base Color（Out0.x）に入れる
        bool IncludeDefaults = false; // 未接続の Material Output のピンも既定値で出力する（ベイク用）
    };
    Program Compile(const MaterialGraph& graph, const TextureResolver& resolveTexture);
    Program Compile(const MaterialGraph& graph, const TextureResolver& resolveTexture, const Options& options);
} // namespace MaterialGraphVm



