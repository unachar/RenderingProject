#pragma once
#include "rendererstate.h"
#include <array>
#include <cstddef>
#include "transientarena.h"

enum class RenderTargetType
{
    Scene,
    PostProcess,
    PreUpscaleAa,
    PreUpscaleAaHistory,
    EditorScene,
    Count
};

constexpr size_t kRenderTargetCount = static_cast<size_t>(RenderTargetType::Count);
using RenderTargetResourceArray = array<ComPtr<ID3D12Resource>, kRenderTargetCount>;
using RenderTargetCpuHandleArray = array<D3D12_CPU_DESCRIPTOR_HANDLE, kRenderTargetCount>;
using RenderTargetGpuHandleArray = array<D3D12_GPU_DESCRIPTOR_HANDLE, kRenderTargetCount>;

class RenderTargets : protected RendererState
{
  protected:
    static void GetGBufferClearColor(UINT index, float outColor[4]);

    inline static TransientArena m_TransparentAaArena;
    inline static bool m_TransparentAaAliased = false;
    inline static bool m_AliasRequested = false;
    inline static RenderTargetResourceArray m_RenderTargetResources{};
    inline static RenderTargetCpuHandleArray m_RenderTargetRtvHandles{};
    inline static RenderTargetGpuHandleArray m_RenderTargetSrvHandles{};

    static constexpr size_t ToIndex(RenderTargetType type)
    {
        return static_cast<size_t>(type);
    }

    static ComPtr<ID3D12Resource>& GetRenderTarget(RenderTargetType type)
    {
        return m_RenderTargetResources[ToIndex(type)];
    }

    static D3D12_CPU_DESCRIPTOR_HANDLE& GetRenderTargetRtvHandle(RenderTargetType type)
    {
        return m_RenderTargetRtvHandles[ToIndex(type)];
    }

    static D3D12_GPU_DESCRIPTOR_HANDLE& GetRenderTargetSrvHandle(RenderTargetType type)
    {
        return m_RenderTargetSrvHandles[ToIndex(type)];
    }

  public:
    using RendererState::GetGBufferFormat;
    using RendererState::GetSceneColorFormat;

    static bool CreateDepthBuffer();
    static bool CreateShadowDepthBuffer(UINT layers = g_kMAX_SHADOW_LIGHTS, UINT resolution = g_kSHADOW_MAP_SIZE);
    static bool ConfigureShadowDepthBuffer(UINT layers, UINT resolution);
    static bool CreateSceneRenderTarget();
    static void ReleaseGBufferResources();

    static void ResizeScene(UINT width, UINT height);
    static void UpdateMemoryPolicy();

    static void ReleaseMemoryResources()
    {
        m_TransparentAaArena.Reset();
        m_TransparentAaAliased = false;
    }

    static void ActivateTransparentCopy(ID3D12GraphicsCommandList* commands)
    {
        if (m_TransparentAaAliased)
        {
            m_TransparentAaArena.Activate(0, commands);
        }
    }

    static void ActivatePreUpscaleAa(ID3D12GraphicsCommandList* commands)
    {
        if (m_TransparentAaAliased)
        {
            m_TransparentAaArena.Activate(1, commands);
        }
    }

    static D3D12_GPU_DESCRIPTOR_HANDLE GetSceneSrvHandle()
    {
        return GetRenderTargetSrvHandle(RenderTargetType::Scene);
    }

    static D3D12_GPU_DESCRIPTOR_HANDLE GetEditorSceneSrvHandle()
    {
        return GetRenderTargetSrvHandle(RenderTargetType::EditorScene);
    }

    static ID3D12Resource* GetSceneResource()
    {
        return GetRenderTarget(RenderTargetType::Scene).Get();
    }

    static ID3D12Resource* GetEditorSceneResource()
    {
        return GetRenderTarget(RenderTargetType::EditorScene).Get();
    }

    static D3D12_GPU_DESCRIPTOR_HANDLE GetGBufferSrvHandle(GBufferType type)
    {
        return m_GBufferSrvHandles[static_cast<UINT>(type)];
    }

    static ID3D12Resource* GetGBufferResource(GBufferType type)
    {
        return m_GBufferTargets[static_cast<UINT>(type)].Get();
    }

    static D3D12_GPU_DESCRIPTOR_HANDLE GetAtmosphereSrvHandle()
    {
        return GetGBufferSrvHandle(GBufferType::ATMOSPHERE);
    }

    static ID3D12Resource* GetShadowDepthResource()
    {
        return m_ShadowDepthBuffer.Get();
    }

    static D3D12_CPU_DESCRIPTOR_HANDLE GetImGuiCpuHandle();
    static D3D12_GPU_DESCRIPTOR_HANDLE GetImGuiGpuHandle();
};
