#include "pch.h"
#include "noisecachebindings.h"
#include "meshshaderrenderer.h"
#include "meshlets/meshletmanager.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "gpuscene/gpuscene.h"
#include "frameconstants.h"
#include "camera.h"
#include "texturemanager.h"
#include "visibility/gpuvisibility.h"
#include "occlusionculling.h"
#include "renderprofiler.h"
#include "rendercapture.h"
#include "shaderpipelines.h"
#include "virtualgeometry/virtualgeometrytraversal.h"
#include "virtualgeometry/virtualgeometrystreaming.h"
#include "virtualgeometry/virtualgeometrybinning.h"
#include "lightingresources.h"
#include <unordered_set>

namespace MeshShaderRenderer
{
    namespace
    {
        struct Constants
        {
            XMFLOAT4X4 VP{}, PreviousVP{};
            XMFLOAT4 Camera{};
            uint32_t Capacity = 0, ObjectCapacity = 0, Meshes = 0, Materials = 0;
            uint32_t Frustum = 1, Cone = 1, Occlusion = 0, Clusters = 1;
            float Bias = 0.001f, Width = 1, Height = 1;
            uint32_t Mips = 1;
        };

        static_assert(sizeof(Constants) == 192);

        struct Frame
        {
            GpuSceneBuffer Candidates, Visible, Counters, Arguments, Fallback;
        };

        struct State
        {
            Settings Options;
            bool Supported = false, Active = false, Failed = false;
            D3D12_MESH_SHADER_TIER Tier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
            string Reason = "Not initialized";
            ComPtr<ID3D12RootSignature> ComputeRoot, OpaqueRoot;
            ComPtr<ID3D12PipelineState> ResetPSO, ClusterPSO, CullPSO, DispatchPSO, OpaquePSO, FallbackPSO;
            ComPtr<ID3D12PipelineState> VelocityPSO, VelocityFallbackPSO;
            ComPtr<ID3D12PipelineState> NativeOpaquePSO, NativeVelocityPSO;
            ComPtr<ID3D12PipelineState> NativeBinnedOpaquePSO, NativeBinnedVelocityPSO;
            ComPtr<ID3D12PipelineState> NativeOverdrawPSO, NativeBinnedOverdrawPSO;
            ComPtr<ID3D12CommandSignature> MeshSignature, FallbackSignature;
            ComPtr<ID3D12CommandSignature> MaterialBinSignature;
            array<Frame, GpuSceneBuffer::FrameCount> Frames;
            uint32_t FrameIndex = 0, ObjectCapacity = 0;
            ID3D12Resource *Objects = nullptr, *Count = nullptr, *Commands = nullptr;
            Constants Parameters;
            array<uint32_t, 16> Debug{};
            vector<array<uint32_t, 4>> DebugRecords;
        };

        State& Data()
        {
            static State data;
            return data;
        }

        Frame& Current()
        {
            return Data().Frames.at(Data().FrameIndex);
        }

        void Check(HRESULT result)
        {
            if (FAILED(result))
            {
                throw runtime_error("Mesh Shader resource/pipeline initialization failed");
            }
        }

        void Root(CD3DX12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>& output)
        {
            ComPtr<ID3DBlob> blob, error;
            Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
            Check(GraphicsDevice::GetDevice()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                                   IID_PPV_ARGS(&output)));
        }

        ComPtr<ID3DBlob> Shader(const char* path)
        {
            ComPtr<ID3DBlob> blob;
            ShaderDescription desc{};
            desc.csoPath = path;
            desc.ppBlob = blob.GetAddressOf();
            if (!ShaderPipelines::LoadShaderBlob(desc))
            {
                throw runtime_error(path);
            }
            return blob;
        }

        template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, class T> struct alignas(void*) Subobject
        {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Tag = Type;
            T Value{};
        };

        struct MeshStream
        {
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> Root;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> MS;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> PS;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> Blend;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> Raster;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> Depth;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> Targets;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> DepthFormat;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> Samples;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, uint32_t> Mask;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> Topology;
        };

