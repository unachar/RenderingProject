#pragma once
#include "main.h"
#include "ecs.h"
#include <d3dcompiler.h>
#include <string>
#include <unordered_map>

enum class PostProcessType;
struct PostProcessComponent;
struct MaterialComponent;

enum class RenderMode
{
	FORWARD,
	DEFERRED
};

enum class GBufferType : uint32_t
{
	BASE_COLOR = 0,
	NORMAL,
	DEPTH,
	MATERIAL,
	SHADOW,
	ATMOSPHERE,
	VELOCITY,
	VISIBILITY,
	COUNT
};

inline constexpr LPCWSTR g_GBufferTargetNames[] =
{
	L"ColorBuffer",
	L"NormalBuffer",
	L"DepthBuffer",
	L"MaterialBuffer",
	L"ShadowBuffer",
	L"AtmosphereGBuffer",
	L"VelocityBuffer",
	L"VisibilityBuffer"
};

struct Vertex
{
	XMFLOAT3 Pos{};
	XMFLOAT3 Normal{};
	XMFLOAT2 Tex{};
	XMFLOAT4 Color{};
};

enum class ShapeType
{
	NONE,
	TRIANGLE,
	QUAD,
	PENTAGON,
	HEXAGON,
	HEPTAGON,
	OCTAGON,
	CIRCLE
};

enum class ObjectType
{
	NONE,
	CUBE,
	SPHERE,
	CAPSULE,
	CYLINDER,
	PLANE,
	QUAD
};

enum class Color
{
	NONE,
	WHITE,
	RED,
	GREEN,
	BLUE,
	YELLOW,
	CYAN,
	MAGENTA,
};

enum class PostProcessType
{
	NONE = 0,
	BLUR,
	SEPIA,
	GRAYSCALE,
	INVERT,
	COUNT
};

enum class AntiAliasingMode
{
	NONE = 0,
	FXAA,
	TAA,
	COUNT
};

struct ConstantBuffer3D
{
	XMMATRIX World{};
	XMMATRIX View{};
	XMMATRIX Projection{};
	int UseTexture = 0;
	int FlipNormal = 0;
	int UseNormalMap = 0;
	int MaterialMode = 0;
	XMFLOAT3 CameraPos = { 0.0f, 0.0f, 5.0f };
	int ShaderClass = 10;
	float MaterialMetallic = 0.0f;
	float MaterialRoughness = 0.5f;
	float MaterialFresnel = 0.04f;
	float MaterialPadding = 0.0f;
	float ToonOutlineWidth = 0.035f;
	float ToonOutlineScreenWidth = 3.0f;
	XMFLOAT2 ViewportSize = { 1.0f, 1.0f };
	int ToonOutlineUseScreenSpace = 0;
	float MaterialAlpha = 1.0f;
	int MaterialIsTransparent = 0;
	float ConstantPadding = 0.0f;
	XMMATRIX PreviousWorld{};
	XMMATRIX PreviousViewProjection{};
};

struct VertexResource
{
	EntityID entityid = g_kINVALID_ENTITY;
	ShapeType shapetype = ShapeType::NONE;
	ObjectType objectType = ObjectType::NONE;
	float radius = 0.f;
	Color color = Color::NONE;
};

class RendererState
{
public:
	static constexpr UINT g_kGBUFFER_COUNT = static_cast<UINT>(GBufferType::COUNT);
	static constexpr UINT g_kGEOMETRY_GBUFFER_COUNT = static_cast<UINT>(GBufferType::ATMOSPHERE);
	static constexpr uint32_t g_kFRAME_COUNT = 3;
	// The authored scene may contain hundreds of emitters.  Only the highest
	// priority on-screen physical lights enter this bounded GPU-visible set.
	static constexpr UINT g_kMAX_SHADER_LIGHTS = 160;
	static constexpr UINT g_kLIGHT_TILE_SIZE = 16;
	static constexpr UINT g_kMAX_LIGHTS_PER_TILE = 8;
	// Color32 + Normal32 + Depth32 + Material64 + Shadow32.
	static constexpr UINT g_kGEOMETRY_GBUFFER_BITS_PER_PIXEL = 192;
	static_assert(g_kGEOMETRY_GBUFFER_BITS_PER_PIXEL <= 256,
		"Stage renderer GBuffer must stay within the 256-bit bandwidth budget");
	static constexpr UINT g_kMAX_LIGHT_GRID_WIDTH = 7680;
	static constexpr UINT g_kMAX_LIGHT_GRID_HEIGHT = 4320;
	static constexpr UINT g_kMAX_LIGHT_TILE_COUNT =
		((g_kMAX_LIGHT_GRID_WIDTH + g_kLIGHT_TILE_SIZE - 1) / g_kLIGHT_TILE_SIZE) *
		((g_kMAX_LIGHT_GRID_HEIGHT + g_kLIGHT_TILE_SIZE - 1) / g_kLIGHT_TILE_SIZE);

