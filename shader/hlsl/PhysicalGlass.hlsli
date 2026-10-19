// 誘電体の界面と、均質な平行板内の光輸送。
// 方向は光の伝播方向を指す。厚みはワールド単位。
#ifndef PHYSICAL_GLASS_INCLUDED
#define PHYSICAL_GLASS_INCLUDED

float GlassFresnel(float cosI, float etaI, float etaT)
{
    cosI = saturate(abs(cosI));
    float eta = etaI / max(etaT, 1e-6f);
    float sinT2 = eta * eta * max(0.0f, 1.0f - cosI * cosI);
    if (sinT2 >= 1.0f)
    {
        return 1.0f;
    }
    float cosT = sqrt(max(0.0f, 1.0f - sinT2));
    float rs = (etaI * cosI - etaT * cosT) / max(etaI * cosI + etaT * cosT, 1e-6f);
    float rp = (etaT * cosI - etaI * cosT) / max(etaT * cosI + etaI * cosT, 1e-6f);
    return saturate(0.5f * (rs * rs + rp * rp));
}

float3 GlassBeer(float3 tint, float3 sigma, float thickness, float path)
{
    float3 tintSigma = -log(clamp(tint, 1e-4f, 1.0f)) / max(thickness, 1e-4f);
    return exp(-min((max(sigma, 0.0f) + tintSigma) * max(path, 0.0f), 80.0f));
}

void GlassSlabWeights(float F, float3 beer, out float3 R, out float3 T)
{
    float3 denominator = max(1.0f - F * F * beer * beer, 1e-5f);
    T = (1.0f - F) * (1.0f - F) * beer / denominator;
    R = F + (1.0f - F) * (1.0f - F) * F * beer * beer / denominator;
    R = saturate(R);
    T = min(saturate(T), 1.0f - R);
}
#endif
