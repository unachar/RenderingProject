struct LightInstanceInput
{
    uint4 TileBounds;
    uint4 Metadata;
};

StructuredBuffer<LightInstanceInput> Instances : register(t0);
RWStructuredBuffer<uint> TileLightIndices : register(u0);
RWStructuredBuffer<uint> VolumetricLightIndices : register(u1);
RWByteAddressBuffer VolumetricCounter : register(u2);

cbuffer BuilderConstants : register(b0)
{
    uint LightCount;
    uint TileCountX;
    uint TileCountY;
    uint SlotsPerTile;
    uint BuildMode;
    uint MaximumVolumetricLights;
    uint TileStride;
    uint Padding;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    uint tileElementCount = TileCountX * TileCountY * TileStride;
    if (BuildMode == 0u)
    {
        if (index < tileElementCount) TileLightIndices[index] = 0xffffffffu;
        if (index < MaximumVolumetricLights) VolumetricLightIndices[index] = 0xffffffffu;
        if (index == 0u) VolumetricCounter.Store(0, 0u);
        return;
    }

    if (index >= LightCount) return;
    LightInstanceInput instance = Instances[index];
    [loop]
    for (uint tileY = instance.TileBounds.y; tileY <= instance.TileBounds.w; ++tileY)
    {
        [loop]
        for (uint tileX = instance.TileBounds.x; tileX <= instance.TileBounds.z; ++tileX)
        {
            uint tileBase = (tileY * TileCountX + tileX) * TileStride;
            [loop]
            for (uint slot = instance.Metadata.y; slot < instance.Metadata.z; ++slot)
            {
                uint original;
                InterlockedCompareExchange(
                    TileLightIndices[tileBase + slot], 0xffffffffu,
                    instance.Metadata.x, original);
                if (original == 0xffffffffu) break;
            }
        }
    }
    if (instance.Metadata.w != 0u)
    {
        uint destination;
        VolumetricCounter.InterlockedAdd(0, 1u, destination);
        if (destination < MaximumVolumetricLights)
            VolumetricLightIndices[destination] = instance.Metadata.x;
    }
}
