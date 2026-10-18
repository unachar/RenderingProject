#pragma once
#include "pch.h"
#include "rendergraph.h"
// Conservative mixed-queue executor. Passes follow the graph's topological
// order, with COMMON ownership transfers and GPU fence waits. It intentionally
// serializes dependent and independent passes; it does not claim GPU overlap.
class RenderGraphQueueExecutor final
{
    struct Batch { ComPtr<ID3D12CommandAllocator>Allocator;ComPtr<ID3D12GraphicsCommandList>List;bool Compute=false; };
    struct Frame { std::vector<Batch>Batches;UINT64 Fence=0; };
public:
    bool Initialize(ID3D12Device* device,ID3D12CommandQueue* graphics)
    {
        Shutdown();if(!device||!graphics)return false;m_Device=device;m_Graphics=graphics;
        D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;
        device->CreateCommandQueue(&desc,IID_PPV_ARGS(&m_Compute));
        if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&m_Fence))))return false;
        m_Event=CreateEvent(nullptr,FALSE,FALSE,nullptr);return m_Event!=nullptr;
    }
    void WaitIdle(){if(m_Fence&&m_Last&&m_Fence->GetCompletedValue()<m_Last&&m_Event&&SUCCEEDED(m_Fence->SetEventOnCompletion(m_Last,m_Event)))WaitForSingleObject(m_Event,INFINITE);}
    void Shutdown(){WaitIdle();for(auto& f:m_Frames)f={};m_Compute.Reset();m_Graphics.Reset();m_Fence.Reset();m_Device.Reset();if(m_Event)CloseHandle(m_Event);m_Event=nullptr;m_Last=0;}
    ~RenderGraphQueueExecutor(){Shutdown();}
    bool Execute(RenderGraph& graph,UINT slot)
    {
        if(!m_Device||!m_Fence||!graph.m_IsCompiled)return false;
        auto& frame=m_Frames[slot%3];
        if(frame.Fence&&m_Fence->GetCompletedValue()<frame.Fence)
        {if(FAILED(m_Fence->SetEventOnCompletion(frame.Fence,m_Event)))return false;WaitForSingleObject(m_Event,INFINITE);}
        frame.Batches.clear();
        auto record=[&](bool compute,const std::function<void(ID3D12GraphicsCommandList*)>& callback)
        {
            Batch batch;batch.Compute=compute&&bool(m_Compute);const auto type=batch.Compute?D3D12_COMMAND_LIST_TYPE_COMPUTE:D3D12_COMMAND_LIST_TYPE_DIRECT;
            if(FAILED(m_Device->CreateCommandAllocator(type,IID_PPV_ARGS(&batch.Allocator)))||FAILED(m_Device->CreateCommandList(0,type,batch.Allocator.Get(),nullptr,IID_PPV_ARGS(&batch.List))))return false;
            callback(batch.List.Get());if(FAILED(batch.List->Close()))return false;frame.Batches.push_back(std::move(batch));return true;
        };
        auto transition=[](ID3D12GraphicsCommandList* list,const RenderGraph::ResourceRecord& resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
        {if(resource.Resource&&resource.AutomaticBarriers&&before!=after){const auto b=CD3DX12_RESOURCE_BARRIER::Transition(resource.Resource,before,after,resource.Subresource);list->ResourceBarrier(1,&b);}};
        // Preflight restrictions before recording or submitting any batch.
        for(const auto& resource:graph.m_Resources)
            if(resource.Resource&&resource.AutomaticBarriers&&resource.HasFinalState&&resource.Resource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&resource.FinalState!=D3D12_RESOURCE_STATE_COMMON)return false;
        constexpr UINT forbidden=D3D12_RESOURCE_STATE_RENDER_TARGET|D3D12_RESOURCE_STATE_DEPTH_WRITE|D3D12_RESOURCE_STATE_DEPTH_READ|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PRESENT;
        for(const auto& pass:graph.m_Passes)if(pass.QueueType==RenderGraph::Queue::Compute&&m_Compute)
            for(const auto& usage:pass.Usages)if(UINT(usage.State)&forbidden)return false;
        if(!record(false,[&](auto* list){for(const auto& resource:graph.m_Resources)transition(list,resource,resource.InitialState,D3D12_RESOURCE_STATE_COMMON);}))return false;
        for(UINT index:graph.m_ExecutionOrder)
        {
            auto& pass=graph.m_Passes[index];
            if(!record(pass.QueueType==RenderGraph::Queue::Compute,[&](auto* list)
            {
                for(const auto& usage:pass.Usages)
                {const auto& resource=graph.m_Resources[usage.Resource.Index];if(resource.Transient&&graph.m_Arena)graph.m_Arena->Activate(resource.ArenaIndex,list);transition(list,resource,D3D12_RESOURCE_STATE_COMMON,usage.State);}
                pass.Execute(list);
                for(const auto& usage:pass.Usages)transition(list,graph.m_Resources[usage.Resource.Index],usage.State,D3D12_RESOURCE_STATE_COMMON);
            }))return false;
        }
        if(!record(false,[&](auto* list){for(const auto& resource:graph.m_Resources)if(resource.HasFinalState)transition(list,resource,D3D12_RESOURCE_STATE_COMMON,resource.FinalState);}))return false;
        // Record everything before submission. After the first ExecuteCommandLists,
        // failures are fatal to this frame and must never trigger duplicate work.
        for(auto& batch:frame.Batches)
        {
            auto* queue=batch.Compute?m_Compute.Get():m_Graphics.Get();if(m_Last&&FAILED(queue->Wait(m_Fence.Get(),m_Last)))return false;
            ID3D12CommandList* lists[]={batch.List.Get()};queue->ExecuteCommandLists(1,lists);++m_Last;
            if(FAILED(queue->Signal(m_Fence.Get(),m_Last)))return false;frame.Fence=m_Last;
        }
        return SUCCEEDED(m_Graphics->Wait(m_Fence.Get(),m_Last));
    }
private:
    ComPtr<ID3D12Device>m_Device;ComPtr<ID3D12CommandQueue>m_Graphics,m_Compute;ComPtr<ID3D12Fence>m_Fence;
    std::array<Frame,3>m_Frames;HANDLE m_Event=nullptr;UINT64 m_Last=0;
};
