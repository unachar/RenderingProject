#include "pch.h"
#include "rendererstate.h"

ComPtr<ID3D12Device> RendererState::m_Device;
ComPtr<ID3D12CommandQueue> RendererState::m_CommandQueue;
ComPtr<IDXGISwapChain4> RendererState::m_SwapChain;
ComPtr<ID3D12DescriptorHeap> RendererState::m_RtvHeap;
ComPtr<ID3D12Resource> RendererState::m_RenderTargets[RendererState::g_kFRAME_COUNT];
ComPtr<ID3D12CommandAllocator> RendererState::m_CommandAllocator[RendererState::g_kFRAME_COUNT];
ComPtr<ID3D12GraphicsCommandList> RendererState::m_CommandList;
ComPtr<IDXGIFactory4> RendererState::m_Factory;
ComPtr<ID3D12Fence> RendererState::m_Fence;
ComPtr<ID3D12RootSignature> RendererState::m_RootSignature;
ComPtr<ID3D12PipelineState> RendererState::m_PipelineState;
unordered_map<string, ComPtr<ID3D12PipelineState>> RendererState::m_PsoCache;
ComPtr<ID3D12DescriptorHeap> RendererState::m_CbvHeap;
ComPtr<ID3D12Resource> RendererState::m_ConstantBuffer;
UINT8* RendererState::m_pCbvDataBegin = nullptr;
HANDLE RendererState::m_FenceEvent = nullptr;

CD3DX12_VIEWPORT RendererState::m_Viewport;
CD3DX12_RECT RendererState::m_ScissorRect;
UINT64 RendererState::m_FenceValues[g_kFRAME_COUNT] = { 0, 0, 0 };
UINT64 RendererState::m_CurrentFenceValue = 0;
UINT RendererState::m_FrameIndex = 0;
UINT RendererState::m_Width = 0;
UINT RendererState::m_Height = 0;
UINT RendererState::m_SceneWidth = 0;
UINT RendererState::m_SceneHeight = 0;
HWND RendererState::m_Hwnd = nullptr;
UINT RendererState::m_CbvIncrementSize = 0;

ComPtr<ID3D12RootSignature> RendererState::m_ModelRootSignature;
ComPtr<ID3D12PipelineState> RendererState::m_ModelPipelineState;
ComPtr<ID3D12Resource> RendererState::m_DepthStencilBuffer;
ComPtr<ID3D12DescriptorHeap> RendererState::m_DsvHeap;
ComPtr<ID3D12Resource> RendererState::m_ShadowDepthBuffer;
ComPtr<ID3D12PipelineState> RendererState::m_ShadowMapPso;
ComPtr<ID3D12Resource> RendererState::m_ShadowConstantBuffer;
void* RendererState::m_pShadowCbvDataBegin = nullptr;
CD3DX12_VIEWPORT RendererState::m_ShadowViewport;
CD3DX12_RECT RendererState::m_ShadowScissorRect;

ComPtr<ID3D12Resource> RendererState::m_SceneRenderTarget;
ComPtr<ID3D12Resource> RendererState::m_EditorSceneRenderTarget;
ComPtr<ID3D12DescriptorHeap> RendererState::m_SceneRtvHeap;
CD3DX12_CPU_DESCRIPTOR_HANDLE RendererState::m_SceneRtvHandle;
CD3DX12_CPU_DESCRIPTOR_HANDLE RendererState::m_EditorSceneRtvHandle;
CD3DX12_GPU_DESCRIPTOR_HANDLE RendererState::m_SceneSrvHandle;
CD3DX12_GPU_DESCRIPTOR_HANDLE RendererState::m_EditorSceneSrvHandle;
ComPtr<ID3D12Resource> RendererState::m_GBufferTargets[RendererState::g_kGBUFFER_COUNT];
CD3DX12_CPU_DESCRIPTOR_HANDLE RendererState::m_GBufferRtvHandles[RendererState::g_kGBUFFER_COUNT];
CD3DX12_GPU_DESCRIPTOR_HANDLE RendererState::m_GBufferSrvHandles[RendererState::g_kGBUFFER_COUNT];
int RendererState::m_EnvironmentTextureSrvIndex = -1;
ComPtr<ID3D12RootSignature> RendererState::m_PostProcessRootSignature;
unordered_map<PostProcessType, ComPtr<ID3D12PipelineState>> RendererState::m_PostProcessPsoMap;
ComPtr<ID3D12PipelineState> RendererState::m_DeferredLightingPso;
ComPtr<ID3D12Resource> RendererState::m_PostProcessConstantBuffer;
ComPtr<ID3D12Resource> RendererState::m_LightConstantBuffer;
ComPtr<ID3D12Resource> RendererState::m_PBRConstantBuffer;
void* RendererState::m_pPostProcessCbvDataBegin = nullptr;
void* RendererState::m_pLightCbvDataBegin = nullptr;
void* RendererState::m_pPBRCbvDataBegin = nullptr;
RenderMode RendererState::m_RenderMode = RenderMode::DEFERRED;
RenderMode RendererState::m_PendingRenderMode = RenderMode::DEFERRED;
bool RendererState::m_HasPendingRenderMode = false;
bool RendererState::m_IsDeferredGeometryPass = false;
bool RendererState::m_IsSceneColorForwardPass = false;
D3D12_RESOURCE_STATES RendererState::m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

ComPtr<ID3D12Resource> RendererState::m_DynamicVertexBuffer;
D3D12_VERTEX_BUFFER_VIEW RendererState::m_DynamicVertexBufferView;
Vertex* RendererState::m_pDynamicVertexDataBegin = nullptr;
UINT RendererState::m_DynamicVertexOffset = 0;
UINT RendererState::m_TransientCbSlot = 0;

ComPtr<ID3D12RootSignature> RendererState::m_SkinningRootSignature;
ComPtr<ID3D12PipelineState> RendererState::m_SkinningPso;
DXGI_FORMAT RendererState::m_SceneColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
DXGI_FORMAT RendererState::m_BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
bool RendererState::m_HasPendingHdr = false;
bool RendererState::m_PendingHdr = false;




