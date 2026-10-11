#include "pch.h"
#include "meshshaderpipeline.h"
#include "dxccompiler.h"
#include "shaderpipelines.h"
#include "renderersettings.h"
#include "occlusionculling.h"
#include "rendertargets.h"
#include "meshdispatch.h"
#include "graphicslog.h"


	static ComPtr<ID3D12Device> g_Device;
	static ComPtr<ID3D12RootSignature> g_RootSignature;
	static ComPtr<ID3D12PipelineState> g_PipelineState;
	static ComPtr<ID3D12PipelineState> g_GeometryPipelineState;
	static MeshShaderPipeline::Statistics g_Statistics;
	static UINT g_PageLimit = MeshDispatch::GroupsPerPage;
	static bool g_FeatureSupported = false;

	template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
	struct alignas(void*) StreamSubobject
	{
		D3D12_PIPELINE_STATE_SUBOBJECT_TYPE TypeValue = Type;
		T Value{};
		StreamSubobject() = default;
		StreamSubobject(const T& value) : Value(value) {}
		StreamSubobject& operator=(const T& value) { Value = value; return *this; }
	};

	struct MeshPipelineStream
	{
		StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> RootSignature;
		StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> AmplificationShader;
		StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> MeshShader;
		StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> PixelShader;
		StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> Blend;
		StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> SampleMask;
		StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> Rasterizer;
		StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> DepthStencil;
		StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> Topology;
		StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> RenderTargets;
		StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> DepthFormat;
		StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> SampleDescription;
	};

	bool CompileShader(const wchar_t* path, const char* entry, const char* target, ComPtr<ID3DBlob>& output)
	{
		ComPtr<ID3DBlob> errors;
		UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
		flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
		flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
		HRESULT hr = DxcCompileFromFileCompat(
			path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
			entry, target, flags, 0, &output, &errors);
		if (errors && errors->GetBufferSize() > 1)
		{
			Debug::Log("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
			WriteGraphicsLog("Mesh shaders: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
		}
		return SUCCEEDED(hr) && output;
	}

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		memcpy(&bits, &value, sizeof(bits));
		return bits;
	}


bool MeshShaderPipeline::Initialize(ID3D12Device* device, ID3D12RootSignature* rootSignature)
{
	Shutdown();
	if (!device || !rootSignature)
	{
		WriteGraphicsLog("Mesh shaders: missing device or root signature\n");
		return false;
	}
	g_Device = device;
	g_RootSignature = rootSignature;
	char validationLimit[16]{};
	const DWORD length = GetEnvironmentVariableA("DX12_VALIDATION_MESH_PAGE_LIMIT", validationLimit, sizeof(validationLimit));
	g_PageLimit = length > 0 && length < sizeof(validationLimit) ?
		std::clamp(static_cast<UINT>(strtoul(validationLimit, nullptr, 10)), 1u, MeshDispatch::GroupsPerPage) : MeshDispatch::GroupsPerPage;
	D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
	g_FeatureSupported = SUCCEEDED(device->CheckFeatureSupport(
		D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))) &&
		options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
	WriteGraphicsLog("Mesh shaders: hardware tier=%u compiler=%u\n", static_cast<UINT>(options7.MeshShaderTier), static_cast<UINT>(DxcRuntimeCompilerIsAvailable()));
	if (!g_FeatureSupported || !DxcRuntimeCompilerIsAvailable()) return false;

	ComPtr<ID3DBlob> amplificationShader;
	ComPtr<ID3DBlob> meshShader;
	ComPtr<ID3DBlob> pixelShader;
	if (!CompileShader(L"shader/hlsl/MeshVisibility.hlsl", "ASMain", "as_6_5", amplificationShader) ||
		!CompileShader(L"shader/hlsl/MeshVisibility.hlsl", "MSMain", "ms_6_5", meshShader) ||
		!CompileShader(L"shader/hlsl/MeshVisibilityPS.hlsl", "PSMain", "ps_6_0", pixelShader))
	{
		g_FeatureSupported = false;
		WriteGraphicsLog("Mesh shaders: shader compilation failed\n");
		return false;
	}

	D3D12_RT_FORMAT_ARRAY renderTargets{};
	renderTargets.NumRenderTargets = 1;
	renderTargets.RTFormats[0] = DXGI_FORMAT_R32G32B32A32_UINT;
	MeshPipelineStream stream{};
	stream.RootSignature = rootSignature;
	stream.AmplificationShader = D3D12_SHADER_BYTECODE{ amplificationShader->GetBufferPointer(), amplificationShader->GetBufferSize() };
	stream.MeshShader = D3D12_SHADER_BYTECODE{ meshShader->GetBufferPointer(), meshShader->GetBufferSize() };
	stream.PixelShader = D3D12_SHADER_BYTECODE{ pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
	stream.Blend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	stream.SampleMask = UINT_MAX;
	stream.Rasterizer = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	stream.DepthStencil = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	stream.Topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	stream.RenderTargets = renderTargets;
	stream.DepthFormat = DXGI_FORMAT_D32_FLOAT;
	stream.SampleDescription = DXGI_SAMPLE_DESC{ 1, 0 };

	D3D12_PIPELINE_STATE_STREAM_DESC streamDescription{};
	streamDescription.SizeInBytes = sizeof(stream);
	streamDescription.pPipelineStateSubobjectStream = &stream;
	ComPtr<ID3D12Device2> device2;
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device2))) ||
		FAILED(device2->CreatePipelineState(&streamDescription, IID_PPV_ARGS(&g_PipelineState))))
	{
		g_FeatureSupported = false;
		WriteGraphicsLog("Mesh shaders: visibility pipeline creation failed\n");
		return false;
	}
	if (!CompileShader(L"shader/hlsl/GeometryPS.hlsl", "main", "ps_6_0", pixelShader)) return false;
	stream.PixelShader = D3D12_SHADER_BYTECODE{ pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
	renderTargets.NumRenderTargets = RendererState::g_kGEOMETRY_GBUFFER_COUNT;
	for (UINT index = 0; index < renderTargets.NumRenderTargets; ++index)
		renderTargets.RTFormats[index] = RenderTargets::GetGBufferFormat(static_cast<GBufferType>(index));
	stream.RenderTargets = renderTargets;
	if (FAILED(device2->CreatePipelineState(&streamDescription, IID_PPV_ARGS(&g_GeometryPipelineState)))) return false;
	WriteGraphicsLog("Mesh shaders: visibility and MRT pipelines ready\n");
	return true;
}

