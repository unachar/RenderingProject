#include "CloudCommon.hlsli"
float4 main(PSInputPostProcess input) : SV_Target0
{
    float distance = SceneRayDistance(input.TexCoord);
    float2 position = input.TexCoord * CloudExtent.zw - 0.5;
    int2 origin = int2(floor(position));
    float2 fraction = frac(position);
    float4 cloud = 0;
    float totalWeight = 0;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x)
    {
        int2 coord = clamp(origin + int2(x, y), int2(0, 0), int2(CloudExtent.zw) - 1);
        float sceneDistance = CloudDepth.Load(int3(coord, 0)).x;
        float difference = abs(sceneDistance - distance);
        float tolerance = max(0.1, distance * 0.02);
        float depthWeight = exp(-difference / tolerance);
        // Reject incompatible foreground/background samples completely.
        if (difference > tolerance * 4) depthWeight = 0;
        float2 bilinear = lerp(1 - fraction, fraction, float2(x, y));
        float weight = bilinear.x * bilinear.y * depthWeight;
        cloud += CloudScatteringTransmittance.Load(int3(coord, 0)) * weight;
        totalWeight += weight;
    }
    if (totalWeight < 0.0001)
        return MarchCloud(input.TexCoord, input.Position.xy).Color;
    return cloud / totalWeight;
}