	static constexpr UINT g_kMAX_SHADOW_LIGHTS = 8;
	static constexpr UINT g_kMAX_VIRTUAL_SHADOW_LEVELS = 4;
	// A 4x4 requested window is kept resident inside the 16x16 physical atlas.
	// Camera movement remaps the window as a ring and redraws only entering pages.
	static constexpr UINT g_kVIRTUAL_SHADOW_RESIDENT_PAGES_PER_DIMENSION = 4;
	static constexpr UINT g_kMAX_SHADOW_PASSES =
		g_kMAX_VIRTUAL_SHADOW_LEVELS *
		g_kVIRTUAL_SHADOW_RESIDENT_PAGES_PER_DIMENSION *
		g_kVIRTUAL_SHADOW_RESIDENT_PAGES_PER_DIMENSION +
		g_kMAX_SHADOW_LIGHTS;
protected:
	static ComPtr<ID3D12Device> m_Device;
	static ComPtr<ID3D12CommandQueue> m_CommandQueue;
	static ComPtr<IDXGISwapChain4> m_SwapChain;
	static ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
	static ComPtr<ID3D12Resource> m_RenderTargets[g_kFRAME_COUNT];
	static ComPtr<ID3D12CommandAllocator> m_CommandAllocator[g_kFRAME_COUNT];
	static ComPtr<ID3D12GraphicsCommandList> m_CommandList;
	static ComPtr<IDXGIFactory4> m_Factory;
	static ComPtr<ID3D12Fence> m_Fence;
	static ComPtr<ID3D12RootSignature> m_RootSignature;
	static ComPtr<ID3D12PipelineState> m_PipelineState;
	static unordered_map<string, ComPtr<ID3D12PipelineState>> m_PsoCache;
	static ComPtr<ID3D12DescriptorHeap> m_CbvHeap;
	static ComPtr<ID3D12Resource> m_ConstantBuffer;
	static UINT8* m_pCbvDataBegin;
	static HANDLE m_FenceEvent;
	static HANDLE m_FrameLatencyWaitableObject;

	static CD3DX12_VIEWPORT m_Viewport;
	static CD3DX12_VIEWPORT m_FullViewport;
	static CD3DX12_RECT m_FullScissorRect;
	static CD3DX12_RECT m_ScissorRect;
	static UINT64 m_FenceValues[g_kFRAME_COUNT];
	static UINT64 m_CurrentFenceValue;
	static UINT m_FrameIndex;
	static UINT m_Width;
	static UINT m_Height;
	static UINT m_SceneWidth;
	static UINT m_SceneHeight;
	static float m_ResolutionScale;
	static float m_PendingResolutionScale;
	static bool m_HasPendingResolutionScale;
	static HWND m_Hwnd;
	static UINT m_CbvIncrementSize;

	static ComPtr<ID3D12RootSignature> m_ModelRootSignature;
	static ComPtr<ID3D12PipelineState> m_ModelPipelineState;
	static ComPtr<ID3D12Resource> m_DepthStencilBuffer;
	static ComPtr<ID3D12Resource> m_LowResDepthBuffer;
	static ComPtr<ID3D12DescriptorHeap> m_DsvHeap;
	static CD3DX12_CPU_DESCRIPTOR_HANDLE m_LowResDsvHandle;
	static bool m_UseLowResDepth;
	static ComPtr<ID3D12Resource> m_ShadowDepthBuffer;
	static ComPtr<ID3D12PipelineState> m_ShadowMapPso;
	static ComPtr<ID3D12Resource> m_ShadowConstantBuffer;
	static void* m_pShadowCbvDataBegin;
	static CD3DX12_VIEWPORT m_ShadowViewport;
	static CD3DX12_RECT m_ShadowScissorRect;

