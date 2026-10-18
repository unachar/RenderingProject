#pragma once
// =============================================================================================
// AsyncCompute
//   専用の COMPUTE コマンドキューで、グラフィックスと並行して実行できる処理を流す仕組み。
//   ・フレームスロットごとのアロケーター（スロット再利用時は前回の GPU 処理が完了済み）
//   ・Submit() で compute キューに投入して Signal し、グラフィックスキューに GPU 側の Wait を挿入
//     （CPU は待たない）。以降にグラフィックスキューへ投入されたコマンドは compute 完了後に実行される。
//   ・キューを作成できない環境では IsAvailable() が false になり、呼び出し側はグラフィックス
//     コマンドリストに記録する（フォールバック）。
//   ヘッダーのみで完結（.vcxproj への追加不要）。
// =============================================================================================
#include "pch.h"
#include "rendererstate.h"
#include "graphicslog.h"

class AsyncCompute final
{
  public:
    static bool Initialize(ID3D12Device* device)
    {
        Shutdown();
        if (!device)
        {
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&s_Queue))))
        {
            WriteGraphicsLog("AsyncCompute: compute queue unavailable, using the graphics queue\n");
            return false;
        }
        s_Queue->SetName(L"Async Compute Queue");
        for (auto& allocator : s_Allocators)
        {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator))))
            {
                Shutdown();
                return false;
            }
        }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, s_Allocators[0].Get(), nullptr,
                                             IID_PPV_ARGS(&s_List))) ||
            FAILED(s_List->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_Fence))))
        {
            Shutdown();
            return false;
        }
        s_List->SetName(L"Async Compute List");
        s_Event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        s_FenceValue = 0;
        s_Available = s_Event != nullptr;
        if (s_Available)
        {
            WriteGraphicsLog("AsyncCompute: dedicated compute queue ready\n");
        }
        return s_Available;
    }

    static void Shutdown()
    {
        WaitIdle();
        s_Available = false;
        s_Recording = false;
        s_List.Reset();
        for (auto& allocator : s_Allocators)
        {
            allocator.Reset();
        }
        s_Fence.Reset();
        s_Queue.Reset();
        if (s_Event)
        {
            CloseHandle(s_Event);
            s_Event = nullptr;
        }
    }

    // Usable this frame (queue exists and the user has not disabled it).
    static bool IsActive()
    {
        return s_Available && s_Enabled;
    }

    static bool IsAvailable()
    {
        return s_Available;
    }

    static bool& Enabled()
    {
        return s_Enabled;
    }

    static UINT64 GetSubmittedCount()
    {
        return s_FenceValue;
    }

    // The slot's allocator is reused only after RenderFrame waited for that slot's graphics
    // fence, and that graphics frame waited for its compute work: the allocator is idle.
    static ID3D12GraphicsCommandList* Begin(UINT frameSlot)
    {
        if (!IsActive() || s_Recording)
        {
            return nullptr;
        }
        auto& allocator = s_Allocators[frameSlot % kFrames];
        if (FAILED(allocator->Reset()) || FAILED(s_List->Reset(allocator.Get(), nullptr)))
        {
            return nullptr;
        }
        s_Recording = true;
        return s_List.Get();
    }

    // Executes the recorded work on the compute queue and makes the graphics queue wait for it
    // on the GPU timeline. Returns false if nothing was submitted.
    static bool Submit(ID3D12CommandQueue* graphicsQueue)
    {
        if (!s_Recording)
        {
            return false;
        }
        s_Recording = false;
        if (FAILED(s_List->Close()))
        {
            return false;
        }
        ID3D12CommandList* lists[] = {s_List.Get()};
        s_Queue->ExecuteCommandLists(1, lists);
        ++s_FenceValue;
        if (FAILED(s_Queue->Signal(s_Fence.Get(), s_FenceValue)))
        {
            return false;
        }
        if (graphicsQueue)
        {
            graphicsQueue->Wait(s_Fence.Get(), s_FenceValue);
        }
        return true;
    }

    static void WaitIdle()
    {
        if (!s_Fence || !s_Event || s_Fence->GetCompletedValue() >= s_FenceValue)
        {
            return;
        }
        if (SUCCEEDED(s_Fence->SetEventOnCompletion(s_FenceValue, s_Event)))
        {
            WaitForSingleObject(s_Event, INFINITE);
        }
    }

  private:
    static constexpr UINT kFrames = RendererState::g_kFRAME_COUNT;
    static inline ComPtr<ID3D12CommandQueue> s_Queue;
    static inline ComPtr<ID3D12CommandAllocator> s_Allocators[kFrames];
    static inline ComPtr<ID3D12GraphicsCommandList> s_List;
    static inline ComPtr<ID3D12Fence> s_Fence;
    static inline HANDLE s_Event = nullptr;
    static inline UINT64 s_FenceValue = 0;
    static inline bool s_Available = false;
    static inline bool s_Enabled = true;
    static inline bool s_Recording = false;
};
