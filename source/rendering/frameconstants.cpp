#include "pch.h"
#include "frameconstants.h"

D3D12_GPU_DESCRIPTOR_HANDLE FrameConstants::AllocateTransientConstantBuffer(const ConstantBuffer3D& constants)
{
	UINT8* frameCbvDataBegin = GetConstantBufferPtr();
	if (!frameCbvDataBegin || !m_CbvHeap || m_TransientCbSlot >= g_kTRANSIENT_CB_SLOT_COUNT)
	{
		return {};
	}

	const UINT slot = g_kTRANSIENT_CB_START_INDEX + m_TransientCbSlot++;
	memcpy(frameCbvDataBegin + (slot * g_kCB_ALIGNED_SIZE), &constants, sizeof(constants));
	return GetConstantBufferHandle(slot);
}