	static ComPtr<ID3D12Resource> m_SceneRenderTarget;
	static ComPtr<ID3D12Resource> m_PostProcessRenderTarget;
	static ComPtr<ID3D12Resource> m_PreUpscaleAaRenderTarget;
	static ComPtr<ID3D12Resource> m_PreUpscaleAaHistory;
	static ComPtr<ID3D12Resource> m_EditorSceneRenderTarget;
	static ComPtr<ID3D12Resource> m_TransparentSceneCopy;
	static ComPtr<ID3D12DescriptorHeap> m_SceneRtvHeap;
	static CD3DX12_CPU_DESCRIPTOR_HANDLE m_SceneRtvHandle;
	static CD3DX12_CPU_DESCRIPTOR_HANDLE m_PostProcessRtvHandle;
	static CD3DX12_CPU_DESCRIPTOR_HANDLE m_PreUpscaleAaRtvHandle;
	static CD3DX12_CPU_DESCRIPTOR_HANDLE m_EditorSceneRtvHandle;
	static CD3DX12_GPU_DESCRIPTOR_HANDLE m_SceneSrvHandle;
	static CD3DX12_GPU_DESCRIPTOR_HANDLE m_PostProcessSrvHandle;
	static CD3DX12_GPU_DESCRIPTOR_HANDLE m_PreUpscaleAaSrvHandle;
	static CD3DX12_GPU_DESCRIPTOR_HANDLE m_PreUpscaleAaHistorySrvHandle;
	static CD3DX12_GPU_DESCRIPTOR_HANDLE m_EditorSceneSrvHandle;
	static CD3DX12_GPU_DESCRIPTOR_HANDLE m_EditorSceneUavHandle;
	static CD3DX12_GPU_DESCRIPTOR_HANDLE m_TransparentSceneSrvHandle;
	static ComPtr<ID3D12Resource> m_GBufferTargets[g_kGBUFFER_COUNT];
	static CD3DX12_CPU_DESCRIPTOR_HANDLE m_GBufferRtvHandles[g_kGBUFFER_COUNT];
	static CD3DX12_GPU_DESCRIPTOR_HANDLE m_GBufferSrvHandles[g_kGBUFFER_COUNT];
	static int m_EnvironmentTextureSrvIndex;
	static int m_MonitorTextureSrvIndex;
	static ComPtr<ID3D12RootSignature> m_PostProcessRootSignature;
	static ComPtr<ID3D12RootSignature> m_UpscaleRootSignature;
	static unordered_map<PostProcessType, ComPtr<ID3D12PipelineState>> m_PostProcessPsoMap;
	static ComPtr<ID3D12PipelineState> m_DeferredLightingPso;
	static ComPtr<ID3D12PipelineState> m_AtmospherePso;
	static ComPtr<ID3D12PipelineState> m_UpscaleBilateralPso;
	static ComPtr<ID3D12PipelineState> m_UpscaleDepthPso;
	static ComPtr<ID3D12PipelineState> m_VelocityPso;
	static ComPtr<ID3D12PipelineState> m_VelocityGeometryPso;
	static ComPtr<ID3D12Resource> m_PostProcessConstantBuffer;
	static ComPtr<ID3D12Resource> m_LightConstantBuffer;
	static ComPtr<ID3D12Resource> m_PBRConstantBuffer;
	static void* m_pPostProcessCbvDataBegin;
	static void* m_pLightCbvDataBegin;
	static void* m_pPBRCbvDataBegin;
	static RenderMode m_RenderMode;
	static RenderMode m_PendingRenderMode;
	static bool m_HasPendingRenderMode;
	static bool m_IsDeferredGeometryPass;
	static bool m_IsSceneColorForwardPass;
	static D3D12_RESOURCE_STATES m_DepthStencilState;

