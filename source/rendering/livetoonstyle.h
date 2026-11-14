#pragma once
#include <array>
#include <string>
#include <unordered_map>
#include "texturemanager.h"
#include "rendererstate.h"

// ---------------------------------------------------------------------------------------------
// ライブ用トゥーンスタイル（学マスのライブシーン風）
//
// CEDEC2024「学園アイドルマスター」講演で紹介された構成を、このエンジンのトゥーン経路に合わせて
// 再構成したもの（実装は独自。パラメーター名・初期値は本エンジン用の目安）：
//   1. トゥーン陰影用メインライト  … 平行光源だけがセル陰影の境界を作る。影領域は「乗算色」で制御
//   2. 加算ライト                  … 点 / スポット / 矩形は陰影の境界を作らず、距離減衰した明るさだけを足す
//   3. リムライト                  … ふわっと広いリム + 輪郭を締める強いリム（ライト色を拾う）
//   4. スペキュラ（ランプ）         … 段のついたハイライト。髪は天使の輪（異方性バンド）
//   5. 環境スペキュラ               … IBL（環境マップ）からの縁の映り込み
//   6. 落ち影の濃さ                 … セルフシャドウ / 落ち影の濃さを陰影と独立に調整
//
// Deferred のフルスクリーンパスは他エンティティのマテリアルを参照できないため、これはシーン全体の
// 設定として LightParams（b1）の末尾 6 個の float4 で渡す（ルートシグネチャは変更しない）。
// ---------------------------------------------------------------------------------------------
struct LiveToonStyleSettings
{
    float Blend = 1.0f;              // 0 = 従来のトゥーン、1 = ライブスタイル
    float ShadowTintStrength = 1.0f; // 影の乗算色の効き
    float AdditiveSoftness = 0.65f;  // 加算ライトの回り込み（大きいほど面の向きに依存しない）
    float AdditiveGain = 1.0f;       // 加算ライトの強さ

    XMFLOAT3 ShadowTint = {0.78f, 0.72f, 0.90f}; // 影の乗算色（わずかに青紫へ寄せると映える）
    float SkinWarmth = 0.65f;                    // 肌の影を暖色へ寄せる量（血色）

    float RimSoft = 0.35f;      // ふわっとしたリム
    float RimHard = 0.55f;      // 輪郭を締める強いリム（逆光・ステージライト）
    float RimWidth = 0.38f;     // リム幅
    float RimLightTint = 0.75f; // リムがライト色を拾う量

    float RampSpecular = 0.30f;      // 段付きスペキュラ（ランプ）
    float HairAngelRing = 0.40f;     // 髪の天使の輪
    float EnvSpecular = 0.20f;       // 環境マップの縁の映り込み
    float CastShadowDensity = 0.85f; // 落ち影の濃さ

    // 陰影ランプ（シーン全体・1 枚）。横軸 = 影(左)→光(右)、上半分 = 肌、下半分 = 肌以外。
    std::string ShadowRampPath;
    float ShadowRampStrength = 1.0f;
    bool PackedShadowRamp = false;
    std::string SpecularRampPath;
    bool SpecularRampByPart = false;
    float LocalSpecular = 0.0f;
    float BacklightDiffuse = 0.0f;
    float EnvironmentRoughness = 0.5f;
    // 質感ランプ（SpecularRampPath）の行数。マテリアルで行（1..7）を指定したときの縦の分割数（学マスは 6 行）。
    int SpecularRampRowCount = 6;
};

// マテリアル・ランプ用のテクスチャ解決。パス → ディスクリプタ番号をキャッシュし、
// 毎フレームの定数構築では文字列マップの検索だけで済ませる。
namespace LiveToonTextures
{
    inline int ResolveDescriptor(const std::string& path)
    {
        if (path.empty())
        {
            return -1;
        }
        static std::unordered_map<std::string, int> cache;
        auto it = cache.find(path);
        if (it != cache.end())
        {
            return it->second;
        }
        // LoadTexture(const char*) は UTF-8 パスを受け取り、読み込み済みなら同じ番号を返す。
        int descriptor = TextureManager::LoadTexture(path.c_str());
        if (descriptor == TextureManager::GetErrorTextureIndex())
        {
            descriptor = -1;
        }
        if (descriptor >= 0)
        {
            cache.emplace(path, descriptor);
        }
        return descriptor;
    }

