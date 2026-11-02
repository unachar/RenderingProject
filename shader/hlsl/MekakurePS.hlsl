// ---------------------------------------------------------------------------------------------
// メカクレ用ピクセルシェーダー。本体と同じ ShadeForward（トゥーン / ライブスタイル込み）で陰影を付け、
// 不透明度と「顔の向き」フェードを掛けた乗算済み α で HDR シーンへ合成する。
// 正面（N・V が大きい）ほど表示、横顔・後ろ向きでは自然に消える。
// ---------------------------------------------------------------------------------------------
#define MODEL_SHADER_NO_ENTRY
#include "modelshaderPS.hlsl"

float4 main(in PSInput3D In) : SV_Target0
{
    float4 premultiplied = ShadeForward(In);
    float3 normal = SafeNormalizeCommon(In.Normal, float3(0.0f, 0.0f, 1.0f));
    float3 viewDir = SurfaceViewDirectionCommon(In.WorldPos, CameraPos);
    float fadeStart = ToonOutlineScreenWidth;
    float fadeEnd = min(ConstantPadding, fadeStart - 0.001f);
    float facing = smoothstep(fadeEnd, fadeStart, dot(normal, viewDir));
    premultiplied *= facing;
    [branch] if (Padding > 0.5f)
    {
        premultiplied *= saturate(SceneColorTexture.Load(int3(In.Position.xy, 0)).r);
    }
    clip(premultiplied.a - (1.0f / 255.0f));
    return premultiplied;
}