	static ComPtr<ID3D12RootSignature> m_SkinningRootSignature;
	static ComPtr<ID3D12PipelineState> m_SkinningPso;
	static DXGI_FORMAT m_SceneColorFormat;
	static DXGI_FORMAT m_BackBufferFormat;
	static bool m_AllowTearing;
	static bool m_HasPendingHdr;
	static bool m_PendingHdr;
public:
	static AntiAliasingMode m_AntiAliasingMode;
	static XMFLOAT4X4 m_PrevViewMatrix;
	static XMFLOAT4X4 m_PrevProjMatrix;
	static UINT m_TaaFrameIndex;
	static ComPtr<ID3D12RootSignature> m_AaRootSignature;
	static ComPtr<ID3D12PipelineState> m_FxaaPso;
	static ComPtr<ID3D12PipelineState> m_TaaBlendPso;

	static ComPtr<ID3D12Resource> m_AaRenderTarget;
	static CD3DX12_CPU_DESCRIPTOR_HANDLE m_AaRtvHandle;
	static CD3DX12_GPU_DESCRIPTOR_HANDLE m_AaSrvHandle;

	static constexpr float m_kSceneClearColor[4] = { 0.1f, 0.2f, 0.4f, 1.0f };
	static constexpr DXGI_FORMAT m_kDeferredRtvFormats[g_kGBUFFER_COUNT] =
	{
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R10G10B10A2_UNORM,
		DXGI_FORMAT_R32_FLOAT,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R32G32B32A32_UINT
	};
public:
	
	static constexpr uint32_t g_kSCREEN_WIDTH = 800;
	static constexpr uint32_t g_kSCREEN_HEIGHT = 600;
	static constexpr float g_kNEAR_CLIP = 0.1f;
	static constexpr float g_kFAR_CLIP = 100.0f;
	static constexpr float g_kDEFAULT_RESOLUTION_SCALE = 0.5f;


	static constexpr UINT g_kCB_ALIGNED_SIZE = (sizeof(ConstantBuffer3D) + 255) & ~255;
	static constexpr UINT g_kPP_CB_ALIGNED_SIZE = (sizeof(float) * 44 + 255) & ~255;
	static constexpr UINT g_kMAX_LOCAL_HEIGHT_FOG_VOLUMES = 16;
	static constexpr UINT g_kMAX_DISTANCE_FIELD_SHADOW_OBJECTS = 16;
	static constexpr UINT g_kLIGHT_CB_FLOAT4_COUNT =
		10 + g_kMAX_SHADER_LIGHTS * 10 + g_kMAX_VIRTUAL_SHADOW_LEVELS * 6 + 16 + 3 +
		g_kMAX_DISTANCE_FIELD_SHADOW_OBJECTS * 2 + 1 +
		g_kMAX_LOCAL_HEIGHT_FOG_VOLUMES * 3 + 1;
	static constexpr UINT g_kLIGHT_CB_ALIGNED_SIZE = (sizeof(float) * 4 * g_kLIGHT_CB_FLOAT4_COUNT + 255) & ~255;
	static constexpr UINT g_kPBR_CB_ALIGNED_SIZE = (sizeof(float) * 512 + 255) & ~255;
	static constexpr UINT g_kPBR_CB_SLOT_COUNT = g_kMAX_ENTITIES + 1;
	static constexpr UINT g_kPBR_CB_TOTAL_SLOT_COUNT = g_kPBR_CB_SLOT_COUNT * g_kFRAME_COUNT;
	static constexpr UINT g_kSHADOW_CB_ALIGNED_SIZE = (sizeof(XMMATRIX) + sizeof(float) * 8 + 255) & ~255;
	static constexpr UINT g_kSHADOW_CB_SLOT_COUNT = g_kFRAME_COUNT * g_kMAX_SHADOW_PASSES;