    // シェーダーのテクスチャ表（t0 space1 / t0 space5）内の番号。無効なら UINT32_MAX。
    inline uint32_t ResolveHeapOffset(const std::string& path)
    {
        const int descriptor = ResolveDescriptor(path);
        if (descriptor < static_cast<int>(RendererState::g_kTEXTURE_SRV_START_INDEX))
        {
            return UINT32_MAX;
        }
        const uint32_t offset = static_cast<uint32_t>(descriptor) - RendererState::g_kTEXTURE_SRV_START_INDEX;
        return offset < 512u ? offset : UINT32_MAX;
    }
} // namespace LiveToonTextures

namespace LiveToonStyle
{
    inline LiveToonStyleSettings& Get()
    {
        static LiveToonStyleSettings settings{};
        return settings;
    }

    inline void Reset()
    {
        Get() = LiveToonStyleSettings{};
    }

    // キャラ別の陰影ランプ（マテリアルの LiveToonShadowRampPath）。最大 6 枚をスロット 1..6 に割り当てる。
    // スロットは一度割り当てたら変えない（マテリアル定数と LightParams の表が常に一致する）。0 = シーン共通。
    constexpr uint32_t kCharacterRampSlots = 6;
    inline std::array<std::string, kCharacterRampSlots>& CharacterRampPaths()
    {
        static std::array<std::string, kCharacterRampSlots> paths{};
        return paths;
    }

    inline uint32_t AcquireShadowRampSlot(const std::string& path)
    {
        if (path.empty())
        {
            return 0u;
        }
        auto& paths = CharacterRampPaths();
        for (uint32_t i = 0; i < kCharacterRampSlots; ++i)
        {
            if (paths[i] == path)
            {
                return i + 1u;
            }
        }
        for (uint32_t i = 0; i < kCharacterRampSlots; ++i)
        {
            if (paths[i].empty())
            {
                paths[i] = path;
                return i + 1u;
            }
        }
        return 0u; // 7 枚目以降はシーン共通のランプにフォールバック
    }

    inline float CharacterRampHeapOffset(uint32_t index)
    {
        const uint32_t offset = LiveToonTextures::ResolveHeapOffset(CharacterRampPaths()[index]);
        return offset == UINT32_MAX ? -1.0f : static_cast<float>(offset);
    }

    // LightParams.LiveToonStyle0..7 と同じ並び。
    inline void Write(XMFLOAT4 (&out)[8])
    {
        const LiveToonStyleSettings& s = Get();
        out[0] = {s.Blend, s.ShadowTintStrength, s.AdditiveSoftness, s.AdditiveGain};
        out[1] = {s.ShadowTint.x, s.ShadowTint.y, s.ShadowTint.z, s.SkinWarmth};
        out[2] = {s.RimSoft, s.RimHard, s.RimWidth, s.RimLightTint};
        out[3] = {s.RampSpecular, s.HairAngelRing, s.EnvSpecular, s.CastShadowDensity};
        const uint32_t ramp = LiveToonTextures::ResolveHeapOffset(s.ShadowRampPath);
        const uint32_t specularRamp = LiveToonTextures::ResolveHeapOffset(s.SpecularRampPath);
        // Values 1–512 use the shared row. Values 513–1024 select rows by material class.
        const uint32_t specularRampCode =
            specularRamp == UINT32_MAX ? 0u : specularRamp + 1u + (s.SpecularRampByPart ? 512u : 0u);
        out[4] = {ramp == UINT32_MAX ? -1.0f : static_cast<float>(ramp), s.ShadowRampStrength,
                  s.PackedShadowRamp ? 1.0f : 0.0f,
                  static_cast<float>(specularRampCode)};
        out[5] = {s.LocalSpecular, s.BacklightDiffuse, s.EnvironmentRoughness, 0.0f};
        // キャラ別陰影ランプの表（スロット 1..6 → テクスチャ表の番号、< 0 で無効）と質感ランプの行数
        out[6] = {CharacterRampHeapOffset(0), CharacterRampHeapOffset(1), CharacterRampHeapOffset(2),
                  CharacterRampHeapOffset(3)};
        out[7] = {CharacterRampHeapOffset(4), CharacterRampHeapOffset(5),
                  static_cast<float>(s.SpecularRampRowCount < 1 ? 1 : (s.SpecularRampRowCount > 16 ? 16 : s.SpecularRampRowCount)),
                  0.0f};
    }
} // namespace LiveToonStyle


