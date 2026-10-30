// Stage 1D: merge the DBuffer into the GBuffer before deferred
// lighting. Rendered as a fullscreen pass over the GBuffer render
// targets with hardware blending doing the read-modify-write:
// - base color RT: lerp(baseColor, decalAlbedo, mask)
// - normal RT:     lerp(encodedNormal, encodedDecalNormal, mask)
// - material RT:   GREEN channel only (write mask in the PSO), so
//                  roughness lerps while metallic/shader class are
//                  preserved.
// Pixels the decals never touched have mask 0 and are discarded.
Texture2D DBufferAlbedo : register(t0);
Texture2D DBufferNormal : register(t1);
Texture2D DBufferRoughnessMask : register(t2);

struct DecalCompositeInput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

struct DecalCompositeOutput
{
    float4 BaseColor : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Material : SV_Target2;
};

DecalCompositeOutput main(DecalCompositeInput input)
{
    int2 pixel = int2(input.Position.xy);
    float4 roughnessMask = DBufferRoughnessMask.Load(int3(pixel, 0));
    clip(roughnessMask.a - 1.0f / 255.0f);
    float4 albedo = DBufferAlbedo.Load(int3(pixel, 0));
    float3 normal = DBufferNormal.Load(int3(pixel, 0)).xyz;

    DecalCompositeOutput output;
    output.BaseColor = float4(albedo.rgb, roughnessMask.a);
    output.Normal = float4(normal * 0.5f + 0.5f, roughnessMask.a);
    output.Material = float4(0.0f, roughnessMask.r, 0.0f, roughnessMask.a);
    return output;
}
