// Stage 1D: fullscreen triangle for one DBuffer decal (the decal
// record itself is passed as root constants to the pixel shader).
struct DecalVSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

DecalVSOutput main(uint vertexID : SV_VertexID)
{
    DecalVSOutput output;
    output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.TexCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
