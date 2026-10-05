#pragma once
#include "rendererstate.h"

class FrameConstants : protected RendererState
{
public:
	using RendererState::g_kCB_ALIGNED_SIZE;
	using RendererState::g_kCBV_PER_FRAME_COUNT;
	using RendererState::g_kCBV_COUNT;
	using RendererState::g_kMAX_DYNAMIC_VERTICES;
	using RendererState::g_kMAX_SRVS;
	using RendererState::g_kTEXTURE_SRV_START_INDEX;
	using RendererState::g_kTRANSIENT_CB_SLOT_COUNT;
	using RendererState::g_kTRANSIENT_CB_START_INDEX;
	using RendererState::m_DynamicVertexBuffer;
	using RendererState::m_DynamicVertexBufferView;
	using RendererState::m_pDynamicVertexDataBegin;
	using RendererState::m_DynamicVertexOffset;

	static D3D12_GPU_DESCRIPTOR_HANDLE AllocateTransientConstantBuffer(const ConstantBuffer3D& constants);
	static ID3D12DescriptorHeap* GetCbvHeap() { return m_CbvHeap.Get(); }
	static UINT GetCurrentFrameCbvBaseIndex() { return m_FrameIndex * g_kCBV_PER_FRAME_COUNT; }
	static UINT GetCurrentFrameCbvIndex(UINT slot)
	{
		UINT safeSlot;
		if (slot < g_kCBV_PER_FRAME_COUNT)
		{
			safeSlot = slot;
		}
		else
		{
			safeSlot = g_kCBV_PER_FRAME_COUNT - 1;
		}
		return GetCurrentFrameCbvBaseIndex() + safeSlot;
	}
	static D3D12_GPU_DESCRIPTOR_HANDLE GetConstantBufferHandle(UINT slot)
	{
		if (!m_CbvHeap)
		{
			return {};
		}
		return CD3DX12_GPU_DESCRIPTOR_HANDLE(
			m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
			GetCurrentFrameCbvIndex(slot),
			m_CbvIncrementSize);
	}
	static UINT8* GetConstantBufferPtr()
	{
		if (!m_pCbvDataBegin)
		{
			return nullptr;
		}
		return m_pCbvDataBegin + GetCurrentFrameCbvBaseIndex() * g_kCB_ALIGNED_SIZE;
	}
	static UINT GetCbvIncrementSize() { return m_CbvIncrementSize; }
};
