#pragma once

#include "pch.h"
#include "procedural/proceduraltextureparams.h"

struct RaytracingGeometry
{
    // 現在の TLAS インスタンス順序に依存しない、エンティティとサブメッシュの識別情報。
    uint64_t SurfaceOwner = 0;
    ComPtr<ID3D12Resource> Vertices;
    ComPtr<ID3D12Resource> Indices;
    D3D12_VERTEX_BUFFER_VIEW VertexView{};
    D3D12_INDEX_BUFFER_VIEW IndexView{};
    UINT IndexCount = 0;
    bool Deforming = false;
    // スキニング出力（キャラクター等）。BLAS 頂点の 16bit 量子化をサイズ不明でも許可する。
    bool Skinned = false;
    // オブジェクト空間のバウンディング球半径。負値は不明（16bit 量子化の誤差判定ができないため 32bit のまま）。
    float ObjectRadius = -1.0f;
    bool CastShadow = true;
    D3D12_RESOURCE_STATES VertexState = D3D12_RESOURCE_STATE_GENERIC_READ;
    D3D12_RESOURCE_STATES IndexState = D3D12_RESOURCE_STATE_GENERIC_READ;
    XMFLOAT4X4 World{};
    ComPtr<ID3D12Resource> Texture;
    int TextureIndex = -1;
    UINT TextureResidentMip = 0;
    XMFLOAT4 BaseColor{1, 1, 1, 1};
    XMFLOAT4 Emission{};
    ComPtr<ID3D12Resource> EmissionTexture;
    UINT EmissionResidentMip = 0;
    // POM の高さマップ（反射の二次ヒットで UV のずれと凹凸法線に使う）。不透明マテリアルアセットのみ。
    ComPtr<ID3D12Resource> HeightTexture;
    UINT HeightResidentMip = 0;
    float ParallaxDepth = 0.0f;
    std::array<ProcStudio::TextureGpuParams, 3> Noise{};
    float NoiseNormalScale = 1.0f;
    float Roughness = 0.5f;
    float Metallic = 0.0f;
    UINT MediumID = 0;
    float IOR = 1.5f;
    float Transmission = 0.0f;
    float TransmissionRoughness = 0.02f;
    float Fresnel = 0.04f;
    bool UsesAssetMaterial = false;
    XMFLOAT3 Absorption{};
    float Thickness = 0.01f;
};

vector<RaytracingGeometry> GatherRaytracingGeometry();