void MeshShaderPipeline::Shutdown()
{
	g_PipelineState.Reset();
	g_GeometryPipelineState.Reset();
	g_RootSignature.Reset();
	g_Device.Reset();
	g_FeatureSupported = false;
}

bool MeshShaderPipeline::IsSupported()
{
	return IsAvailable() && RendererSettings::GetMeshShadersEnabled();
}
bool MeshShaderPipeline::IsAvailable() { return g_FeatureSupported && g_PipelineState && g_GeometryPipelineState; }

void MeshShaderPipeline::BeginFrame() { g_Statistics = {}; }
const MeshShaderPipeline::Statistics& MeshShaderPipeline::GetStatistics() { return g_Statistics; }

bool MeshShaderPipeline::Draw(
	ID3D12GraphicsCommandList* commandList,
	const StaticMeshData& mesh,
	const XMFLOAT3& localCenter,
	const XMFLOAT3& localExtents)
{
	Geometry geometry;
	geometry.Vertices = mesh.VertexBuffer.Get();
	geometry.Indices = mesh.IndexBuffer.Get();
	geometry.VertexView = mesh.VertexBufferView;
	geometry.IndexView = mesh.IndexBufferView;
	geometry.ElementCount = mesh.IndexCount;
	return Draw(commandList, geometry, localCenter, localExtents);
}

bool MeshShaderPipeline::Draw(ID3D12GraphicsCommandList* commandList, const Geometry& geometry,
	const XMFLOAT3& localCenter, const XMFLOAT3& localExtents)
{
	if (!commandList || !IsSupported() || !geometry.Vertices || geometry.ElementCount < 3 ||
		geometry.VertexView.StrideInBytes != 48 ||
		(geometry.Indices && geometry.IndexView.Format != DXGI_FORMAT_R16_UINT && geometry.IndexView.Format != DXGI_FORMAT_R32_UINT))
	{
		return false;
	}
	ComPtr<ID3D12GraphicsCommandList6> meshCommandList;
	if (FAILED(commandList->QueryInterface(IID_PPV_ARGS(&meshCommandList)))) return false;

	std::vector<D3D12_RESOURCE_BARRIER> barriers;
	const auto addBarrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
	{
		D3D12_HEAP_PROPERTIES heap{};
		if (resource && SUCCEEDED(resource->GetHeapProperties(&heap, nullptr)) && heap.Type == D3D12_HEAP_TYPE_UPLOAD) return;
		if (resource && state != D3D12_RESOURCE_STATE_GENERIC_READ && state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	};
	addBarrier(geometry.Vertices, geometry.VertexState);
	addBarrier(geometry.Indices, geometry.IndexState);
	if (!barriers.empty()) commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
	commandList->SetPipelineState(RendererSettings::GetComputeGBufferEnabled() ? g_PipelineState.Get() : g_GeometryPipelineState.Get());
	commandList->SetGraphicsRootShaderResourceView(11, geometry.VertexView.BufferLocation);
	commandList->SetGraphicsRootShaderResourceView(12, geometry.Indices ? geometry.IndexView.BufferLocation : geometry.VertexView.BufferLocation);
	uint32_t constants[16] =
	{
		geometry.ElementCount,
		geometry.Indices ? static_cast<UINT>(geometry.IndexView.Format == DXGI_FORMAT_R16_UINT) : 2u,
		OcclusionCulling::GetPhase(),
		static_cast<UINT>(OcclusionCulling::HasPrevious()),
		static_cast<UINT>(OcclusionCulling::HasCurrent()),
		OcclusionCulling::GetWidth(),
		OcclusionCulling::GetHeight(),
		OcclusionCulling::GetMipCount(),
		FloatBits(localCenter.x), FloatBits(localCenter.y), FloatBits(localCenter.z), FloatBits(1.0f),
		FloatBits(localExtents.x), FloatBits(localExtents.y), FloatBits(localExtents.z), 0u
	};
	commandList->SetGraphicsRoot32BitConstants(13, 16, constants, 0);
	commandList->SetGraphicsRootDescriptorTable(14, OcclusionCulling::GetPreviousSrv());
	commandList->SetGraphicsRootDescriptorTable(15, OcclusionCulling::GetCurrentSrv());
	const UINT meshletCount = MeshDispatch::MeshletCount(geometry.ElementCount);
	for (UINT first = 0; first < meshletCount;)
	{
		const UINT count = MeshDispatch::PageSize(meshletCount, first, g_PageLimit);
		commandList->SetGraphicsRoot32BitConstants(13, 1, &first, 15);
		meshCommandList->DispatchMesh(count, 1, 1);
		++g_Statistics.Pages;
		g_Statistics.MaxGroupsPerPage = (std::max)(g_Statistics.MaxGroupsPerPage, count);
		first += count;
	}
	++g_Statistics.Draws;
	g_Statistics.Meshlets += meshletCount;
	for (auto& barrier : barriers) swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	if (!barriers.empty()) commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
	return true;
}
