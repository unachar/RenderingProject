#pragma once

#include "pch.h"
#include "dlssupscaler.h"
#include "graphicsdevice.h"
#include "renderersettings.h"
#include <algorithm>
#include <cmath>

// 時間方向アップスケール（DLSS / TSR）用のテクスチャ Mip LOD バイアス。
//
// 内部解像度で描くと、GPU は内部解像度に合ったミップ（＝表示解像度より粗いミップ）を選ぶ。
// アップスケーラーはジッターしたサンプルを時間方向に積み重ねて表示解像度へ復元するので、
// テクスチャも表示解像度相当のミップを読ませないと、復元後もテクスチャだけがぼやける。
//
//   bias = log2(内部解像度 / 表示解像度) + Offset    （例：Performance 2.0x → -1.0）
//
// 値は LightParams（b1）末尾の TextureSamplingParams.x でシェーダーへ渡し、
// マテリアルのテクスチャ読み込み（common.hlsl の SampleMaterialBiasCommon / SampleMaterialGradCommon）で適用する。
// ルートシグネチャ・静的サンプラー・PSO は変更しないので、品質を切り替えても再作成は発生しない。
// 空間アップスケーラー（Bilateral / FSR1 / NIS）は時間方向の積み重ねが無く、
// 負のバイアスはそのままちらつきになるので 0 にする。
class TextureMipBias
{
  public:
    struct Settings
    {
        bool Enabled = true;
        bool ApplyToTsr = true;
        // 計算値に足す微調整。マイナスでさらにシャープ（モアレ・ちらつきが出やすい）、プラスで穏やか。
        float Offset = 0.0f;
    };

    static Settings& GetSettings()
    {
        static Settings settings;
        return settings;
    }

    static bool IsTemporalUpscaling()
    {
        const UpscaleMode mode = RendererSettings::GetUpscaleMode();
        if (mode == UpscaleMode::Dlss)
        {
            // DLL が無い等で Bilateral に戻っている間は掛けない（空間アップスケールでは負のバイアスがちらつきになる）。
            return DlssUpscaler::IsRequested() && DlssUpscaler::IsAvailable();
        }
        return mode == UpscaleMode::Tsr && GetSettings().ApplyToTsr;
    }

    // 理論値（Offset 前）。内部解像度 = 表示解像度なら 0。
    static float ComputeAutomaticBias()
    {
        const float render = static_cast<float>((std::max)(1u, GraphicsDevice::GetSceneWidth()));
        const float display = static_cast<float>((std::max)(1u, GraphicsDevice::GetWidth()));
        return (std::min)(0.0f, std::log2(render / display));
    }

    // シェーダーへ渡す最終値。異常値（ゼロ除算・極端な設定）でも [-3, 1] に収める。
    static float GetCurrentBias()
    {
        const Settings& settings = GetSettings();
        if (!settings.Enabled || !IsTemporalUpscaling())
        {
            return 0.0f;
        }
        const float bias = ComputeAutomaticBias() + settings.Offset;
        return std::isfinite(bias) ? std::clamp(bias, -3.0f, 1.0f) : 0.0f;
    }

    // TextureSamplingParams（x = バイアス, y = 2^バイアス, zw = 予約）
    static XMFLOAT4 GetShaderParams()
    {
        const float bias = GetCurrentBias();
        return {bias, std::exp2(bias), 0.0f, 0.0f};
    }
};
