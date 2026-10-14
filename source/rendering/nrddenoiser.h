#pragma once
#include "pch.h"
#include <memory>

enum class NrdSignal { Shadow, Specular, AmbientOcclusion };

struct NrdTexture
{
    ID3D12Resource* Resource = nullptr;
    D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
};

struct NrdInputs
{
    NrdTexture Motion;
    NrdTexture NormalRoughness;
    NrdTexture ViewZ;
    NrdTexture NoisySignal;
};

struct NrdFrame
{
    XMFLOAT4X4 View{};
    XMFLOAT4X4 Projection{};
    XMFLOAT4X4 PreviousView{};
    XMFLOAT4X4 PreviousProjection{};
    XMFLOAT2 Jitter{};
    XMFLOAT2 PreviousJitter{};
    XMFLOAT3 LightDirection{};
    UINT FrameIndex = 0; // safe engine frame slot
    UINT Sequence = 0;   // consecutive temporal frame number
    float Range = 2000.0f;
    float AmbientRadius = 1.5f;
    bool ResetHistory = false;
};

// Native D3D12 integration of NVIDIA NRD. Each signal/light owns its temporal history.
// Execute restores input states; the result is returned in PIXEL_SHADER_RESOURCE state.
class NrdDenoiser
{
public:
    NrdDenoiser();
    ~NrdDenoiser();
    NrdDenoiser(const NrdDenoiser&) = delete;
    NrdDenoiser& operator=(const NrdDenoiser&) = delete;
    bool Initialize(ID3D12Device* device, NrdSignal signal, UINT width, UINT height);
    bool Execute(ID3D12GraphicsCommandList* commands, const NrdInputs& inputs, const NrdFrame& frame);
    ID3D12Resource* GetOutput() const;
    UINT GetDispatchCount() const;
    bool DidRestartHistory() const;
    static bool IsCompiled();
    static const char* GetVersion();
private:
    struct Impl;
    unique_ptr<Impl> m_Impl;
};
