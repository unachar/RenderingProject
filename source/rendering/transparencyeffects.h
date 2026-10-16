#pragma once
// =============================================================================================
// TransparencyEffects
//   ・Weighted Blended OIT（McGuire & Bavoil 2013）
//       透明パス中は (Scene, Accum RGBA16F 加算, Revealage R16F 乗算) の 3 枚をバインドし、
//       modelshaderPS は OIT の 2 枚へ、その他のシェーダーは従来通りシーンへ描画する。
//       パス終了時に 1 回の全画面合成でシーンへ重ねる。描画順に依存しない。
//   ・RT 屈折のための不透明シーンのコピー（t3: SceneColorTexture）
//       透明パス開始時に不透明シーンをコピーし、modelshaderPS が RayQuery で求めた
//       屈折先の位置を投影して参照する。
//   OIT は HDR（シーン形式 RGBA16F）時のみ有効。ヘッダーのみ（.vcxproj 追加不要）。
// =============================================================================================
#include "pch.h"
#include "rendererstate.h"
#include "renderquality.h"
#include "graphicslog.h"
#include "graphicsdevice.h"
#include "raytracing.h"

class TransparencyEffects final
{
public:
	static bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* engineHeap)
	{
		Shutdown();
		if (!device || !engineHeap) return false;
		s_Device = device;
		s_EngineHeap = engineHeap;
		s_Increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		D3D12_DESCRIPTOR_HEAP_DESC rtv{};
		rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtv.NumDescriptors = 2;
		if (FAILED(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&s_RtvHeap)))) return Fail("RTV heap");

		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
		CD3DX12_ROOT_PARAMETER parameter;
		parameter.InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);
		const CD3DX12_ROOT_SIGNATURE_DESC root(1, &parameter, 0, nullptr);
		ComPtr<ID3DBlob> blob, errors;
		if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
			FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s_Root))))
			return Fail("root signature");
		ComPtr<ID3DBlob> vs, ps;
		if (FAILED(D3DCompileFromFile(L"shader/hlsl/OitCompositePS.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
				"FullscreenVS", "vs_6_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors)) ||
			FAILED(D3DCompileFromFile(L"shader/hlsl/OitCompositePS.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
				"CompositePS", "ps_6_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors)))
		{
			WriteGraphicsLog("TransparencyEffects: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
			return Fail("shader compile");
		}
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = s_Root.Get();
		desc.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
		desc.PS = CD3DX12_SHADER_BYTECODE(ps.Get());
		desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		auto& blend = desc.BlendState.RenderTarget[0];
		blend.BlendEnable = TRUE;
		blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;       // average colour * (1 - revealage)
		blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;  // background * revealage
		blend.BlendOp = D3D12_BLEND_OP_ADD;
		blend.SrcBlendAlpha = D3D12_BLEND_ONE;
		blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		desc.DepthStencilState.DepthEnable = FALSE;
		desc.DepthStencilState.StencilEnable = FALSE;
		desc.SampleMask = UINT_MAX;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&s_CompositePso)))) return Fail("composite PSO");
		s_Available = true;
		WriteGraphicsLog("TransparencyEffects: weighted blended OIT ready\n");
		return true;
	}

	static void Shutdown()
	{
		s_Available = false;
		s_OitPass = false;
		s_Accum.Reset(); s_Reveal.Reset(); s_RtvHeap.Reset(); s_Root.Reset(); s_CompositePso.Reset();
		s_Device.Reset(); s_EngineHeap.Reset();
		s_Width = s_Height = 0;
	}

	// Read by PsoManager while the transparent pass records draws.
	static bool IsOitPassActive() { return s_OitPass; }
	static DXGI_FORMAT GetRevealFormat() { return DXGI_FORMAT_R16_FLOAT; }
	static bool IsOitActive()
	{
		return s_Available && RenderQuality::Get().OrderIndependentTransparency &&
			RendererState::GetSceneColorFormat() == DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
	static bool IsRefractionActive() { return RenderQuality::Get().RaytracedRefraction; }

	// Called with the scene target bound as RENDER_TARGET. Returns true if the OIT targets
	// were bound (the caller must not rebind the scene target).
	static bool BeginTransparentPass(ID3D12GraphicsCommandList* commands, ID3D12Resource* scene,
		D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv, ID3D12Resource* sceneCopy, D3D12_CPU_DESCRIPTOR_HANDLE dsv)
	{
		s_OitPass = false;
		if (!commands || !scene) return false;
		// Opaque scene for refraction (same extent and format as the scene target).
		if (IsRefractionActive() && sceneCopy && sceneCopy->GetDesc().Width == scene->GetDesc().Width &&
			sceneCopy->GetDesc().Height == scene->GetDesc().Height && sceneCopy->GetDesc().Format == scene->GetDesc().Format)
		{
			D3D12_RESOURCE_BARRIER toCopy[] = {
				CD3DX12_RESOURCE_BARRIER::Transition(scene, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(sceneCopy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST) };
			commands->ResourceBarrier(2, toCopy);
			commands->CopyResource(sceneCopy, scene);
			D3D12_RESOURCE_BARRIER back[] = {
				CD3DX12_RESOURCE_BARRIER::Transition(scene, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
				CD3DX12_RESOURCE_BARRIER::Transition(sceneCopy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) };
			commands->ResourceBarrier(2, back);
		}
		if (!IsOitActive() || !EnsureTargets(static_cast<UINT>(scene->GetDesc().Width), scene->GetDesc().Height)) return false;
		D3D12_RESOURCE_BARRIER toTarget[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(s_Accum.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
			CD3DX12_RESOURCE_BARRIER::Transition(s_Reveal.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET) };
		commands->ResourceBarrier(2, toTarget);
		const float accumClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		const float revealClear[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = { sceneRtv, Rtv(0), Rtv(1) };
		commands->ClearRenderTargetView(rtvs[1], accumClear, 0, nullptr);
		commands->ClearRenderTargetView(rtvs[2], revealClear, 0, nullptr);
		// RT0 scene (non OIT shaders), RT1 accumulation, RT2 revealage.
		commands->OMSetRenderTargets(3, rtvs, FALSE, &dsv);
		s_OitPass = true;
		return true;
	}

	// Composites the OIT targets over the scene target (left bound as RENDER_TARGET).
	static void EndTransparentPass(ID3D12GraphicsCommandList* commands, D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv,
		const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor)
	{
		if (!s_OitPass || !commands) return;
		s_OitPass = false;
		D3D12_RESOURCE_BARRIER toRead[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(s_Accum.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(s_Reveal.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) };
		commands->ResourceBarrier(2, toRead);
		commands->OMSetRenderTargets(1, &sceneRtv, FALSE, nullptr);
		commands->RSSetViewports(1, &viewport);
		commands->RSSetScissorRects(1, &scissor);
		ID3D12DescriptorHeap* heaps[] = { s_EngineHeap.Get() };
		commands->SetDescriptorHeaps(1, heaps);
		commands->SetGraphicsRootSignature(s_Root.Get());
		commands->SetPipelineState(s_CompositePso.Get());
		commands->SetGraphicsRootDescriptorTable(0, CD3DX12_GPU_DESCRIPTOR_HANDLE(
			s_EngineHeap->GetGPUDescriptorHandleForHeapStart(), kSrvIndex, s_Increment));
		commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		commands->DrawInstanced(3, 1, 0, 0);
	}

private:
	static constexpr UINT kSrvIndex = RendererState::g_kQUALITY_DESCRIPTOR_START + 3; // accum, reveal

	static inline bool s_Available = false, s_OitPass = false;
	static inline UINT s_Increment = 0, s_Width = 0, s_Height = 0;
	static inline ComPtr<ID3D12Device> s_Device;
	static inline ComPtr<ID3D12DescriptorHeap> s_EngineHeap, s_RtvHeap;
	static inline ComPtr<ID3D12RootSignature> s_Root;
	static inline ComPtr<ID3D12PipelineState> s_CompositePso;
	static inline ComPtr<ID3D12Resource> s_Accum, s_Reveal;

	static bool Fail(const char* what)
	{
		WriteGraphicsLog("TransparencyEffects: %s failed, OIT disabled\n", what);
		Shutdown();
		return false;
	}

	static D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT index)
	{
		return CD3DX12_CPU_DESCRIPTOR_HANDLE(s_RtvHeap->GetCPUDescriptorHandleForHeapStart(), index,
			s_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
	}

	static bool CreateTarget(ComPtr<ID3D12Resource>& target, DXGI_FORMAT format, UINT width, UINT height,
		const float* clear, UINT index, const wchar_t* name)
	{
		const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
		D3D12_CLEAR_VALUE value{};
		value.Format = format;
		memcpy(value.Color, clear, sizeof(value.Color));
		if (FAILED(s_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			&value, IID_PPV_ARGS(target.ReleaseAndGetAddressOf())))) return false;
		target->SetName(name);
		s_Device->CreateRenderTargetView(target.Get(), nullptr, Rtv(index));
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = format;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		s_Device->CreateShaderResourceView(target.Get(), &srv, CD3DX12_CPU_DESCRIPTOR_HANDLE(
			s_EngineHeap->GetCPUDescriptorHandleForHeapStart(), kSrvIndex + index, s_Increment));
		return true;
	}

	static bool EnsureTargets(UINT width, UINT height)
	{
		if (s_Accum && s_Width == width && s_Height == height) return true;
		if (s_Accum && !GraphicsDevice::WaitForGpuIdle()) return false;
		const float accumClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		const float revealClear[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		if (!CreateTarget(s_Accum, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, accumClear, 0, L"OIT Accumulation") ||
			!CreateTarget(s_Reveal, GetRevealFormat(), width, height, revealClear, 1, L"OIT Revealage"))
		{
			s_Accum.Reset(); s_Reveal.Reset();
			return false;
		}
		s_Width = width; s_Height = height;
		return true;
	}
};
