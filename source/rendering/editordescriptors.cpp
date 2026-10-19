#include "pch.h"
#include "editordescriptors.h"
#include "editordescriptorpool.h"
#include "frameconstants.h"
#include "graphicsdevice.h"

namespace
{
    EditorDescriptorPool pool(RendererState::g_kTHUMBNAIL_DESCRIPTOR_COUNT);
}

EditorDescriptorLease::~EditorDescriptorLease()
{
    EditorDescriptors::Retire(Index);
}

std::shared_ptr<EditorDescriptorLease> EditorDescriptors::Allocate()
{
    const auto slot = pool.Allocate();
    if (!slot)
    {
        return {};
    }
    auto lease = std::make_shared<EditorDescriptorLease>();
    lease->Index = RendererState::g_kTHUMBNAIL_DESCRIPTOR_START + *slot;
    const auto heap = FrameConstants::GetCbvHeap();
    lease->Cpu = CD3DX12_CPU_DESCRIPTOR_HANDLE(heap->GetCPUDescriptorHandleForHeapStart(), lease->Index,
                                               FrameConstants::GetCbvIncrementSize());
    lease->Gpu = CD3DX12_GPU_DESCRIPTOR_HANDLE(heap->GetGPUDescriptorHandleForHeapStart(), lease->Index,
                                               FrameConstants::GetCbvIncrementSize());
    return lease;
}

void EditorDescriptors::Retire(UINT index)
{
    pool.Retire(index - RendererState::g_kTHUMBNAIL_DESCRIPTOR_START, GraphicsDevice::GetPendingSubmissionFence());
}

void EditorDescriptors::Collect()
{
    pool.Collect(GraphicsDevice::GetCompletedFence());
}

UINT EditorDescriptors::Available()
{
    return pool.Available();
}

void EditorDescriptors::Shutdown()
{
    pool = EditorDescriptorPool(RendererState::g_kTHUMBNAIL_DESCRIPTOR_COUNT);
}
