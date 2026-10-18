#pragma once

#include "pch.h"

struct RaytracingGeometry
{
    ComPtr<ID3D12Resource> Vertices;
    ComPtr<ID3D12Resource> Indices;
    D3D12_VERTEX_BUFFER_VIEW VertexView{};
    D3D12_INDEX_BUFFER_VIEW IndexView{};
    UINT IndexCount = 0;
    bool Deforming = false;
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
