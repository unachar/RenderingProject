#pragma once
#include "pch.h"
#include "graphicsdevice.h"
#include "camera.h"
#include "renderersettings.h"
#include "temporalhistorycontext.h"
#include "persistentpsocache.h"
#include "renderprofiler.h"
#include "graphicslog.h"
class TemporalUpscaler final
{
public:
    static bool IsRequested() { return RendererSettings::GetUpscaleMode()==UpscaleMode::Tsr; }
    static bool HasValidHistory() { return s_Valid; }
    static bool IsSubpixelAAEnabled() { return s_SubpixelAA; }
    static bool IsInputTaaEnabled() { return s_InputTaa; }
    static bool IsDetailStabilityEnabled() { return s_DetailStability; }
    static bool IsPlanarDepthEnabled() { return s_PlanarDepth; }
    static bool IsThinGeometryEnabled() { return s_ThinGeometry; }
    static void SetThinGeometryEnabled(bool enabled)
    {
        if (s_ThinGeometry != enabled) Invalidate();
        s_ThinGeometry = enabled;
    }
    static void SetPlanarDepthEnabled(bool enabled)
    {
        if (s_PlanarDepth != enabled) Invalidate();
        s_PlanarDepth = enabled;
    }
    static void SetDetailStabilityEnabled(bool enabled)
    {
        if (s_DetailStability != enabled) Invalidate();
        s_DetailStability = enabled;
    }
    static void SetInputTaaEnabled(bool enabled)
    {
        if (s_InputTaa != enabled) Invalidate();
        s_InputTaa = enabled;
    }
    static void SetSubpixelAAEnabled(bool enabled)
    {
        if (s_SubpixelAA != enabled) Invalidate();
        s_SubpixelAA = enabled;
    }
    static XMFLOAT2 Jitter()
    {
        auto halton=[](UINT i,UINT base) { float r=0,f=1; while(i) { f/=base; r+=f*(i%base); i/=base; } return r; };
        const float scale=(std::max)(1.f,float(GraphicsDevice::GetWidth())/(std::max)(1u,GraphicsDevice::GetSceneWidth()));
        const UINT phase=RendererState::m_TaaFrameIndex%static_cast<UINT>(std::clamp(8.f*scale*scale,8.f,128.f))+1;
        return {halton(phase,2)-.5f,halton(phase,3)-.5f};
    }
    static void Invalidate() { s_Valid=false; }
    static void Shutdown()
    {
        for (auto& color : s_Color) color.Reset();
        for (auto& depth : s_Depth) depth.Reset();
        for (auto& detail : s_Detail) detail.Reset();
        s_Heap.Reset();
        s_Constants.Reset();
        s_Root.Reset();
        s_Pso.Reset();
        s_Width = s_Height = 0;
        s_Valid = false;
    }
    static bool Execute(ID3D12GraphicsCommandList* list,UINT slot,ID3D12Resource* color,ID3D12Resource* depth,ID3D12Resource* velocity,ID3D12Resource* reveal,ID3D12Resource* output, double exposure)
    {
        if(!list||!color||!depth||!velocity||!output) { Invalidate(); return false; }
        auto* device=GraphicsDevice::GetDevice(); const auto in=color->GetDesc(),out=output->GetDesc();
        if(!Initialize(device)||!EnsureTargets(device,UINT(out.Width),out.Height,out.Format)) { Invalidate(); return false; }
        RenderProfiler::ScopedEvent profile("Custom TSR",list); slot%=RendererState::g_kFRAME_COUNT;
        const UINT write=s_Write,history=write^1u,base=slot*10,inc=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu=[&](UINT i){return CD3DX12_CPU_DESCRIPTOR_HANDLE(s_Heap->GetCPUDescriptorHandleForHeapStart(),base+i,inc);};
        auto gpu=[&](UINT i){return CD3DX12_GPU_DESCRIPTOR_HANDLE(s_Heap->GetGPUDescriptorHandleForHeapStart(),base+i,inc);};
        auto srv=[&](UINT i,ID3D12Resource* resource,DXGI_FORMAT format)
        { D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.Format=format; d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; d.Texture2D.MipLevels=1; device->CreateShaderResourceView(resource,&d,cpu(i)); };
        srv(0,color,in.Format); srv(1,depth,DXGI_FORMAT_R32_FLOAT); srv(2,velocity,velocity->GetDesc().Format);
        srv(3,s_Color[history].Get(),out.Format); srv(4,s_Depth[history].Get(),DXGI_FORMAT_R32_FLOAT); srv(5,reveal,reveal?reveal->GetDesc().Format:DXGI_FORMAT_R16_FLOAT);
        srv(6, s_Detail[history].Get(), DXGI_FORMAT_R16G16_FLOAT);
        for(UINT i=0;i<2;++i)
        { D3D12_UNORDERED_ACCESS_VIEW_DESC d{}; d.Format=i?DXGI_FORMAT_R32_FLOAT:out.Format; d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D; device->CreateUnorderedAccessView(i?s_Depth[write].Get():s_Color[write].Get(),nullptr,&d,cpu(7+i)); }
        D3D12_UNORDERED_ACCESS_VIEW_DESC detailUav{};
        detailUav.Format = DXGI_FORMAT_R16G16_FLOAT;
        detailUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(s_Detail[write].Get(), nullptr, &detailUav, cpu(9));
        struct alignas(16) Constants { XMFLOAT4 Extent,Jitter,Parameters; XMFLOAT4X4 InvCurrent,Previous; XMFLOAT4 Reconstruction; } constants{};
        static_assert(sizeof(Constants)<=256);
        constants.Extent={float(in.Width),float(in.Height),float(out.Width),float(out.Height)};
        const auto jitter=Jitter(); constants.Jitter={jitter.x/in.Width,jitter.y/in.Height,s_PreviousJitter.x,s_PreviousJitter.y};
        const auto state=TemporalHistory::Context::Capture(Camera::GetCameraEntity(),UINT(in.Width),in.Height,(uint64_t(out.Width)<<32)^out.Height^uint64_t(out.Format), exposure);
        XMMATRIX view,projection; Camera::GetCameraMatrices(Camera::GetCameraEntity(),view,projection);
        XMFLOAT4X4 currentView{}; XMStoreFloat4x4(&currentView,view); float delta=0;
        for(UINT r=0;r<4;++r)for(UINT c=0;c<4;++c) { const float d=currentView.m[r][c]-s_PreviousView.m[r][c]; delta+=d*d; }
        const bool reuse=TemporalHistory::Evaluate(s_State,state,s_Valid).CanReuse&&delta<16;
        constants.Parameters={reuse?1.f:0.f,reveal?1.f:0.f,s_SubpixelAA?1.f:0.f,s_DetailStability?1.f:0.f};
        constants.Reconstruction.x = s_PlanarDepth ? 1.f : 0.f;
        constants.Reconstruction.y = s_ThinGeometry ? 1.f : 0.f;
        XMStoreFloat4x4(&constants.InvCurrent,XMMatrixTranspose(XMMatrixInverse(nullptr,view*projection)));
        XMStoreFloat4x4(&constants.Previous,XMMatrixTranspose(XMLoadFloat4x4(&GraphicsDevice::GetPreviousViewMatrix())*XMLoadFloat4x4(&GraphicsDevice::GetPreviousProjectionMatrix())));
        void* mapped=nullptr; const D3D12_RANGE noRead{0,0}; if(FAILED(s_Constants->Map(0,&noRead,&mapped))) { Invalidate(); return false; }
        memcpy(static_cast<BYTE*>(mapped)+slot*256,&constants,sizeof(constants)); s_Constants->Unmap(0,nullptr);
        constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto transition=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
        { const auto b=CD3DX12_RESOURCE_BARRIER::Transition(resource,before,after); list->ResourceBarrier(1,&b); };
        for(auto* resource:{color,depth,velocity})transition(resource,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,read);
        if(reveal)transition(reveal,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,read);
        transition(s_Color[write].Get(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); transition(s_Depth[write].Get(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(s_Detail[write].Get(), read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[]={s_Heap.Get()}; list->SetDescriptorHeaps(1,heaps); list->SetComputeRootSignature(s_Root.Get()); list->SetPipelineState(s_Pso.Get());
        list->SetComputeRootConstantBufferView(0,s_Constants->GetGPUVirtualAddress()+slot*256); list->SetComputeRootDescriptorTable(1,gpu(0)); list->SetComputeRootDescriptorTable(2,gpu(7));
        list->Dispatch((s_Width+7)/8,(s_Height+7)/8,1);
        transition(s_Depth[write].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read); transition(s_Color[write].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(s_Detail[write].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
        transition(output,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST); list->CopyResource(output,s_Color[write].Get());
        transition(output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); transition(s_Color[write].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,read);
        for(auto* resource:{color,depth,velocity})transition(resource,read,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if(reveal)transition(reveal,read,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        s_Write^=1; s_Valid=true; s_State=state; s_PreviousJitter={constants.Jitter.x,constants.Jitter.y}; s_PreviousView=currentView; return true;
    }
private:
    static bool Initialize(ID3D12Device* device)
    {
        if(s_Pso&&s_Heap&&s_Constants)return true;
        CD3DX12_DESCRIPTOR_RANGE ranges[2]; ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,7,0); ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0);
        CD3DX12_ROOT_PARAMETER parameters[3]; parameters[0].InitAsConstantBufferView(0); parameters[1].InitAsDescriptorTable(1,&ranges[0]); parameters[2].InitAsDescriptorTable(1,&ranges[1]);
        CD3DX12_STATIC_SAMPLER_DESC sampler(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC root(3,parameters,1,&sampler); ComPtr<ID3DBlob> blob,error;
        if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))||FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&s_Root))))return false;
        if(FAILED(D3DCompileFromFile(L"shader/hlsl/TemporalUpscaleCS.hlsl",nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"main","cs_6_5",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error)))
        { if(error)WriteGraphicsLog("Custom TSR: %s\n",static_cast<const char*>(error->GetBufferPointer()));return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=s_Root.Get();p.CS={blob->GetBufferPointer(),blob->GetBufferSize()};if(FAILED(PersistentPsoCache::Compute(device,p,s_Pso)))return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=10*RendererState::g_kFRAME_COUNT;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const auto upload=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),unused=upload; const auto buffer=CD3DX12_RESOURCE_DESC::Buffer(256*RendererState::g_kFRAME_COUNT);
        return SUCCEEDED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s_Heap)))&&SUCCEEDED(device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&s_Constants)));
    }
    static bool EnsureTargets(ID3D12Device* device,UINT width,UINT height,DXGI_FORMAT format)
    {
        if (width == s_Width && height == s_Height && format == s_Format &&
            s_Color[0] && s_Color[1] && s_Detail[0] && s_Detail[1]) return true;
        if (!width || !height || !GraphicsDevice::WaitForGpuIdle()) return false;

        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        std::array<ComPtr<ID3D12Resource>, 2> colors, depths, details;
        const auto createHistory = [&](DXGI_FORMAT historyFormat, ComPtr<ID3D12Resource>& resource)
        {
            const auto description = CD3DX12_RESOURCE_DESC::Tex2D(historyFormat, width, height,
                1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&resource)));
        };
        for (UINT i = 0; i < 2; ++i)
        {
            if (!createHistory(format, colors[i]) ||
                !createHistory(DXGI_FORMAT_R32_FLOAT, depths[i]) ||
                !createHistory(DXGI_FORMAT_R16G16_FLOAT, details[i])) return false;
        }
        s_Color = colors;
        s_Depth = depths;
        s_Detail = details;
        s_Width = width;
        s_Height = height;
        s_Format = format;
        s_Write = 0;
        s_Valid = false;
        return true;
    }
    static inline ComPtr<ID3D12RootSignature> s_Root;static inline ComPtr<ID3D12PipelineState>s_Pso;static inline ComPtr<ID3D12DescriptorHeap>s_Heap;static inline ComPtr<ID3D12Resource>s_Constants;
    static inline std::array<ComPtr<ID3D12Resource>,2>s_Color,s_Depth,s_Detail;
    static inline UINT s_Width=0,s_Height=0,s_Write=0;static inline DXGI_FORMAT s_Format=DXGI_FORMAT_UNKNOWN;static inline bool s_Valid=false;
    static inline XMFLOAT2 s_PreviousJitter{};static inline XMFLOAT4X4 s_PreviousView{};static inline TemporalHistory::State s_State;
    static inline bool s_SubpixelAA = true;
    static inline bool s_InputTaa = false;
    static inline bool s_DetailStability = true;
    static inline bool s_PlanarDepth = true;
    static inline bool s_ThinGeometry = true;
};
