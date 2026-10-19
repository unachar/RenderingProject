// =============================================================================================
// MegaLights M6 : NVIDIA NRD（REBLUR_DIFFUSE）でのデノイズの入出力
//   NrdPack   : Raw（デモジュレート済みの照明, a = 1 + ヒット距離）→ REBLUR の入力（放射輝度 + 正規化ヒット距離）
//   NrdUnpack : REBLUR の出力 → MegaLights の「デノイズ済み」バッファ（ディファードが反射率を掛け戻す）
//   ガイド（Motion / NormalRoughness / ViewZ）は Raytracing が書いた共有ガイドを使う。
//   USE_NRD=1 のときだけコンパイルする（megalights.h）。
// =============================================================================================
#define MEGALIGHTS_CB_REGISTER b0
#include "MegaLightsCommon.hlsli"
#include "../../External/NRD/sdk/Shaders/NRD.hlsli"

// RayReflectionsCS / nrddenoiser.cpp の kHitDistance* と同じ
static const float3 kHitDistanceParameters = float3(3.0f, 0.1f, 20.0f);

StructuredBuffer<float4> Raw : register(t0);
Texture2D<float> GuideViewZ : register(t1);
Texture2D<float4> NrdOutput : register(t2);
RWTexture2D<float4> NoisyOut : register(u0);
RWStructuredBuffer<float4> Denoised : register(u1);

[numthreads(8, 8, 1)] void NrdPack(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= MLScreen.xy))
    {
        return;
    }
    const float4 raw = Raw[id.y * MLScreen.x + id.x];
    if (raw.a < 0.5f)
    {
        // MegaLights の対象外（非 PBR 面・ライトなし）：照明 0、ヒット距離は「とても遠い」
        NoisyOut[id.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(0.0f, 1.0f, true);
        return;
    }
    const float viewZ = GuideViewZ.Load(int3(id.xy, 0));
    const float hitDistance = max(raw.a - 1.0f, 0.0f);
    const float normalized = REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, kHitDistanceParameters, 1.0f);
    NoisyOut[id.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(max(raw.rgb, 0.0f), normalized, true);
}

    [numthreads(8, 8, 1)] void NrdUnpack(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= MLScreen.xy))
    {
        return;
    }
    const float3 radiance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(NrdOutput.Load(int3(id.xy, 0))).rgb;
    Denoised[id.y * MLScreen.x + id.x] = float4(all(isfinite(radiance)) ? max(radiance, 0.0f) : 0.0f, 0.0f);
}
