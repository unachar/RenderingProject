#ifndef MATERIAL_TEXTURE_HEAP_INCLUDED
#define MATERIAL_TEXTURE_HEAP_INCLUDED

#ifndef RP_BINDLESS
#define RP_BINDLESS 0
#endif

#if !RP_BINDLESS
Texture2D AssetTextureHeap[512] : register(t0, space1);
#endif

// マテリアルの保存形式は従来どおりテクスチャ領域内のインデックス。
// Bindless では、同じ領域の先頭を加えて共有 CBV/SRV/UAV ヒープを直接参照する。
Texture2D LoadMaterialTexture(uint index)
{
#if RP_BINDLESS
#ifdef SHADER_GPU_DRIVEN
    uint baseIndex = RP_TEXTURE_HEAP_BASE;
#else
    uint baseIndex = (uint)AssetParallax.w;
#endif
    return ResourceDescriptorHeap[NonUniformResourceIndex(baseIndex + index)];
#else
    return AssetTextureHeap[NonUniformResourceIndex(index)];
#endif
}

#endif