	static constexpr UINT g_kSHADOW_MAP_SIZE = 2048;
	static constexpr UINT g_kSHADOW_MAP_SIZE_SMALL = 1024;
	static constexpr UINT g_kVIRTUAL_SHADOW_PAGE_SIZE = 128;
	static constexpr UINT g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION = g_kSHADOW_MAP_SIZE / g_kVIRTUAL_SHADOW_PAGE_SIZE;
	static_assert(
		(g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION &
			(g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION - 1)) == 0,
		"VSM ring addressing requires a power-of-two physical page grid");
	static constexpr UINT g_kTRANSIENT_CB_SLOT_COUNT = 2048;
	static constexpr UINT g_kTRANSIENT_CB_START_INDEX = g_kMAX_ENTITIES;
	static constexpr UINT g_kCBV_PER_FRAME_COUNT = g_kMAX_ENTITIES + g_kTRANSIENT_CB_SLOT_COUNT;
	static constexpr UINT g_kCBV_COUNT = g_kCBV_PER_FRAME_COUNT * g_kFRAME_COUNT;
	static constexpr uint32_t g_kMAX_SRVS = 512;
	static constexpr UINT g_kTEXTURE_SRV_START_INDEX = g_kCBV_COUNT;
	static constexpr UINT g_kSCENE_SRV_INDEX = g_kTEXTURE_SRV_START_INDEX + g_kMAX_SRVS;
	static constexpr UINT g_kEDITOR_SCENE_SRV_INDEX = g_kSCENE_SRV_INDEX + 1;
	static constexpr UINT g_kGBUFFER_SRV_START_INDEX = g_kEDITOR_SCENE_SRV_INDEX + 1;
	static constexpr UINT g_kIMGUI_SRV_INDEX = g_kGBUFFER_SRV_START_INDEX + g_kGBUFFER_COUNT;
	static constexpr UINT g_kSHADOW_SRV_INDEX = g_kIMGUI_SRV_INDEX + 1;
	static constexpr UINT g_kAA_SRV_INDEX = g_kSHADOW_SRV_INDEX + 1;
	static constexpr UINT g_kAA_HISTORY_SRV_INDEX = g_kAA_SRV_INDEX + 1;
	static constexpr UINT g_kTRANSPARENT_SCENE_SRV_INDEX = g_kAA_HISTORY_SRV_INDEX + 1;
	static constexpr UINT g_kVELOCITY_CALCULATION_SRV_INDEX = g_kTRANSPARENT_SCENE_SRV_INDEX + 1;
	static constexpr UINT g_kDEPTH_SRV_INDEX = g_kVELOCITY_CALCULATION_SRV_INDEX + 1;
	static constexpr UINT g_kPOST_PROCESS_SRV_INDEX = g_kDEPTH_SRV_INDEX + 1;
	static constexpr UINT g_kPRE_UPSCALE_AA_SRV_INDEX = g_kPOST_PROCESS_SRV_INDEX + 1;
	static constexpr UINT g_kPRE_UPSCALE_AA_HISTORY_SRV_INDEX = g_kPRE_UPSCALE_AA_SRV_INDEX + 1;
	static constexpr UINT g_kEDITOR_SCENE_UAV_INDEX = g_kPRE_UPSCALE_AA_HISTORY_SRV_INDEX + 1;
	static constexpr UINT g_kFSR_SCRATCH_SRV_INDEX = g_kEDITOR_SCENE_UAV_INDEX + 1;
	static constexpr UINT g_kFSR_SCRATCH_UAV_INDEX = g_kFSR_SCRATCH_SRV_INDEX + 1;
	static constexpr UINT g_kNIS_COEF_SCALE_SRV_INDEX = g_kFSR_SCRATCH_UAV_INDEX + 1;
	static constexpr UINT g_kNIS_COEF_USM_SRV_INDEX = g_kNIS_COEF_SCALE_SRV_INDEX + 1;
	static constexpr UINT g_kGBUFFER_UAV_START_INDEX = g_kNIS_COEF_USM_SRV_INDEX + 1;
	static constexpr UINT g_kSSAO_SRV_INDEX = g_kGBUFFER_UAV_START_INDEX + g_kGEOMETRY_GBUFFER_COUNT;
	static constexpr UINT g_kSSGI_SRV_INDEX = g_kSSAO_SRV_INDEX + 1;
	static constexpr UINT g_kSS_HISTORY_SRV_INDEX = g_kSSGI_SRV_INDEX + 1;
	static constexpr UINT g_kSS_DEINTERLEAVED_DEPTH_SRV_INDEX = g_kSS_HISTORY_SRV_INDEX + 1;
	static constexpr UINT g_kSS_DEINTERLEAVED_NORMAL_SRV_INDEX = g_kSS_DEINTERLEAVED_DEPTH_SRV_INDEX + 1;
	static constexpr UINT g_kSS_DEINTERLEAVED_LIGHT_SRV_INDEX = g_kSS_DEINTERLEAVED_NORMAL_SRV_INDEX + 1;
	static constexpr UINT g_kSS_DEINTERLEAVED_GI_SRV_INDEX = g_kSS_DEINTERLEAVED_LIGHT_SRV_INDEX + 1;
	static constexpr UINT g_kSS_RAY_ORDER_SRV_INDEX = g_kSS_DEINTERLEAVED_GI_SRV_INDEX + 1;
	static constexpr UINT g_kSSAO_UAV_INDEX = g_kSS_RAY_ORDER_SRV_INDEX + 1;
	static constexpr UINT g_kSSGI_UAV_INDEX = g_kSSAO_UAV_INDEX + 1;
	static constexpr UINT g_kSS_DEINTERLEAVED_DEPTH_UAV_INDEX = g_kSSGI_UAV_INDEX + 1;
	static constexpr UINT g_kSS_DEINTERLEAVED_NORMAL_UAV_INDEX = g_kSS_DEINTERLEAVED_DEPTH_UAV_INDEX + 1;
	static constexpr UINT g_kSS_DEINTERLEAVED_LIGHT_UAV_INDEX = g_kSS_DEINTERLEAVED_NORMAL_UAV_INDEX + 1;
	static constexpr UINT g_kSS_DEINTERLEAVED_GI_UAV_INDEX = g_kSS_DEINTERLEAVED_LIGHT_UAV_INDEX + 1;
	static constexpr UINT g_kSS_RAY_ORDER_UAV_INDEX = g_kSS_DEINTERLEAVED_GI_UAV_INDEX + 1;
	static constexpr UINT g_kLOW_RES_DEPTH_SRV_INDEX = g_kSS_RAY_ORDER_UAV_INDEX + 1;
	static constexpr UINT g_kHIZ_SRV_START_INDEX = g_kLOW_RES_DEPTH_SRV_INDEX + 1;
	static constexpr UINT g_kHIZ_MAX_MIPS = 16;
	static constexpr UINT g_kHIZ_UAV_START_INDEX = g_kHIZ_SRV_START_INDEX + 2;
	static constexpr UINT g_kHIZ_MIP_SRV_START_INDEX = g_kHIZ_UAV_START_INDEX + 2 * g_kHIZ_MAX_MIPS;
	// Keep a stable engine-owned descriptor range for the advanced compute paths.
	static constexpr UINT g_kENGINE_DESCRIPTOR_END = g_kDEPTH_SRV_INDEX + 128;
	static_assert(g_kHIZ_MIP_SRV_START_INDEX + 2 * g_kHIZ_MAX_MIPS <= g_kENGINE_DESCRIPTOR_END + 1,
		"The Hi-Z descriptor range exceeds the engine-owned descriptor range.");
	static constexpr UINT g_kMAX_DYNAMIC_VERTICES = 65536;

	static DXGI_FORMAT GetSceneColorFormat() { return m_SceneColorFormat; }
	static DXGI_FORMAT GetBackBufferFormat() { return m_BackBufferFormat; }
	static DXGI_FORMAT GetForwardRtvFormat() { return (m_RenderMode == RenderMode::DEFERRED && !m_IsDeferredGeometryPass && !m_IsSceneColorForwardPass) ? m_BackBufferFormat : m_SceneColorFormat; }
	static bool IsHdrEnabled() { return m_SceneColorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT; }
	static DXGI_FORMAT GetGBufferFormat(GBufferType type) { return m_kDeferredRtvFormats[static_cast<UINT>(type)]; }



public:
	static ComPtr<ID3D12Resource> m_DynamicVertexBuffer;
	static D3D12_VERTEX_BUFFER_VIEW m_DynamicVertexBufferView;
	static Vertex* m_pDynamicVertexDataBegin;
	static UINT m_DynamicVertexOffset;
	static UINT m_TransientCbSlot;
};