        void Initialize()
        {
            auto& state = Data();
            if (state.MeshSignature)
            {
                return;
            }
            auto* device = GraphicsDevice::GetDevice();
            CD3DX12_DESCRIPTOR_RANGE hiz, cbv;
            hiz.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 9);
            cbv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
            CD3DX12_ROOT_PARAMETER compute[17];
            for (uint32_t i = 0; i < 9; ++i)
            {
                compute[i].InitAsShaderResourceView(i);
            }
            for (uint32_t i = 0; i < 5; ++i)
            {
                compute[9 + i].InitAsUnorderedAccessView(i);
            }
            compute[14].InitAsDescriptorTable(1, &hiz);
            compute[15].InitAsDescriptorTable(1, &cbv);
            compute[16].InitAsShaderResourceView(10);
            CD3DX12_ROOT_SIGNATURE_DESC desc(17, compute);
            Root(desc, state.ComputeRoot);
            auto cs = [&](const char* path, ComPtr<ID3D12PipelineState>& output) {
                auto blob = Shader(path);
                D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
                pipeline.pRootSignature = state.ComputeRoot.Get();
                pipeline.CS = {blob->GetBufferPointer(), blob->GetBufferSize()};
                Check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&output)));
            };
            cs("shader/hlsl/build/MeshletResetCS.cso", state.ResetPSO);
            cs("shader/hlsl/build/ClusterCullingCS.cso", state.ClusterPSO);
            cs("shader/hlsl/build/MeshletCullingCS.cso", state.CullPSO);
            cs("shader/hlsl/build/BuildMeshDispatchCS.cso", state.DispatchPSO);
            CD3DX12_DESCRIPTOR_RANGE textures;
            textures.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, FrameConstants::g_kMAX_SRVS, 0, 1);
            CD3DX12_ROOT_PARAMETER opaque[20];
            opaque[0].InitAsConstants(1, 4);
            opaque[1].InitAsDescriptorTable(1, &cbv);
            opaque[2].InitAsShaderResourceView(30);
            opaque[3].InitAsShaderResourceView(31);
            opaque[4].InitAsDescriptorTable(1, &textures, D3D12_SHADER_VISIBILITY_PIXEL);
            opaque[5].InitAsConstants(3, 5);
            opaque[6].InitAsShaderResourceView(32, 0, D3D12_SHADER_VISIBILITY_PIXEL);
            for (uint32_t i = 0; i < 6; ++i)
            {
                opaque[7 + i].InitAsShaderResourceView(33 + i);
            }
            opaque[13].InitAsConstants(1, 6);
            CD3DX12_DESCRIPTOR_RANGE pages;
            pages.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, VirtualGeometryStreaming::MaximumSlots, 0, 2);
            opaque[14].InitAsShaderResourceView(39);
            opaque[15].InitAsDescriptorTable(1, &pages);
            opaque[16].InitAsConstants(2, 7);
            // ライト定数（b1）。GPU 駆動の不透明 PS（POM セルフシャドウ）と同じ契約にしておく。
            opaque[17].InitAsConstantBufferView(1);
            opaque[18].InitAsShaderResourceView(0, 15, D3D12_SHADER_VISIBILITY_PIXEL);
            opaque[19].InitAsUnorderedAccessView(0, 15, D3D12_SHADER_VISIBILITY_PIXEL);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_ANISOTROPIC);
            sampler.MaxAnisotropy = 16;
            CD3DX12_ROOT_SIGNATURE_DESC opaqueDesc(_countof(opaque), opaque, 1, &sampler,
                                                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
            if (g_BindlessShaderSettings.Enabled)
            {
                opaqueDesc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
            }
            Root(opaqueDesc, state.OpaqueRoot);
            auto ms = Shader("shader/hlsl/build/MeshletMS.cso"), ps = Shader("shader/hlsl/build/MeshletOpaquePS.cso");
            MeshStream stream{};
            stream.Root.Value = state.OpaqueRoot.Get();
            stream.MS.Value = {ms->GetBufferPointer(), ms->GetBufferSize()};
            stream.PS.Value = {ps->GetBufferPointer(), ps->GetBufferSize()};
            stream.Blend.Value = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            stream.Raster.Value = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            stream.Raster.Value.CullMode =
                D3D12_CULL_MODE_NONE; // メッシュ出力はプリミティブごとに片面・両面の規則を適用する。
            stream.Depth.Value = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            stream.Depth.Value.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
            stream.Targets.Value.NumRenderTargets = RendererState::g_kGEOMETRY_GBUFFER_COUNT;
            for (uint32_t i = 0; i < RendererState::g_kGEOMETRY_GBUFFER_COUNT; ++i)
            {
                stream.Targets.Value.RTFormats[i] = RendererState::GetGBufferFormat(static_cast<GBufferType>(i));
            }
            stream.DepthFormat.Value = DXGI_FORMAT_D32_FLOAT;
            stream.Samples.Value = {1, 0};
            stream.Mask.Value = UINT_MAX;
            stream.Topology.Value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            ComPtr<ID3D12Device2> device2;
            Check(device->QueryInterface(IID_PPV_ARGS(&device2)));
            D3D12_PIPELINE_STATE_STREAM_DESC meshDesc{sizeof(stream), &stream};
            Check(device2->CreatePipelineState(&meshDesc, IID_PPV_ARGS(&state.OpaquePSO)));
            auto nativeMS = Shader("shader/hlsl/build/VirtualGeometryMS.cso");
            auto nativePS = Shader("shader/hlsl/build/VirtualGeometryOpaquePS.cso");
            stream.MS.Value = {nativeMS->GetBufferPointer(), nativeMS->GetBufferSize()};
            stream.PS.Value = {nativePS->GetBufferPointer(), nativePS->GetBufferSize()};
            Check(device2->CreatePipelineState(&meshDesc, IID_PPV_ARGS(&state.NativeOpaquePSO)));
            auto createOverdraw = [&](ComPtr<ID3D12PipelineState>& output) {
                const auto originalDepth = stream.Depth.Value;
                const auto originalBlend = stream.Blend.Value;
                stream.Depth.Value.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                stream.Depth.Value.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                stream.Blend.Value.IndependentBlendEnable = TRUE;
                for (auto& target : stream.Blend.Value.RenderTarget)
                {
                    target.RenderTargetWriteMask = 0;
                }
                auto& emission = stream.Blend.Value.RenderTarget[5];
                emission.RenderTargetWriteMask =
                    D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
                emission.BlendEnable = TRUE;
                emission.SrcBlend = emission.DestBlend = D3D12_BLEND_ONE;
                emission.BlendOp = D3D12_BLEND_OP_ADD;
                emission.SrcBlendAlpha = D3D12_BLEND_ONE;
                emission.DestBlendAlpha = D3D12_BLEND_ZERO;
                emission.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                const auto result = device2->CreatePipelineState(&meshDesc, IID_PPV_ARGS(&output));
                stream.Depth.Value = originalDepth;
                stream.Blend.Value = originalBlend;
                Check(result);
            };
            createOverdraw(state.NativeOverdrawPSO);
            try
            {
                auto binnedMS = Shader("shader/hlsl/build/VirtualGeometryBinnedMS.cso");
                stream.MS.Value = {binnedMS->GetBufferPointer(), binnedMS->GetBufferSize()};
                Check(device2->CreatePipelineState(&meshDesc, IID_PPV_ARGS(&state.NativeBinnedOpaquePSO)));
                createOverdraw(state.NativeBinnedOverdrawPSO);
            }
            catch (const std::exception& error)
            {
                WriteGraphicsLog("Native binned raster unavailable; original native path retained: %s\n", error.what());
            }
            stream.MS.Value = {ms->GetBufferPointer(), ms->GetBufferSize()};
            stream.PS.Value = {ps->GetBufferPointer(), ps->GetBufferSize()};
            D3D12_INDIRECT_ARGUMENT_DESC meshArg{};
            meshArg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
            D3D12_COMMAND_SIGNATURE_DESC signature{12, 1, &meshArg, 0};
            Check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&state.MeshSignature)));
            D3D12_INDIRECT_ARGUMENT_DESC binArgs[2]{};
            binArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
            binArgs[0].Constant.RootParameterIndex = 16;
            binArgs[0].Constant.Num32BitValuesToSet = 2;
            binArgs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
            D3D12_COMMAND_SIGNATURE_DESC binSignature{sizeof(VirtualGeometryBinning::IndirectCommand), 2, binArgs, 0};
            Check(device->CreateCommandSignature(&binSignature, state.OpaqueRoot.Get(),
                                                 IID_PPV_ARGS(&state.MaterialBinSignature)));
            // フォールバックは同じピクセルシェーダーとバインドを使い、別経路の物体のインデックス数をゼロにする。
            auto vs = Shader("shader/hlsl/build/MeshletFallbackVS.cso");
            const D3D12_INPUT_ELEMENT_DESC layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
            D3D12_GRAPHICS_PIPELINE_STATE_DESC traditional{};
            traditional.pRootSignature = state.OpaqueRoot.Get();
            traditional.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            traditional.PS = stream.PS.Value;
            traditional.InputLayout = {layout, 4};
            traditional.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            traditional.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            traditional.BlendState = stream.Blend.Value;
            traditional.DepthStencilState = stream.Depth.Value;
            traditional.NumRenderTargets = RendererState::g_kGEOMETRY_GBUFFER_COUNT;
            for (uint32_t i = 0; i < RendererState::g_kGEOMETRY_GBUFFER_COUNT; ++i)
            {
                traditional.RTVFormats[i] = stream.Targets.Value.RTFormats[i];
            }
            traditional.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            traditional.SampleDesc = {1, 0};
            traditional.SampleMask = UINT_MAX;
            traditional.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            Check(device->CreateGraphicsPipelineState(&traditional, IID_PPV_ARGS(&state.FallbackPSO)));
            D3D12_INDIRECT_ARGUMENT_DESC fallbackArgs[2]{};
            fallbackArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
            fallbackArgs[0].Constant.RootParameterIndex = 0;
            fallbackArgs[0].Constant.Num32BitValuesToSet = 1;
            fallbackArgs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
            signature = {24, 2, fallbackArgs, 0};
            Check(device->CreateCommandSignature(&signature, state.OpaqueRoot.Get(),
                                                 IID_PPV_ARGS(&state.FallbackSignature)));
            auto velocityMS = Shader("shader/hlsl/build/MeshletVelocityMS.cso"),
                 velocityPS = Shader("shader/hlsl/build/MeshletVelocityPS.cso"),
                 velocityVS = Shader("shader/hlsl/build/MeshletVelocityVS.cso");
            stream.MS.Value = {velocityMS->GetBufferPointer(), velocityMS->GetBufferSize()};
            stream.PS.Value = {velocityPS->GetBufferPointer(), velocityPS->GetBufferSize()};
            stream.Targets.Value = {};
            stream.Targets.Value.NumRenderTargets = 1;
            stream.Targets.Value.RTFormats[0] = RendererState::GetGBufferFormat(GBufferType::VELOCITY);
            stream.Depth.Value.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            stream.Depth.Value.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            Check(device2->CreatePipelineState(&meshDesc, IID_PPV_ARGS(&state.VelocityPSO)));
            auto nativeVelocity = Shader("shader/hlsl/build/VirtualGeometryVelocityMS.cso");
            stream.MS.Value = {nativeVelocity->GetBufferPointer(), nativeVelocity->GetBufferSize()};
            Check(device2->CreatePipelineState(&meshDesc, IID_PPV_ARGS(&state.NativeVelocityPSO)));
            try
            {
                auto binnedVelocityMS = Shader("shader/hlsl/build/VirtualGeometryBinnedVelocityMS.cso");
                stream.MS.Value = {binnedVelocityMS->GetBufferPointer(), binnedVelocityMS->GetBufferSize()};
                Check(device2->CreatePipelineState(&meshDesc, IID_PPV_ARGS(&state.NativeBinnedVelocityPSO)));
            }
            catch (const std::exception& error)
            {
                WriteGraphicsLog("Native binned velocity unavailable; original native path retained: %s\n",
                                 error.what());
            }
            traditional.VS = {velocityVS->GetBufferPointer(), velocityVS->GetBufferSize()};
            traditional.PS = stream.PS.Value;
            traditional.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            traditional.NumRenderTargets = 1;
            for (auto& format : traditional.RTVFormats)
            {
                format = DXGI_FORMAT_UNKNOWN;
            }
            traditional.RTVFormats[0] = stream.Targets.Value.RTFormats[0];
            traditional.DepthStencilState = stream.Depth.Value;
            Check(device->CreateGraphicsPipelineState(&traditional, IID_PPV_ARGS(&state.VelocityFallbackPSO)));
            state.Reason = "Ready";
            WriteGraphicsLog("Phase 5 Mesh Shader PSO and indirect signature ready\n");
        }

        RenderGraph::ResourceHandle Import(RenderGraph& graph, const char* name, GpuSceneBuffer& buffer,
                                           D3D12_RESOURCE_STATES finalState)
        {
            const auto existing = graph.FindImportedResource(buffer.Resource());
            if (existing.IsValid())
            {
                return existing;
            }
            RenderGraph::ImportedResource input{};
            input.Name = name;
            input.Resource = buffer.Resource();
            input.InitialState = buffer.State;
            input.HasFinalState = true;
            input.FinalState = finalState;
            buffer.State = finalState;
            return graph.ImportResource(input);
        }

        RenderGraph::ResourceHandle External(RenderGraph& graph, const char* name, ID3D12Resource* resource,
                                             D3D12_RESOURCE_STATES state)
        {
            const auto existing = graph.FindImportedResource(resource);
            if (existing.IsValid())
            {
                return existing;
            }
            RenderGraph::ImportedResource input{};
            input.Name = name;
            input.Resource = resource;
            input.InitialState = input.FinalState = state;
            input.HasFinalState = true;
            return graph.ImportResource(input);
        }

        void BindCompute(ID3D12GraphicsCommandList* list)
        {
            auto& s = Data();
            auto& f = Current();
            ID3D12DescriptorHeap* heap = FrameConstants::GetCbvHeap();
            list->SetDescriptorHeaps(1, &heap);
            list->SetComputeRootSignature(s.ComputeRoot.Get());
            ID3D12Resource* resources[] = {GpuScene::InstanceBuffer(),
                                           s.Objects,
                                           MeshletManager::RangeBuffer().Resource(),
                                           MeshletManager::ClusterBuffer().Resource(),
                                           MeshletManager::BoundsBuffer().Resource(),
                                           MeshletManager::MeshletBuffer().Resource(),
                                           s.Count,
                                           s.Commands,
                                           GpuScene::MaterialBuffer()};
            for (uint32_t i = 0; i < 9; ++i)
            {
                list->SetComputeRootShaderResourceView(i, resources[i]->GetGPUVirtualAddress());
            }
            GpuSceneBuffer* outputs[] = {&f.Candidates, &f.Visible, &f.Counters, &f.Arguments, &f.Fallback};
            for (uint32_t i = 0; i < 5; ++i)
            {
                list->SetComputeRootUnorderedAccessView(9 + i, outputs[i]->Resource()->GetGPUVirtualAddress());
            }
            list->SetComputeRootDescriptorTable(14, OcclusionCulling::GetPreviousSrv());
            list->SetComputeRootDescriptorTable(
                15, FrameConstants::AllocateTransientConstantBuffer(&s.Parameters, sizeof(s.Parameters)));
            list->SetComputeRootShaderResourceView(
                16, VirtualGeometryTraversal::SuccessBuffer().Resource()->GetGPUVirtualAddress());
        }
    } // 名前空間

    Settings& GetSettings()
    {
        return Data().Options;
    }

    void QuerySupport(ID3D12Device* device)
    {
        auto& s = Data();
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
        D3D12_FEATURE_DATA_D3D12_OPTIONS bindings{};
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
        s.Supported = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options))) &&
                      options.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED &&
                      SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) &&
                      model.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
        s.Supported =
            s.Supported &&
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &bindings, sizeof(bindings))) &&
            bindings.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2;
        s.Tier = options.MeshShaderTier;
        s.Reason = s.Supported ? "Nanite automatic static geometry rendering"
                               : "Mesh Shader/SM6.5 unsupported; traditional rendering";
        char env[8]{};
        if (GetEnvironmentVariableA("DX12_PHASE5_FORCE_UNSUPPORTED", env, sizeof(env)) && atoi(env) != 0)
        {
            s.Supported = false;
            s.Reason = "Unsupported feature gate forced for validation";
        }
        if (GetEnvironmentVariableA("DX12_PHASE5_MESH_SHADER", env, sizeof(env)))
        {
            s.Options.Enabled = atoi(env) != 0;
        }
        if (GetEnvironmentVariableA("DX12_VIRTUAL_GEOMETRY_ENABLED", env, sizeof(env)))
        {
            VirtualGeometryTraversal::GetSettings().Enabled = atoi(env) != 0;
        }
        WriteGraphicsLog("Phase 5 Mesh Shader support=%u tier=%u SM=%u\n", s.Supported, static_cast<uint32_t>(s.Tier),
                         static_cast<uint32_t>(model.HighestShaderModel));
    }

    bool Supported()
    {
        return Data().Supported;
    }

    bool IsActive()
    {
        return Data().Active;
    }

    bool NativeReady()
    {
        const auto& state = Data();
        return GetGeometryRenderingMode() != GeometryRenderingMode::Legacy && state.Supported &&
               state.Options.Enabled && !state.Failed && state.NativeOpaquePSO && state.NativeVelocityPSO;
    }

    void Prepare(RenderGraph& graph, RenderGraph::ResourceHandle instances, RenderGraph::ResourceHandle objects,
                 RenderGraph::ResourceHandle count, RenderGraph::ResourceHandle commands,
                 ID3D12Resource* objectResource, ID3D12Resource* countResource, ID3D12Resource* commandsResource,
                 uint32_t capacity, uint32_t meshes)
    {
        auto& s = Data();
        s.Active = false;
        if (GetGeometryRenderingMode() == GeometryRenderingMode::Legacy || !s.Options.Enabled || !s.Supported ||
            s.Failed)
        {
            VirtualGeometryTraversal::InvalidateHistory();
            return;
        }
        try
        {
            Initialize();
        }
        catch (const exception& error)
        {
            s.Failed = true;
            s.Reason = error.what();
            WriteGraphicsLog("Phase 5 traditional fallback: %s\n", error.what());
            return;
        }
        s.FrameIndex = GraphicsDevice::GetFrameIndex();
        s.Objects = objectResource;
        s.Count = countResource;
        s.Commands = commandsResource;
        s.ObjectCapacity = capacity;
        s.Options.MaxMeshlets = clamp(s.Options.MaxMeshlets, 1u, 4000000u);
        auto& f = Current();
        auto prepare = [&](GpuSceneBuffer& buffer, uint32_t n, uint32_t stride) {
            buffer.BeginFrame(s.FrameIndex);
            buffer.Ensure(GraphicsDevice::GetDevice(), n, stride, s.FrameIndex, L"Phase 5 Frame Buffer", true);
        };
        try
        {
            prepare(f.Candidates, s.Options.MaxMeshlets, 16);
            prepare(f.Visible, s.Options.MaxMeshlets, 8);
            prepare(f.Counters, 16, 4);
            prepare(f.Arguments, 1, 12);
            prepare(f.Fallback, capacity, 24);
            MeshletManager::Prepare(graph, meshes);
            VirtualGeometryTraversal::Prepare(graph,
                                              {instances, objects, count, objectResource, countResource, capacity});
            const bool overdraw =
                VirtualGeometryTraversal::GetSettings().View == VirtualGeometryTraversal::DebugView::Overdraw;
            VirtualGeometryBinning::BeginFrame(s.NativeBinnedOpaquePSO.Get() && s.NativeBinnedVelocityPSO.Get() &&
                                               (!overdraw || s.NativeBinnedOverdrawPSO.Get()));
        }
        catch (const exception& error)
        {
            s.Failed = true;
            s.Reason = error.what();
            WriteGraphicsLog("Phase 5 buffer allocation fallback: %s\n", error.what());
            return;
        }
        auto& p = s.Parameters;
        XMMATRIX view, projection;
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        XMStoreFloat4x4(&p.VP, view * projection);
        XMStoreFloat4x4(&p.PreviousVP, XMLoadFloat4x4(&GraphicsDevice::GetPreviousViewMatrix()) *
                                           XMLoadFloat4x4(&GraphicsDevice::GetPreviousProjectionMatrix()));
        XMStoreFloat4(&p.Camera, XMMatrixInverse(nullptr, view).r[3]);
        if (GPUVisibility::GetSettings().FreezeCamera)
        {
            p.VP = GPUVisibility::CullingViewProjection();
            // 現在のカメラとカリング用カメラが異なる間は、法線コーンを無効にする。
        }
        p.Capacity = s.Options.MaxMeshlets;
        p.ObjectCapacity = capacity;
        p.Meshes = meshes;
        p.Materials = GpuScene::Statistics().Materials;
        p.Frustum = s.Options.Frustum;
        p.Cone = s.Options.Cone && !GPUVisibility::GetSettings().FreezeCamera;
        p.Occlusion = s.Options.Occlusion && GPUVisibility::HistoryValid();
        p.Clusters = s.Options.Clusters;
        p.Bias = s.Options.Bias;
        p.Width = float((max)(1u, OcclusionCulling::GetWidth()));
        p.Height = float((max)(1u, OcclusionCulling::GetHeight()));
        p.Mips = (max)(1u, OcclusionCulling::GetMipCount());
        auto candidate = Import(graph, "Meshlet Candidates", f.Candidates, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto visible = Import(graph, "Visible Meshlets", f.Visible, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto stats = Import(graph, "Meshlet Counters", f.Counters, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto args = Import(graph, "Mesh Dispatch Arguments", f.Arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        auto fallback =
            Import(graph, "Mesh Shader Fallback Commands", f.Fallback, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        vector<RenderGraph::ResourceHandle> asset;
        GpuSceneBuffer* sources[] = {&MeshletManager::RangeBuffer(), &MeshletManager::ClusterBuffer(),
                                     &MeshletManager::BoundsBuffer(), &MeshletManager::MeshletBuffer()};
        for (auto* source : sources)
        {
            asset.push_back(
                Import(graph, "Meshlet Asset SRV", *source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        }
        auto materials = External(graph, "Meshlet Material Flags", GpuScene::MaterialBuffer(),
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto nativeSuccess = Import(graph, "Nanite Successful Instances", VirtualGeometryTraversal::SuccessBuffer(),
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        RenderGraph::ResourceHandle hiz;
        if (OcclusionCulling::GetPreviousResource())
        {
            hiz =
                External(graph, "Meshlet Hi-Z", OcclusionCulling::GetPreviousResource(), OcclusionCulling::ReadState());
        }
        graph.AddPass(
            "Meshlet Counter Reset",
            [=](RenderGraph::PassBuilder& b) {
                b.Write(stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            },
            [](ID3D12GraphicsCommandList* list) {
                BindCompute(list);
                list->SetPipelineState(Data().ResetPSO.Get());
                list->Dispatch(1, 1, 1);
            });
        graph.AddPass(
            "Cluster Culling / Meshlet Candidates",
            [=](RenderGraph::PassBuilder& b) {
                b.Read(instances, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    .Read(objects, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    .Read(count, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    .Read(commands, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    .Read(materials, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                b.Read(nativeSuccess, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                for (auto handle : asset)
                {
                    b.Read(handle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                if (OcclusionCulling::GetPreviousResource())
                {
                    b.Read(hiz, OcclusionCulling::ReadState());
                }
                b.Write(candidate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    .ReadWrite(stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    .Write(fallback, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            },
            [](ID3D12GraphicsCommandList* list) {
                RenderProfiler::ScopedEvent profile("Cluster Culling", list);
                BindCompute(list);
                list->SetPipelineState(Data().ClusterPSO.Get());
                auto groups = Meshlets::DispatchDimensions(Data().ObjectCapacity);
                list->Dispatch(groups[0], groups[1], groups[2]);
            });
        graph.AddPass(
            "Meshlet Culling",
            [=](RenderGraph::PassBuilder& b) {
                b.Read(instances, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    .Read(materials, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                for (auto handle : asset)
                {
                    b.Read(handle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                if (OcclusionCulling::GetPreviousResource())
                {
                    b.Read(hiz, OcclusionCulling::ReadState());
                }
                b.ReadWrite(candidate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    .ReadWrite(stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    .Write(visible, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            },
            [](ID3D12GraphicsCommandList* list) {
                RenderProfiler::ScopedEvent profile("Meshlet Culling", list);
                BindCompute(list);
                list->SetPipelineState(Data().CullPSO.Get());
                list->Dispatch((Data().Parameters.Capacity + 63) / 64, 1, 1);
            });
        graph.AddPass(
            "Build Mesh Dispatch Arguments",
            [=](RenderGraph::PassBuilder& b) {
                b.ReadWrite(stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    .Write(args, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            },
            [](ID3D12GraphicsCommandList* list) {
                RenderProfiler::ScopedEvent profile("Mesh Dispatch Generation", list);
                BindCompute(list);
                list->SetPipelineState(Data().DispatchPSO.Get());
                list->Dispatch(1, 1, 1);
            });
        s.Active = true;
    }

    static bool DrawPass(ID3D12GraphicsCommandList* list, GpuSceneBuffer& vertices, bool velocity,
                         VirtualGeometryTraversal::RasterPhase phase)
    {
        auto& s = Data();
        if (!s.Active)
        {
            return false;
        }
        RenderGraph graph;
        bool binning = VirtualGeometryBinning::Active();
        VirtualGeometryBinning::Prepare(graph, phase);
        auto vertex =
            Import(graph, "Mesh Shader Global Vertices", vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        auto visible = Import(graph, "Mesh Shader Visible List", Current().Visible,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto stats =
            Import(graph, "Mesh Shader Count", Current().Counters, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto args = Import(graph, "Mesh Shader Dispatch", Current().Arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        auto fallback =
            Import(graph, "Mesh Shader Fallback", Current().Fallback, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        auto count = External(graph, "Fallback Count", s.Count, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        auto scene = External(graph, "Mesh Shader Instances", GpuScene::InstanceBuffer(),
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto material = External(graph, "Mesh Shader Materials", GpuScene::MaterialBuffer(),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto debug = External(graph, "Mesh Shader Visibility Debug", GPUVisibility::ResultBuffer(),
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        vector<RenderGraph::ResourceHandle> asset;
        GpuSceneBuffer* sources[] = {&MeshletManager::MeshletBuffer(), &MeshletManager::VertexBuffer(),
                                     &MeshletManager::PrimitiveBuffer()};
        for (auto* source : sources)
        {
            asset.push_back(
                Import(graph, "Mesh Shader Asset", *source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        }
        GpuSceneBuffer* nativeSources[] = {&VirtualGeometryResources::ClusterBuffer(),
                                           &VirtualGeometryResources::AssetBuffer(),
                                           &VirtualGeometryResources::TriangleBuffer(),
                                           &VirtualGeometryResources::VertexBuffer(),
                                           binning ? &VirtualGeometryBinning::VisibleBuffer()
                                                   : &VirtualGeometryTraversal::VisibleBuffer(phase),
                                           &VirtualGeometryTraversal::CounterBuffer(phase)};
        for (auto* source : nativeSources)
        {
            asset.push_back(Import(graph, "Native VirtualGeometry Raster Input", *source,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        }
        auto& nativeArgumentBuffer =
            binning ? VirtualGeometryBinning::ArgumentBuffer() : VirtualGeometryTraversal::ArgumentBuffer(phase);
        auto nativeArgs = Import(graph, "Native VirtualGeometry Dispatch", nativeArgumentBuffer,
                                 D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        RenderGraph::ResourceHandle binCount;
        if (binning)
        {
            binCount = Import(graph, "Native material bin count", VirtualGeometryBinning::CounterBuffer(),
                              D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        }
        asset.push_back(Import(graph, "Geometry Raster Residency", VirtualGeometryStreaming::ResidencyBuffer(),
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        // 負荷削減：常駐ページ（数千個の 64KB バッファ）を毎パス RenderGraph に登録しない。
        //   以前は FindImportedResource の線形探索（ページ数の 2 乗）と、ページごとの遷移バリア 2 回 × 3 パス
        //   （1 回目 / 2 回目 / Velocity）で GBuffer の CPU 時間の大半を使っていた。
        //   ページはコピーキューで書かれ、直接キューでは常に COMMON のバッファ。バッファは COMMON から
        //   読み取り状態へ暗黙に昇格し、ExecuteCommandLists の終わりに COMMON へ戻るので、明示的な遷移は不要
        graph.AddPass(
            velocity ? "Mesh Shader Velocity" : "Mesh Shader Opaque",
            [=](RenderGraph::PassBuilder& b) {
                auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                b.Read(scene, read)
                    .Read(material, read)
                    .Read(debug, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                    .Read(vertex, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    .Read(visible, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    .Read(stats, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    .Read(args, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
                    .Read(fallback, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
                    .Read(count, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                b.Read(nativeArgs, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                if (binning)
                {
                    b.Read(binCount, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                }
                for (auto handle : asset)
                {
                    b.Read(handle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
            },
            [&vertices, velocity, phase, binning](ID3D12GraphicsCommandList* commands) {
                auto& data = Data();
                RenderProfiler::ScopedEvent profile(velocity ? "Mesh Shader Velocity" : "Mesh Shader Opaque", commands);
                ID3D12DescriptorHeap* heap = FrameConstants::GetCbvHeap();
                commands->SetDescriptorHeaps(1, &heap);
                commands->SetGraphicsRootSignature(data.OpaqueRoot.Get());
                NoiseCacheBindings::BindGraphics(commands, NoiseCacheBindings::MeshRoot);
                ConstantBuffer3D constants{};
                XMMATRIX view, projection;
                Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
                constants.View = XMMatrixTranspose(view);
                constants.Projection = XMMatrixTranspose(projection);
                const auto cameraEntity = Camera::GetCameraEntity();
                if (ComponentManager::HasComponent<TransformComponent>(cameraEntity))
                {
                    constants.CameraPos =
                        ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity).Position;
                }
                constants.PreviousViewProjection =
                    XMMatrixTranspose(XMLoadFloat4x4(&GraphicsDevice::GetPreviousViewMatrix()) *
                                      XMLoadFloat4x4(&GraphicsDevice::GetPreviousProjectionMatrix()));
                commands->SetGraphicsRootDescriptorTable(1, FrameConstants::AllocateTransientConstantBuffer(constants));
                commands->SetGraphicsRootShaderResourceView(2, GpuScene::InstanceBuffer()->GetGPUVirtualAddress());
                commands->SetGraphicsRootShaderResourceView(3, GpuScene::MaterialBuffer()->GetGPUVirtualAddress());
                commands->SetGraphicsRootShaderResourceView(6, GPUVisibility::ResultBuffer()->GetGPUVirtualAddress());
                CD3DX12_GPU_DESCRIPTOR_HANDLE textures(heap->GetGPUDescriptorHandleForHeapStart(),
                                                       FrameConstants::g_kTEXTURE_SRV_START_INDEX,
                                                       FrameConstants::GetCbvIncrementSize());
                commands->SetGraphicsRootDescriptorTable(4, textures);
                const uint32_t limits[] = {FrameConstants::g_kTEXTURE_SRV_START_INDEX,
                                           static_cast<uint32_t>(TextureManager::GetDefaultTextureIndex()) -
                                               FrameConstants::g_kTEXTURE_SRV_START_INDEX,
                                           static_cast<uint32_t>(GPUVisibility::GetSettings().DebugMode)};
                commands->SetGraphicsRoot32BitConstants(5, 3, limits, 0);
                ID3D12Resource* resources[] = {MeshletManager::MeshletBuffer().Resource(),
                                               MeshletManager::VertexBuffer().Resource(),
                                               MeshletManager::PrimitiveBuffer().Resource(),
                                               vertices.Resource(),
                                               Current().Visible.Resource(),
                                               Current().Counters.Resource()};
                for (uint32_t i = 0; i < 6; ++i)
                {
                    commands->SetGraphicsRootShaderResourceView(7 + i, resources[i]->GetGPUVirtualAddress());
                }
                commands->SetGraphicsRoot32BitConstant(13, data.Options.Colors, 0);
                commands->SetGraphicsRootConstantBufferView(17,
                                                            LightingResources::GetCurrentLightConstantBufferAddress());
                if (phase != VirtualGeometryTraversal::RasterPhase::Second)
                {
                    commands->SetPipelineState(velocity ? data.VelocityFallbackPSO.Get() : data.FallbackPSO.Get());
                    commands->ExecuteIndirect(data.FallbackSignature.Get(), data.ObjectCapacity,
                                              Current().Fallback.Resource(), 0, data.Count, 0);
                    commands->SetPipelineState(velocity ? data.VelocityPSO.Get() : data.OpaquePSO.Get());
                    commands->ExecuteIndirect(data.MeshSignature.Get(), 1, Current().Arguments.Resource(), 0, nullptr,
                                              0);
                }
                ID3D12Resource* nativeResources[] = {VirtualGeometryResources::ClusterBuffer().Resource(),
                                                     VirtualGeometryResources::AssetBuffer().Resource(),
                                                     VirtualGeometryResources::TriangleBuffer().Resource(),
                                                     VirtualGeometryResources::VertexBuffer().Resource(),
                                                     binning
                                                         ? VirtualGeometryBinning::VisibleBuffer().Resource()
                                                         : VirtualGeometryTraversal::VisibleBuffer(phase).Resource(),
                                                     VirtualGeometryTraversal::CounterBuffer(phase).Resource()};
                for (uint32_t index = 0; index < 6; ++index)
                {
                    uint64_t offset = index == 5 ? VirtualGeometryTraversal::CounterOffset(phase) : 0;
                    commands->SetGraphicsRootShaderResourceView(
                        7 + index, nativeResources[index]->GetGPUVirtualAddress() + offset);
                }
                const auto& visualization = VirtualGeometryTraversal::GetSettings();
                uint32_t debugView = static_cast<uint32_t>(visualization.View);
                if (!debugView && visualization.LevelColors)
                {
                    debugView = static_cast<uint32_t>(VirtualGeometryTraversal::DebugView::LOD);
                }
                commands->SetGraphicsRoot32BitConstant(13, debugView, 0);
                commands->SetGraphicsRootShaderResourceView(
                    14, VirtualGeometryStreaming::ResidencyBuffer().Resource()->GetGPUVirtualAddress());
                commands->SetGraphicsRootDescriptorTable(15, VirtualGeometryStreaming::PageDescriptors());
                ID3D12PipelineState* nativePipeline = nullptr;
                if (binning)
                {
                    nativePipeline = velocity ? data.NativeBinnedVelocityPSO.Get() : data.NativeBinnedOpaquePSO.Get();
                }
                else
                {
                    nativePipeline = velocity ? data.NativeVelocityPSO.Get() : data.NativeOpaquePSO.Get();
                }
                commands->SetPipelineState(nativePipeline);
                if (!velocity && visualization.View == VirtualGeometryTraversal::DebugView::Overdraw)
                {
                    commands->SetPipelineState(binning ? data.NativeBinnedOverdrawPSO.Get()
                                                       : data.NativeOverdrawPSO.Get());
                }
                RenderProfiler::ScopedEvent nativeProfile(
                    velocity ? "Nanite Velocity Mesh Shader" : "Nanite Mesh Shader", commands);
                if (binning)
                {
                    commands->ExecuteIndirect(data.MaterialBinSignature.Get(),
                                              VirtualGeometryBinning::MaximumCommands(),
                                              VirtualGeometryBinning::ArgumentBuffer().Resource(), 0,
                                              VirtualGeometryBinning::CounterBuffer().Resource(), 0);
                }
                else
                {
                    commands->ExecuteIndirect(data.MeshSignature.Get(), 1,
                                              VirtualGeometryTraversal::ArgumentBuffer(phase).Resource(),
                                              VirtualGeometryTraversal::ArgumentOffset(phase), nullptr, 0);
                }
            });
        if (!graph.Compile() || !graph.Execute(list))
        {
            throw runtime_error(graph.GetLastError());
        }
        return true;
    }

    bool Draw(ID3D12GraphicsCommandList* list, GpuSceneBuffer& vertices)
    {
        return DrawPass(list, vertices, false, VirtualGeometryTraversal::RasterPhase::First);
    }

    bool DrawSecondPass(ID3D12GraphicsCommandList* list, GpuSceneBuffer& vertices)
    {
        VirtualGeometryTraversal::Retest(list);
        if (!VirtualGeometryTraversal::TwoPassActive())
        {
            return false;
        }
        return DrawPass(list, vertices, false, VirtualGeometryTraversal::RasterPhase::Second);
    }

    bool DrawVelocity(ID3D12GraphicsCommandList* list, GpuSceneBuffer& vertices)
    {
        return DrawPass(list, vertices, true, VirtualGeometryTraversal::RasterPhase::Combined);
    }

    void DrawDebug()
    {
        auto& s = Data();
        ImGui::Checkbox("Enabled##Phase5", &s.Options.Enabled);
        ImGui::Text("Supported: %s Tier: %u Active: %s", s.Supported ? "Yes" : "No", static_cast<uint32_t>(s.Tier),
                    s.Active ? "Yes" : "No");
        ImGui::TextUnformatted(s.Reason.c_str());
        ImGui::Checkbox("Meshlet Frustum", &s.Options.Frustum);
        ImGui::Checkbox("Meshlet Cone", &s.Options.Cone);
        ImGui::Checkbox("Meshlet Occlusion", &s.Options.Occlusion);
        ImGui::Checkbox("Cluster Culling", &s.Options.Clusters);
        ImGui::SliderFloat("Meshlet Occlusion Bias", &s.Options.Bias, 0, 0.01f, "%.5f");
        ImGui::Checkbox("Meshlet Colors", &s.Options.Colors);
        ImGui::Checkbox("Meshlet Bounds", &s.Options.MeshletBounds);
        ImGui::Checkbox("Cluster Bounds", &s.Options.ClusterBounds);
        ImGui::Checkbox("Cone Axes", &s.Options.ConeAxes);
        int capacity = static_cast<int>(s.Options.MaxMeshlets);
        if (ImGui::InputInt("Meshlet Capacity (overflow falls back)", &capacity))
        {
            s.Options.MaxMeshlets = static_cast<uint32_t>(clamp(capacity, 1, 4000000));
        }
        uint64_t total = 0, clusters = 0;
        for (const auto& r : MeshletManager::Ranges())
        {
            total += r.MeshletCount;
            clusters += r.ClusterCount;
        }
        ImGui::Text("Asset meshlets: %llu Clusters: %llu", total, clusters);
        ImGui::Text("Meshlet buffer memory: %.2f MiB", double(MeshletManager::MemoryBytes()) / (1024 * 1024));
        uint64_t scratchBytes = 0;
        for (const auto& frame : s.Frames)
        {
            scratchBytes += frame.Candidates.MemoryBytes() + frame.Visible.MemoryBytes() +
                            frame.Counters.MemoryBytes() + frame.Arguments.MemoryBytes() + frame.Fallback.MemoryBytes();
        }
        const auto& asset = MeshletManager::AssetData();
        uint64_t metadataBytes = asset.Meshlets.size() * sizeof(Meshlets::Meshlet) +
                                 asset.Bounds.size() * sizeof(Meshlets::Bounds) +
                                 asset.Clusters.size() * sizeof(Meshlets::Cluster);
        uint64_t dataBytes = metadataBytes + (asset.Vertices.size() + asset.Primitives.size()) * 4;
        ImGui::Text("Metadata / meshlet asset data: %.2f / %.2f MiB", double(metadataBytes) / (1024 * 1024),
                    double(dataBytes) / (1024 * 1024));
        ImGui::Text("All frame scratch memory: %.2f MiB", double(scratchBytes) / (1024 * 1024));
        ImGui::Checkbox("Meshlet debug readback (stalls GPU)", &s.Options.DebugReadback);
        if (s.Options.DebugReadback)
        {
            ImGui::Text("Candidate %u Visible %u Cluster %u Visible cluster %u", s.Debug[11], s.Debug[1], s.Debug[2],
                        s.Debug[3]);
            ImGui::Text("Frustum %u Cone %u Occlusion %u Cluster-culled %u", s.Debug[4], s.Debug[5], s.Debug[6],
                        s.Debug[10]);
            ImGui::Text("Vertices %u Primitives %u Overflow fallback objects %u", s.Debug[7], s.Debug[8], s.Debug[9]);
        }
        for (const auto& pass : RenderProfiler::GetLatest().Passes)
        {
            if (pass.Name == "Cluster Culling" || pass.Name == "Meshlet Culling" ||
                pass.Name == "Mesh Dispatch Generation" || pass.Name == "Mesh Shader Opaque")
            {
                ImGui::Text("%s: %.4f GPU ms", pass.Name.c_str(), pass.GpuMs);
            }
        }
        if (s.Options.MeshletBounds || s.Options.ClusterBounds || s.Options.ConeAxes)
        {
            ImGui::TextDisabled("Preview requires debug readback; first 4096 candidates. Cyan visible, purple "
                                "cluster, red frustum, yellow cone, orange Hi-Z.");
            auto* draw = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetCursorScreenPos(), size(320, 180);
            draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(100, 100, 100, 255));
            const auto& assets = MeshletManager::AssetData();
            XMMATRIX vp = XMLoadFloat4x4(&s.Parameters.VP);
            auto project = [&](XMVECTOR world, ImVec2& output) {
                XMFLOAT4 clip;
                XMStoreFloat4(&clip, XMVector4Transform(world, vp));
                if (clip.w <= 1e-5 || clip.z <= 0)
                {
                    return false;
                }
                float x = clip.x / clip.w * 0.5f + 0.5f, y = 0.5f - clip.y / clip.w * 0.5f;
                if (x < 0 || x > 1 || y < 0 || y > 1)
                {
                    return false;
                }
                output = ImVec2(origin.x + x * size.x, origin.y + y * size.y);
                return true;
            };
            const ImU32 colors[] = {IM_COL32(150, 150, 150, 160), IM_COL32(40, 230, 230, 160),
                                    IM_COL32(200, 80, 240, 160),  IM_COL32(250, 60, 60, 160),
                                    IM_COL32(240, 220, 40, 160),  IM_COL32(250, 150, 40, 160)};
            for (const auto& record : s.DebugRecords)
            {
                auto* instance = GpuScene::GetInstance(record[0]);
                if (!instance || record[1] >= assets.Bounds.size())
                {
                    continue;
                }
                XMMATRIX world = XMLoadFloat4x4(&instance->World);
                auto sphere = [&](const Meshlets::Bounds& bounds, ImU32 color) {
                    for (uint32_t axis = 0; axis < 3; ++axis)
                    {
                        ImVec2 previous;
                        bool have = false;
                        for (uint32_t step = 0; step <= 24; ++step)
                        {
                            float angle = XM_2PI * float(step) / 24;
                            float xyz[] = {bounds.Center.X, bounds.Center.Y, bounds.Center.Z};
                            xyz[axis] += cosf(angle) * bounds.Radius;
                            xyz[(axis + 1) % 3] += sinf(angle) * bounds.Radius;
                            ImVec2 current;
                            bool valid =
                                project(XMVector4Transform(XMVectorSet(xyz[0], xyz[1], xyz[2], 1), world), current);
                            if (have && valid)
                            {
                                draw->AddLine(previous, current, color);
                            }
                            previous = current;
                            have = valid;
                        }
                    }
                };
                auto color = colors[(min)(record[2], 5u)];
                const auto& bounds = assets.Bounds[record[1]];
                if (s.Options.MeshletBounds)
                {
                    sphere(bounds, color);
                }
                if (s.Options.ClusterBounds && record[3] < assets.Clusters.size() &&
                    record[1] == assets.Clusters[record[3]].MeshletOffset)
                {
                    sphere(assets.Clusters[record[3]].Bounds, IM_COL32(190, 190, 255, 180));
                }
                if (s.Options.ConeAxes && bounds.ConeCutoff < 1)
                {
                    ImVec2 a, b;
                    auto center = XMVectorSet(bounds.Center.X, bounds.Center.Y, bounds.Center.Z, 1);
                    auto end = center +
                               XMVectorSet(bounds.ConeAxis.X, bounds.ConeAxis.Y, bounds.ConeAxis.Z, 0) * bounds.Radius;
                    if (project(XMVector4Transform(center, world), a) && project(XMVector4Transform(end, world), b))
                    {
                        draw->AddLine(a, b, IM_COL32(255, 255, 255, 220), 2);
                    }
                }
            }
            ImGui::Dummy(size);
        }
    }

    void EndFrame()
    {
        VirtualGeometryTraversal::EndFrame();
        Data().DebugRecords.clear();
        const auto& options = Data().Options;
        if (!Data().Active ||
            (!options.DebugReadback && !options.MeshletBounds && !options.ClusterBounds && !options.ConeAxes))
        {
            Data().DebugRecords.clear();
            return;
        }
        vector<uint8_t> bytes;
        if (RenderCapture::ReadBuffer(Current().Counters.Resource(), Current().Counters.State, 64, bytes))
        {
            memcpy(Data().Debug.data(), bytes.data(), 64);
            uint32_t samples = (min)(Data().Debug[0], 4096u);
            if (samples && RenderCapture::ReadBuffer(Current().Candidates.Resource(), Current().Candidates.State,
                                                     uint64_t(samples) * 16, bytes))
            {
                Data().DebugRecords.resize(samples);
                memcpy(Data().DebugRecords.data(), bytes.data(), bytes.size());
            }
            else
            {
                Data().DebugRecords.clear();
            }
        }
    }

    const vector<array<uint32_t, 4>>& DebugRecords()
    {
        return Data().DebugRecords;
    }

    ID3D12Resource* VisibleBuffer()
    {
        return Current().Visible.Resource();
    }

    ID3D12Resource* StatisticsBuffer()
    {
        return Current().Counters.Resource();
    }

    NativeRoutingSample CaptureNativeRouting()
    {
        NativeRoutingSample result;
        auto fail = [&](const char* message) {
            result.Error = message;
            return result;
        };
        auto read = [](GpuSceneBuffer& source, uint64_t bytes, std::vector<uint8_t>& output) {
            return RenderCapture::ReadBuffer(source.Resource(), source.State, bytes, output);
        };
        auto word = [](const std::vector<uint8_t>& bytes, size_t offset) {
            uint32_t value;
            std::memcpy(&value, bytes.data() + offset, 4);
            return value;
        };
        std::vector<uint8_t> counters, records, success, fallback, legacyCounters, legacyRecords;
        if (!read(VirtualGeometryTraversal::CounterBuffer(), 256, counters))
        {
            return fail("Native counters unavailable");
        }
        result.SelectedClusters = word(counters, 4);
        result.LeafClusters = word(counters, 32);
        result.SuccessfulInstances = word(counters, 16);
        if (result.SelectedClusters > VirtualGeometryTraversal::VisibleBuffer().Capacity())
        {
            return fail("Native count exceeds capacity");
        }
        if (result.SelectedClusters &&
            !read(VirtualGeometryTraversal::VisibleBuffer(), uint64_t(result.SelectedClusters) * 16, records))
        {
            return fail("Native records unavailable");
        }
        std::unordered_set<uint64_t> selected;
        std::unordered_set<uint32_t> nativeInstances;
        const auto& assets = VirtualGeometryResources::AssetTable();
        const auto& metadata = VirtualGeometryResources::Metadata();
        std::vector<VisibleVirtualCluster> visible(result.SelectedClusters);
        for (uint32_t index = 0; index < result.SelectedClusters; ++index)
        {
            std::memcpy(&visible[index], records.data() + size_t(index) * 16, 16);
            const auto& record = visible[index];
            if (record.Flags >= assets.size())
            {
                return fail("Native record asset out of range");
            }
            const auto& asset = assets[record.Flags];
            if (record.ClusterIndex < asset.ClusterOffset ||
                uint64_t(record.ClusterIndex) >= uint64_t(asset.ClusterOffset) + asset.ClusterCount)
            {
                return fail("Native record cluster outside asset");
            }
            uint64_t key = (uint64_t(record.InstanceIndex) << 32) | record.ClusterIndex;
            if (!selected.insert(key).second)
            {
                return fail("Duplicate native cluster record");
            }
            nativeInstances.insert(record.InstanceIndex);
        }
        for (const auto& record : visible)
        {
            const auto& asset = assets[record.Flags];
            std::vector<uint32_t> queue{record.ClusterIndex};
            std::unordered_set<uint32_t> visited;
            for (size_t cursor = 0; cursor < queue.size(); ++cursor)
            {
                uint32_t group = metadata.Clusters[queue[cursor]].GroupIndex;
                if (group == UINT32_MAX)
                {
                    continue;
                }
                const auto& node = metadata.Groups[asset.GroupOffset + group];
                for (uint32_t parent = 0; parent < node.ParentCount; ++parent)
                {
                    uint32_t ancestor =
                        asset.ClusterOffset +
                        metadata.HierarchyIndices[asset.HierarchyIndexOffset + node.FirstParent + parent];
                    uint64_t key = (uint64_t(record.InstanceIndex) << 32) | ancestor;
                    if (selected.contains(key))
                    {
                        return fail("Native parent and descendant selected together");
                    }
                    if (visited.insert(ancestor).second)
                    {
                        queue.push_back(ancestor);
                    }
                }
            }
        }
        uint32_t capacity = Data().ObjectCapacity;
        if (!read(VirtualGeometryTraversal::SuccessBuffer(), uint64_t(capacity) * 4, success) ||
            !read(Current().Fallback, uint64_t(capacity) * 24, fallback))
        {
            return fail("Native fallback routing unavailable");
        }
        for (uint32_t object = 0; object < capacity; ++object)
        {
            if (word(success, size_t(object) * 4) && word(fallback, size_t(object) * 24 + 4))
            {
                return fail("Successful native object still has fallback draw indices");
            }
        }
        if (!read(Current().Counters, 64, legacyCounters))
        {
            return fail("Legacy counters unavailable");
        }
        uint32_t legacyCount = word(legacyCounters, 4);
        if (legacyCount > Current().Visible.Capacity())
        {
            return fail("Legacy count exceeds capacity");
        }
        if (legacyCount && !read(Current().Visible, uint64_t(legacyCount) * 8, legacyRecords))
        {
            return fail("Legacy records unavailable");
        }
        for (uint32_t index = 0; index < legacyCount; ++index)
        {
            if (nativeInstances.contains(word(legacyRecords, size_t(index) * 8)))
            {
                return fail("Native instance also emitted legacy meshlets");
            }
        }
        return result;
    }

    void Shutdown()
    {
        VirtualGeometryBinning::Shutdown();
        VirtualGeometryTraversal::Shutdown();
        MeshletManager::Shutdown();
        Data() = {};
    }
} // 名前空間 MeshShaderRenderer
