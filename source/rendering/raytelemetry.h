#pragma once
#include "raytelemetrydata.h"
#include <d3d12.h>
#include <wrl.h>
#include <cstring>

namespace RayTelemetry
{
    class Collector final
    {
      public:
        static constexpr UINT FrameCount = 3;
        static constexpr UINT Bytes = RAY_TELEMETRY_CHANNEL_COUNT * 2 * sizeof(uint32_t);

        bool BeginFrame(ID3D12Device* device, ID3D12GraphicsCommandList* commands, uint64_t serial, UINT slot,
                        bool requested)
        {
            m_Collecting = false;
            if (slot >= FrameCount || !device || !commands)
            {
                return false;
            }
            if (m_Device != device)
            {
                m_Frames = {};
                m_Zero.Reset();
                m_Device = device;
            }
            m_Slot = slot;
            auto& frame = m_Frames[slot];
            frame.Serial = serial;
            frame.Requested = requested;
            frame.Submitted = false;
            frame.Active = frame.Measured = 0;
            if (!requested)
            {
                return false;
            }
            if (!frame.Buffer &&
                !CreateBuffer(device, frame.Buffer, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            {
                return false;
            }
            if (!frame.Readback &&
                !CreateBuffer(device, frame.Readback, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST))
            {
                return false;
            }
            if (!m_Zero)
            {
                if (!CreateBuffer(device, m_Zero, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ))
                {
                    return false;
                }
                void* mapped = nullptr;
                const D3D12_RANGE noRead{0, 0};
                if (FAILED(m_Zero->Map(0, &noRead, &mapped)))
                {
                    m_Zero.Reset();
                    return false;
                }
                const std::array<uint32_t, RAY_TELEMETRY_CHANNEL_COUNT * 2> zero{};
                std::memcpy(mapped, zero.data(), Bytes);
                m_Zero->Unmap(0, nullptr);
            }
            Transition(commands, frame.Buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            commands->CopyBufferRegion(frame.Buffer.Get(), 0, m_Zero.Get(), 0, Bytes);
            Transition(commands, frame.Buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_Collecting = true;
            return true;
        }

        bool IsCollecting() const
        {
            return m_Collecting;
        }

        D3D12_GPU_VIRTUAL_ADDRESS Address() const
        {
            return m_Collecting ? m_Frames[m_Slot].Buffer->GetGPUVirtualAddress() : 0;
        }

        void RecordProvider(uint32_t provider, bool instrumented)
        {
            if (!m_Collecting)
            {
                return;
            }
            auto& frame = m_Frames[m_Slot];
            frame.Active |= provider;
            if (instrumented)
            {
                frame.Measured |= provider;
            }
        }

        void EndFrame(ID3D12GraphicsCommandList* commands)
        {
            if (!m_Collecting || !commands)
            {
                return;
            }
            auto& frame = m_Frames[m_Slot];
            Transition(commands, frame.Buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->CopyBufferRegion(frame.Readback.Get(), 0, frame.Buffer.Get(), 0, Bytes);
            Transition(commands, frame.Buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            frame.Submitted = true;
            m_Collecting = false;
        }

        // 呼び出し前に、描画側のフレームスロットフェンスが完了している必要がある。
        Counts Consume(UINT slot, uint64_t serial) const
        {
            Counts unavailable;
            unavailable.Serial = serial;
            if (slot >= FrameCount)
            {
                return unavailable;
            }
            const auto& frame = m_Frames[slot];
            unavailable.Requested = frame.Requested;
            if (!frame.Submitted || !frame.Requested || frame.Serial != serial)
            {
                return unavailable;
            }
            void* mapped = nullptr;
            const D3D12_RANGE read{0, Bytes};
            if (FAILED(frame.Readback->Map(0, &read, &mapped)) || !mapped)
            {
                return unavailable;
            }
            std::array<uint32_t, RAY_TELEMETRY_CHANNEL_COUNT * 2> words;
            std::memcpy(words.data(), mapped, Bytes);
            const D3D12_RANGE noWrites{0, 0};
            frame.Readback->Unmap(0, &noWrites);
            return Decode(words, frame.Serial, serial, frame.Requested, frame.Active, frame.Measured);
        }

        Counts ConsumeCurrent(uint64_t serial) const
        {
            return Consume(m_Slot, serial);
        }

        void Shutdown()
        {
            m_Frames = {};
            m_Zero.Reset();
            m_Device = nullptr;
            m_Collecting = false;
        }

      private:
        struct Frame
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> Buffer, Readback;
            uint64_t Serial = 0;
            uint32_t Active = 0, Measured = 0;
            bool Requested = false, Submitted = false;
        };

        std::array<Frame, FrameCount> m_Frames;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_Zero;
        ID3D12Device* m_Device = nullptr;
        UINT m_Slot = 0;
        bool m_Collecting = false;

        static bool CreateBuffer(ID3D12Device* device, Microsoft::WRL::ComPtr<ID3D12Resource>& buffer,
                                 D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = type;
            heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = Bytes;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = flags;
            return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                             IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf())));
        }

        static void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                               D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commands->ResourceBarrier(1, &barrier);
        }
    };

    inline Collector& Data()
    {
        static Collector collector;
        return collector;
    }

    inline bool& Requested()
    {
        static bool enabled = false;
        return enabled;
    }

    inline void SetEnabled(bool enabled)
    {
        Requested() = enabled;
    }

    inline bool IsCollecting()
    {
        return Data().IsCollecting();
    }
} // 名前空間 RayTelemetry
