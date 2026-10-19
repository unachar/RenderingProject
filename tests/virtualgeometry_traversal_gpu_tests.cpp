// 本番の走査シェーダーをD3D12で単独実行し、CPUの参照実装と比較する。
// コンピュート処理には、アプリのシーンやMesh Shaderの対応階層を必要としない。
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <DirectXMath.h>
#include "d3dx12.h"
using std::array;
#include "rendering/gpuscene/gpuscenetypes.h"
#include "virtualgeometry/virtualgeometrygpu.h"
#include "virtualgeometry/virtualgeometryselection.h"
#include "virtualgeometry_selection_fixture.h"
using Microsoft::WRL::ComPtr;

namespace
{
    void Check(HRESULT result, const char* message)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(message);
        }
    }

    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    struct Constants
    {
        DirectX::XMFLOAT4X4 VP{}, PreviousVP{};
        DirectX::XMFLOAT4 Camera{};
        uint32_t Capacity = 16, ObjectCapacity = 1, Assets = 1, Materials = 1;
        uint32_t Frustum = 0, Cone = 0, Occlusion = 0, Enabled = 1;
        float Bias = 0.001f, Width = 1, Height = 1;
        uint32_t Mips = 1;
        float ProjectionPixels = 10, TargetPixels = 1;
        int32_t ForceLevel = -1;
        uint32_t StateCapacity = 64;
        uint32_t PageCount = 0, PageRequestCapacity = 65536, StreamingPadding[2]{};
        uint32_t HistorySlots = 0, HistoryCapacity = 0, ResetHistory = 0, TwoPass = 0;
        float HysteresisFraction = 0;
        uint32_t HysteresisEnabled = 0, TemporalPhase = 0, Orthographic = 0;
    };

    static_assert(sizeof(Constants) == 256);
    static_assert(offsetof(Constants, Orthographic) == 252);

    template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, class T> struct alignas(void*) Subobject
    {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Tag = Type;
        T Value{};
    };

    struct RasterStream
    {
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> Root;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> MS;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> PS;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> Blend;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> Raster;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> Depth;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> Targets;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> Samples;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, uint32_t> Mask;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> Topology;
    };

    class Harness
    {
      public:
        explicit Harness(bool warp)
        {
            ComPtr<IDXGIFactory4> factory;
            Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
            if (warp)
            {
                Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&m_Adapter)), "Get WARP adapter");
            }
            else
            {
                Check(factory->EnumAdapters1(0, &m_Adapter), "Get hardware adapter");
            }
            DXGI_ADAPTER_DESC1 adapterDescription{};
            m_Adapter->GetDesc1(&adapterDescription);
            std::wcout << L"Traversal validation device: " << adapterDescription.Description << L'\n';
            Check(D3D12CreateDevice(m_Adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_Device)),
                  "Create D3D12 device");
            D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
            m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options));
            std::cout << "Mesh Shader tier: " << unsigned(options.MeshShaderTier) << '\n';
            m_MeshSupported = options.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
            D3D12_COMMAND_QUEUE_DESC queue{};
            Check(m_Device->CreateCommandQueue(&queue, IID_PPV_ARGS(&m_Queue)), "Create command queue");
            Check(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_Allocator)),
                  "Create allocator");
            Check(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&m_List)),
                  "Create list");
            Check(m_List->Close(), "Close initial list");
            Check(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence)), "Create fence");
            m_Event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            Require(m_Event != nullptr, "Create fence event");
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heap.NumDescriptors = 1;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Check(m_Device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_Heap)), "Create null Hi-Z heap");
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R32_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            // 画面全体を覆う手前の遮蔽物を表す、保守的なHi-Zテストデータ。
            auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, 1, 1);
            auto textureHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            Check(m_Device->CreateCommittedResource(&textureHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_HiZ)),
                  "Create test Hi-Z");
            std::array<float, 64> depths{};
            auto upload = Upload(depths.data(), sizeof(depths));
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            footprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
            footprint.Footprint.Width = footprint.Footprint.Height = footprint.Footprint.Depth = 1;
            footprint.Footprint.RowPitch = 256;
            Check(m_Allocator->Reset(), "Reset Hi-Z allocator");
            Check(m_List->Reset(m_Allocator.Get(), nullptr), "Reset Hi-Z list");
            CD3DX12_TEXTURE_COPY_LOCATION target(m_HiZ.Get(), 0), source(upload.Get(), footprint);
            m_List->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            auto transition = CD3DX12_RESOURCE_BARRIER::Transition(m_HiZ.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m_List->ResourceBarrier(1, &transition);
            Check(m_List->Close(), "Close Hi-Z list");
            ID3D12CommandList* command = m_List.Get();
            m_Queue->ExecuteCommandLists(1, &command);
            Check(m_Queue->Signal(m_Fence.Get(), ++m_FenceValue), "Signal Hi-Z fence");
            Check(m_Fence->SetEventOnCompletion(m_FenceValue, m_Event), "Set Hi-Z event");
            Require(WaitForSingleObject(m_Event, 30000) == WAIT_OBJECT_0, "Hi-Z upload timed out");
            m_Device->CreateShaderResourceView(m_HiZ.Get(), &srv, m_Heap->GetCPUDescriptorHandleForHeapStart());
            D3D12_ROOT_PARAMETER parameters[29]{};
            for (uint32_t index = 0; index < 9; ++index)
            {
                parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                parameters[index].Descriptor.ShaderRegister = index;
            }
            for (uint32_t index = 0; index < 6; ++index)
            {
                parameters[9 + index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
                parameters[9 + index].Descriptor.ShaderRegister = index;
            }
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;
            range.BaseShaderRegister = 9;
            parameters[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[15].DescriptorTable = {1, &range};
            parameters[16].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[17].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[17].Descriptor.ShaderRegister = 10;
            for (uint32_t index = 0; index < 3; ++index)
            {
                parameters[18 + index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
                parameters[18 + index].Descriptor.ShaderRegister = 6 + index;
            }
            parameters[21].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameters[21].Descriptor.ShaderRegister = 9;
            parameters[22].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[22].Descriptor.ShaderRegister = 11;
            for (uint32_t index = 0; index < 6; ++index)
            {
                parameters[23 + index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
                parameters[23 + index].Descriptor.ShaderRegister = 10 + index;
            }
            D3D12_ROOT_SIGNATURE_DESC root{};
            root.NumParameters = 29;
            root.pParameters = parameters;
            ComPtr<ID3DBlob> blob, error;
            Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "Serialize root");
            Check(m_Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                IID_PPV_ARGS(&m_Root)),
                  "Create root");
            m_Reset = Pipeline("VirtualGeometryResetCS");
            m_Traverse = Pipeline("VirtualGeometryTraversalCS");
            m_Arguments = Pipeline("VirtualGeometryArgumentsCS");
        }

        ~Harness()
        {
            if (m_Event)
            {
                CloseHandle(m_Event);
            }
        }

        void Run(const VirtualGeometryAsset& graph, Constants constants, bool eligible = true, bool masked = false,
                 bool allCulled = false, const std::vector<uint8_t>& resident = {})
        {
            GPUInstanceData instance;
            DirectX::XMStoreFloat4x4(&instance.World, DirectX::XMMatrixIdentity());
            instance.PreviousWorld = instance.World;
            instance.Flags = eligible ? 4 : 0;
            instance.VirtualGeometryAssetIndex = 0;
            std::vector<VirtualGeometryGPUCluster> clusters;
            for (const auto& source : graph.Clusters)
            {
                VirtualGeometryGPUCluster cluster;
                cluster.Level = source.Level;
                cluster.GroupIndex = source.GroupIndex;
                cluster.GeneratingGroupIndex = source.GeneratingGroupIndex;
                cluster.LodError = source.LodError;
                cluster.Bounds = {source.Bounds.Center[0], source.Bounds.Center[1], source.Bounds.Center[2],
                                  source.Bounds.Radius};
                cluster.Cone = {source.Cone.Axis[0], source.Cone.Axis[1], source.Cone.Axis[2], source.Cone.Cutoff};
                cluster.ConeApex = {source.Cone.Apex[0], source.Cone.Apex[1], source.Cone.Apex[2], 0};
                clusters.push_back(cluster);
            }
            std::vector<uint32_t> clusterPages(clusters.size());
            std::vector<std::array<uint32_t, 4>> residency(std::max(size_t(1), resident.size() + 7));
            if (!resident.empty())
            {
                constants.PageCount = uint32_t(residency.size());
                for (uint32_t index = 0; index < clusters.size(); ++index)
                {
                    clusters[index].PageIndex = index;
                    clusterPages[index] = index;
                    residency[index + 7] = {index, 1, resident.at(index), 0};
                }
            }
            std::vector<VirtualGeometryGPUGroup> groups;
            for (const auto& source : graph.Groups)
            {
                VirtualGeometryGPUGroup group;
                group.FirstChild = source.FirstChild;
                group.ChildCount = source.ChildCount;
                group.FirstParent = source.FirstParent;
                group.ParentCount = source.ParentCount;
                group.Level = source.Level;
                group.MaxError = source.MaxError;
                group.Bounds = {source.Bounds.Center[0], source.Bounds.Center[1], source.Bounds.Center[2],
                                source.Bounds.Radius};
                groups.push_back(group);
            }
            VirtualGeometryGPUAsset asset;
            asset.ClusterCount = uint32_t(clusters.size());
            asset.GroupCount = uint32_t(groups.size());
            asset.RootCount = uint32_t(graph.RootClusters.size());
            asset.Flags = 1;
            for (const auto& cluster : clusters)
            {
                asset.Flags = std::max(asset.Flags, 1u | (cluster.Level << 8));
            }
            if (!resident.empty())
            {
                asset.Flags |= 2u;
                asset.VertexOffset = 7;
                asset.TriangleOffset = uint32_t(resident.size());
            }
            std::array<uint32_t, 4> object{};
            uint32_t count = 1;
            std::array<uint32_t, 656> material{};
            if (masked)
            {
                material[(64 + 149 * 16 + 12) / 4] = 1;
            }
            std::array<ComPtr<ID3D12Resource>, 9> sources{
                Upload(&instance, sizeof(instance)),
                Upload(object.data(), sizeof(object)),
                Upload(&asset, sizeof(asset)),
                Upload(clusters.data(), clusters.size() * sizeof(clusters[0])),
                Upload(groups.data(), groups.size() * sizeof(groups[0])),
                Upload(graph.HierarchyIndices.data(), graph.HierarchyIndices.size() * 4),
                Upload(graph.RootClusters.data(), graph.RootClusters.size() * 4),
                Upload(&count, 4),
                Upload(material.data(), sizeof(material))};
            auto constant = Upload(&constants, sizeof(constants));
            auto residencySource = Upload(residency.data(), residency.size() * 16);
            std::array<ComPtr<ID3D12Resource>, 9> outputs;
            uint64_t visibleBytes = std::max(uint64_t(1024), uint64_t(constants.Capacity) * 16);
            uint64_t sizes[] = {std::max(uint64_t(1024), uint64_t(constants.StateCapacity) * 4),
                                visibleBytes,
                                1024,
                                1024,
                                1024,
                                std::max(uint64_t(1024), uint64_t(constants.StateCapacity) * 4),
                                1024,
                                16ull + 65536ull * 4,
                                1024};
            for (uint32_t index = 0; index < 9; ++index)
            {
                outputs[index] = Buffer(sizes[index], D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
            // 従来の正解データを使うケースでは時間方向の選択を無効にする。このテストで
            // 追加のバインディングにアクセスするのは、カウンターのリセット処理だけ。
            auto temporal = Buffer(4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto readback = Buffer(2048 + visibleBytes + sizes[7] + 1024, D3D12_HEAP_TYPE_READBACK,
                                   D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
            Check(m_Allocator->Reset(), "Reset allocator");
            Check(m_List->Reset(m_Allocator.Get(), nullptr), "Reset list");
            m_List->SetComputeRootSignature(m_Root.Get());
            ID3D12DescriptorHeap* heap = m_Heap.Get();
            m_List->SetDescriptorHeaps(1, &heap);
            for (uint32_t index = 0; index < 9; ++index)
            {
                m_List->SetComputeRootShaderResourceView(index, sources[index]->GetGPUVirtualAddress());
            }
            for (uint32_t index = 0; index < 6; ++index)
            {
                m_List->SetComputeRootUnorderedAccessView(9 + index, outputs[index]->GetGPUVirtualAddress());
            }
            m_List->SetComputeRootDescriptorTable(15, m_Heap->GetGPUDescriptorHandleForHeapStart());
            m_List->SetComputeRootConstantBufferView(16, constant->GetGPUVirtualAddress());
            m_List->SetComputeRootShaderResourceView(17, residencySource->GetGPUVirtualAddress());
            for (uint32_t index = 0; index < 3; ++index)
            {
                m_List->SetComputeRootUnorderedAccessView(18 + index, outputs[6 + index]->GetGPUVirtualAddress());
            }
            m_List->SetPipelineState(m_Reset.Get());
            m_List->SetComputeRootUnorderedAccessView(21, temporal->GetGPUVirtualAddress());
            m_List->SetComputeRootShaderResourceView(22, sources[0]->GetGPUVirtualAddress());
            for (uint32_t index = 0; index < 6; ++index)
            {
                m_List->SetComputeRootUnorderedAccessView(23 + index, temporal->GetGPUVirtualAddress());
            }
            m_List->Dispatch(std::max(1u, (((constants.PageCount + 31) / 32) + 63) / 64), 1, 1);
            Barrier();
            m_List->SetPipelineState(m_Traverse.Get());
            m_List->Dispatch(1, 1, 1);
            Barrier();
            m_List->SetPipelineState(m_Arguments.Get());
            m_List->Dispatch(1, 1, 1);
            Barrier();
            const uint32_t copied[] = {2, 4, 1};
            for (uint32_t index = 0; index < 3; ++index)
            {
                D3D12_RESOURCE_BARRIER transition{};
                transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                transition.Transition.pResource = outputs[copied[index]].Get();
                transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                m_List->ResourceBarrier(1, &transition);
                m_List->CopyBufferRegion(readback.Get(), index * 1024, outputs[copied[index]].Get(), 0,
                                         index == 2 ? visibleBytes : 1024);
            }
            for (uint32_t index : {7u, 8u})
            {
                auto transition = CD3DX12_RESOURCE_BARRIER::Transition(
                    outputs[index].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                m_List->ResourceBarrier(1, &transition);
                uint64_t offset = 2048 + visibleBytes;
                if (index == 8)
                {
                    offset += sizes[7];
                }
                m_List->CopyBufferRegion(readback.Get(), offset, outputs[index].Get(), 0, sizes[index]);
            }
            Check(m_List->Close(), "Close compute list");
            ID3D12CommandList* command = m_List.Get();
            m_Queue->ExecuteCommandLists(1, &command);
            Check(m_Queue->Signal(m_Fence.Get(), ++m_FenceValue), "Signal compute fence");
            Check(m_Fence->SetEventOnCompletion(m_FenceValue, m_Event), "Set compute event");
            Require(WaitForSingleObject(m_Event, 30000) == WAIT_OBJECT_0, "GPU traversal timed out");
            Check(m_Device->GetDeviceRemovedReason(), "GPU device removed");
            void* bytes = nullptr;
            D3D12_RANGE range{0, SIZE_T(2048 + visibleBytes + sizes[7] + 1024)};
            Check(readback->Map(0, &range, &bytes), "Map compute results");
            const auto* counters = static_cast<const uint32_t*>(bytes);
            const auto* success = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(bytes) + 1024);
            const auto* visible =
                reinterpret_cast<const VisibleVirtualCluster*>(static_cast<const uint8_t*>(bytes) + 2048);
            VirtualGeometrySelection::View view;
            view.Camera = {constants.Camera.x, constants.Camera.y, constants.Camera.z};
            view.ProjectionScalePixels = constants.ProjectionPixels;
            view.Orthographic = constants.Orthographic != 0;
            VirtualGeometrySelection::Settings settings;
            settings.TargetErrorPixels = constants.TargetPixels;
            settings.ForceHierarchyLevel = constants.ForceLevel;
            auto reference = VirtualGeometrySelection::Select(graph, view, settings);
            std::vector<uint32_t> expectedRequests;
            if (!resident.empty())
            {
                auto streamed = VirtualGeometrySelection::SelectResident(graph, view, settings, clusterPages, resident);
                reference.Clusters = streamed.Clusters;
                reference.Error = streamed.Error;
                expectedRequests = streamed.RequestedPages;
                const auto* requested =
                    reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(bytes) + 2048 + visibleBytes);
                Require(requested[0] == expectedRequests.size(), "GPU Page request count mismatch");
                Require(requested[1] == expectedRequests.size() -
                                            std::min(size_t(constants.PageRequestCapacity), expectedRequests.size()),
                        "GPU request overflow mismatch");
                for (uint32_t index = 0; index < std::min(requested[0], constants.PageRequestCapacity); ++index)
                {
                    Require(std::find(expectedRequests.begin(), expectedRequests.end(), requested[4 + index] - 7) !=
                                expectedRequests.end(),
                            "GPU requested an unreachable Page");
                }
            }
            else
            {
                Require(reference.Error.empty(), reference.Error.c_str());
            }
            if (allCulled)
            {
                reference.Clusters.clear();
            }
            bool expectedSuccess = reference.Error.empty() && eligible && !masked && constants.Enabled &&
                                   constants.StateCapacity >= clusters.size() &&
                                   constants.Capacity >= reference.Clusters.size();
            Require((success[0] != 0) == expectedSuccess, "GPU fallback routing mismatch");
            if (expectedSuccess)
            {
                std::vector<uint32_t> actual;
                Require(counters[1] == reference.Clusters.size(), "GPU selected cluster count mismatch");
                for (uint32_t index = 0; index < counters[1]; ++index)
                {
                    actual.push_back(visible[index].ClusterIndex);
                }
                std::sort(actual.begin(), actual.end());
                std::sort(reference.Clusters.begin(), reference.Clusters.end());
                Require(actual == reference.Clusters, "GPU hierarchy cut differs from CPU reference");
            }
            else
            {
                Require(counters[1] == 0, "Fallback published a partial native instance");
            }
            D3D12_RANGE written{0, 0};
            readback->Unmap(0, &written);
            ++m_Cases;
        }

        uint32_t Cases() const
        {
            return m_Cases;
        }

        void RasterNativeParent(bool paged = false, bool velocity = false)
        {
            if (!m_MeshSupported)
            {
                std::cout << "SKIP: native raster requires Mesh Shader support\n";
                return;
            }
            D3D12_ROOT_PARAMETER parameters[13]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[1].Constants = {6, 0, 1};
            for (uint32_t index = 0; index < 9; ++index)
            {
                parameters[2 + index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                parameters[2 + index].Descriptor.ShaderRegister = 30 + index;
            }
            parameters[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[11].Descriptor.ShaderRegister = 39;
            D3D12_DESCRIPTOR_RANGE pageRange{};
            pageRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            pageRange.NumDescriptors = 16384;
            pageRange.RegisterSpace = 2;
            parameters[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[12].DescriptorTable = {1, &pageRange};
            D3D12_ROOT_SIGNATURE_DESC rootDesc{};
            rootDesc.NumParameters = 13;
            rootDesc.pParameters = parameters;
            ComPtr<ID3DBlob> blob, error;
            Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
                  "Serialize native raster root");
            ComPtr<ID3D12RootSignature> root;
            Check(
                m_Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
                "Create native raster root");
            auto ms = ReadShader(velocity ? "VirtualGeometryVelocityMS" : "VirtualGeometryMS");
            auto ps = ReadShader(velocity ? "VirtualGeometryVelocityValidationPS" : "VirtualGeometryValidationPS");
            RasterStream stream;
            stream.Root.Value = root.Get();
            stream.MS.Value = {ms.data(), ms.size()};
            stream.PS.Value = {ps.data(), ps.size()};
            stream.Blend.Value = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            stream.Raster.Value = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            stream.Raster.Value.CullMode = D3D12_CULL_MODE_NONE;
            stream.Depth.Value = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            stream.Depth.Value.DepthEnable = FALSE;
            stream.Depth.Value.StencilEnable = FALSE;
            stream.Targets.Value.NumRenderTargets = 1;
            stream.Targets.Value.RTFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
            stream.Samples.Value = {1, 0};
            stream.Mask.Value = UINT32_MAX;
            stream.Topology.Value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            ComPtr<ID3D12Device2> device2;
            Check(m_Device.As(&device2), "Query mesh-capable device");
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
            ComPtr<ID3D12PipelineState> pso;
            Check(device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso)), "Create production native raster PSO");
            GPUInstanceData instance;
            DirectX::XMStoreFloat4x4(&instance.World, DirectX::XMMatrixIdentity());
            instance.PreviousWorld = instance.World;
            if (velocity)
            {
                DirectX::XMStoreFloat4x4(&instance.PreviousWorld, DirectX::XMMatrixTranslation(-0.2f, 0, 0));
            }
            std::array<VirtualGeometryGPUCluster, 2> clusters{};
            clusters[1].Level = 3;
            clusters[1].VertexOffset = 1;
            clusters[1].VertexCount = 3;
            clusters[1].TriangleOffset = 1;
            clusters[1].TriangleCount = 1;
            VirtualGeometryGPUAsset asset;
            asset.VertexOffset = 1;
            asset.TriangleOffset = 4;
            std::array<VirtualGeometryVertex, 5> vertices{};
            vertices[2].Position[0] = -0.7f;
            vertices[2].Position[1] = -0.7f;
            vertices[3].Position[1] = 0.7f;
            vertices[4].Position[0] = 0.7f;
            vertices[4].Position[1] = -0.7f;
            for (auto& vertex : vertices)
            {
                vertex.Position[2] = 0.5f;
                vertex.Normal[2] = -1;
                vertex.Diffuse[3] = 7;
            }
            const std::array<uint8_t, 8> triangles{0, 0, 0, 0, 0, 0, 1, 2};
            std::vector<uint8_t> page(65536);
            std::memcpy(page.data() + 32, vertices.data() + 2, 3 * sizeof(VirtualGeometryVertex));
            page[200] = 0;
            page[201] = 1;
            page[202] = 2;
            if (paged)
            {
                asset.Flags = 3;
                asset.VertexOffset = 7;
                asset.TriangleOffset = 4;
                clusters[1].PageIndex = 3;
                clusters[1].VertexOffset = 32;
                clusters[1].TriangleOffset = 200;
            }
            std::array<std::array<uint32_t, 4>, 11> residency{};
            residency[10] = {1, 1, 1, 0};
            auto residencySource = Upload(residency.data(), sizeof(residency));
            auto pageSource = Upload(page.data(), page.size());
            D3D12_DESCRIPTOR_HEAP_DESC pageHeapDescription{};
            pageHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            pageHeapDescription.NumDescriptors = 16384;
            pageHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            ComPtr<ID3D12DescriptorHeap> pageHeap;
            Check(m_Device->CreateDescriptorHeap(&pageHeapDescription, IID_PPV_ARGS(&pageHeap)), "Page raster heap");
            D3D12_SHADER_RESOURCE_VIEW_DESC pageView{};
            pageView.Format = DXGI_FORMAT_R32_TYPELESS;
            pageView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            pageView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            pageView.Buffer.NumElements = 65536 / 4;
            pageView.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            auto pageCpu = pageHeap->GetCPUDescriptorHandleForHeapStart();
            pageCpu.ptr += m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            m_Device->CreateShaderResourceView(pageSource.Get(), &pageView, pageCpu);
            VisibleVirtualCluster selected;
            selected.ClusterIndex = 1;
            const std::array<uint32_t, 2> counters{0, 1};
            uint32_t dummy = 0;
            std::array<uint32_t, 16> dummyMaterial{};
            std::array<ComPtr<ID3D12Resource>, 9> inputs{Upload(&instance, sizeof(instance)),
                                                         Upload(dummyMaterial.data(), sizeof(dummyMaterial)),
                                                         Upload(&dummy, 4),
                                                         Upload(clusters.data(), sizeof(clusters)),
                                                         Upload(&asset, sizeof(asset)),
                                                         Upload(triangles.data(), sizeof(triangles)),
                                                         Upload(vertices.data(), sizeof(vertices)),
                                                         Upload(&selected, sizeof(selected)),
                                                         Upload(counters.data(), sizeof(counters))};
            std::array<float, 128> matrices{};
            for (uint32_t offset : {0u, 16u, 32u, 68u, 84u})
            {
                for (uint32_t diagonal = 0; diagonal < 4; ++diagonal)
                {
                    matrices[offset + diagonal * 5] = 1;
                }
            }
            auto constants = Upload(matrices.data(), sizeof(matrices));
            auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, 128, 128, 1, 1, 1, 0,
                                                            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            ComPtr<ID3D12Resource> texture;
            Check(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                    IID_PPV_ARGS(&texture)),
                  "Create raster target");
            D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
            rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvDesc.NumDescriptors = 1;
            ComPtr<ID3D12DescriptorHeap> rtvHeap;
            Check(m_Device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)), "Create raster RTV heap");
            auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            m_Device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            uint64_t byteCount;
            m_Device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &byteCount);
            auto readback =
                Buffer(byteCount, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
            Check(m_Allocator->Reset(), "Reset raster allocator");
            Check(m_List->Reset(m_Allocator.Get(), nullptr), "Reset raster list");
            m_List->SetGraphicsRootSignature(root.Get());
            ID3D12DescriptorHeap* rasterHeap = pageHeap.Get();
            m_List->SetDescriptorHeaps(1, &rasterHeap);
            m_List->SetGraphicsRootShaderResourceView(11, residencySource->GetGPUVirtualAddress());
            m_List->SetGraphicsRootDescriptorTable(12, pageHeap->GetGPUDescriptorHandleForHeapStart());
            m_List->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
            m_List->SetGraphicsRoot32BitConstant(1, 1, 0);
            for (uint32_t index = 0; index < 9; ++index)
            {
                m_List->SetGraphicsRootShaderResourceView(2 + index, inputs[index]->GetGPUVirtualAddress());
            }
            m_List->SetPipelineState(pso.Get());
            m_List->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            float clear[4]{};
            m_List->ClearRenderTargetView(rtv, clear, 0, nullptr);
            D3D12_VIEWPORT viewport{0, 0, 128, 128, 0, 1};
            D3D12_RECT rect{0, 0, 128, 128};
            m_List->RSSetViewports(1, &viewport);
            m_List->RSSetScissorRects(1, &rect);
            ComPtr<ID3D12GraphicsCommandList6> meshList;
            Check(m_List.As(&meshList), "Query mesh command list");
            meshList->DispatchMesh(1, 1, 1);
            auto transition = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
            m_List->ResourceBarrier(1, &transition);
            CD3DX12_TEXTURE_COPY_LOCATION destination(readback.Get(), footprint), source(texture.Get(), 0);
            m_List->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            Check(m_List->Close(), "Close raster list");
            ID3D12CommandList* command = m_List.Get();
            m_Queue->ExecuteCommandLists(1, &command);
            Check(m_Queue->Signal(m_Fence.Get(), ++m_FenceValue), "Signal raster fence");
            Check(m_Fence->SetEventOnCompletion(m_FenceValue, m_Event), "Set raster fence event");
            Require(WaitForSingleObject(m_Event, 30000) == WAIT_OBJECT_0, "Native raster timeout");
            Check(m_Device->GetDeviceRemovedReason(), "Raster device removed");
            void* bytes = nullptr;
            D3D12_RANGE range{0, SIZE_T(byteCount)};
            Check(readback->Map(0, &range, &bytes), "Map native raster");
            auto pixel = reinterpret_cast<const float*>(static_cast<const uint8_t*>(bytes) +
                                                        64 * footprint.Footprint.RowPitch + 64 * 16);
            if (velocity)
            {
                Require(std::abs(pixel[0] - 0.2f) < 0.001f && std::abs(pixel[1]) < 0.001f && pixel[2] == 1 &&
                            pixel[3] == 1,
                        "Production native velocity differs from current/previous transform reference");
            }
            else
            {
                Require(std::abs(pixel[0] - 1) < 0.01f && std::abs(pixel[1] - 0.9f) < 0.01f &&
                            std::abs(pixel[2] - 0.1f) < 0.01f && pixel[3] == 1,
                        "Production native Mesh Shader failed to rasterize selected Level 3 color");
            }
            std::string captureName = "native-level3";
            if (velocity)
            {
                captureName = "native-velocity";
            }
            if (paged)
            {
                captureName += "-paged";
            }
            std::ofstream capture("bin/validation/virtualgeometry-phase6/" + captureName + ".ppm", std::ios::binary);
            capture << "P6\n128 128\n255\n";
            for (uint32_t row = 0; row < 128; ++row)
            {
                auto pixels = reinterpret_cast<const float*>(static_cast<const uint8_t*>(bytes) +
                                                             row * footprint.Footprint.RowPitch);
                for (uint32_t column = 0; column < 128; ++column)
                {
                    for (uint32_t component = 0; component < 3; ++component)
                    {
                        capture.put(char(std::clamp(pixels[column * 4 + component], 0.0f, 1.0f) * 255));
                    }
                }
            }
            D3D12_RANGE written{0, 0};
            readback->Unmap(0, &written);
            if (velocity)
            {
                std::cout << "PASS: production native velocity raster matches transform reference, paged=" << paged
                          << '\n';
            }
            else
            {
                std::cout
                    << "PASS: production native Mesh Shader rasterizes Level 3 with nonzero vertex/triangle offsets, paged="
                    << paged << '\n';
            }
        }

      private:
        std::vector<char> ReadShader(const char* name)
        {
            std::ifstream file(std::string("shader/hlsl/build/") + name + ".cso", std::ios::binary);
            std::vector<char> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            Require(!code.empty(), "Missing production shader");
            return code;
        }

        ComPtr<ID3D12Resource> Buffer(uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                                      D3D12_RESOURCE_FLAGS flags)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = type;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = std::max(uint64_t(256), size);
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = flags;
            ComPtr<ID3D12Resource> resource;
            Check(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                    IID_PPV_ARGS(&resource)),
                  "Create test buffer");
            return resource;
        }

        ComPtr<ID3D12Resource> Upload(const void* data, uint64_t size)
        {
            auto resource =
                Buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
            void* mapped = nullptr;
            D3D12_RANGE read{0, 0};
            Check(resource->Map(0, &read, &mapped), "Map test upload");
            if (size)
            {
                memcpy(mapped, data, size);
            }
            resource->Unmap(0, nullptr);
            return resource;
        }

        ComPtr<ID3D12PipelineState> Pipeline(const char* name)
        {
            auto code = ReadShader(name);
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = m_Root.Get();
            desc.CS = {code.data(), code.size()};
            ComPtr<ID3D12PipelineState> pso;
            Check(m_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)), "Create traversal PSO");
            return pso;
        }

        void Barrier()
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            m_List->ResourceBarrier(1, &barrier);
        }

        ComPtr<IDXGIAdapter1> m_Adapter;
        ComPtr<ID3D12Device> m_Device;
        ComPtr<ID3D12CommandQueue> m_Queue;
        ComPtr<ID3D12CommandAllocator> m_Allocator;
        ComPtr<ID3D12GraphicsCommandList> m_List;
        ComPtr<ID3D12Fence> m_Fence;
        ComPtr<ID3D12DescriptorHeap> m_Heap;
        ComPtr<ID3D12Resource> m_HiZ;
        ComPtr<ID3D12RootSignature> m_Root;
        ComPtr<ID3D12PipelineState> m_Reset, m_Traverse, m_Arguments;
        HANDLE m_Event = nullptr;
        uint64_t m_FenceValue = 0;
        uint32_t m_Cases = 0;
        bool m_MeshSupported = false;
    };
} // 無名名前空間

int main(int argc, char** argv)
{
    try
    {
        Harness gpu(argc > 1 && std::string(argv[1]) == "--warp");
        auto graph = MakeSharedVirtualGeometryGraph();
        Constants constants;
        constants.ForceLevel = 0;
        for (uint32_t mask = 0; mask < 16; ++mask)
        {
            std::vector<uint8_t> resident(6, 1);
            for (uint32_t page = 0; page < 4; ++page)
            {
                resident[page] = uint8_t((mask >> page) & 1u);
            }
            gpu.Run(graph, constants, true, false, false, resident);
        }
        constants.PageRequestCapacity = 1;
        gpu.Run(graph, constants, true, false, false, std::vector<uint8_t>{0, 0, 0, 0, 1, 1});
        constants.PageRequestCapacity = 65536;
        gpu.Run(graph, constants, true, false, false, std::vector<uint8_t>{1, 1, 1, 1, 0, 1});
        constants.ForceLevel = -1;
        for (float scale : {0.5f, 1.0f, 2.5f, 3.0f, 10.0f, 100.0f})
        {
            constants.ProjectionPixels = scale;
            gpu.Run(graph, constants);
        }
        constants.Orthographic = 1;
        for (float scale : {0.1f, 0.3f, 1.0f})
        {
            constants.ProjectionPixels = scale;
            constants.Camera.z = 0;
            gpu.Run(graph, constants);
            constants.Camera.z = 10000;
            gpu.Run(graph, constants);
        }
        constants.Camera.z = 0;
        constants.Orthographic = 0;
        for (int level : {0, 1, 2, 31})
        {
            constants.ForceLevel = level;
            gpu.Run(graph, constants);
        }
        constants.ForceLevel = -1;
        constants.Camera.z = 10;
        gpu.Run(graph, constants);
        constants.Camera.z = 0;
        constants.Capacity = 1;
        gpu.Run(graph, constants);
        constants.Capacity = 16;
        constants.StateCapacity = 1;
        gpu.Run(graph, constants);
        constants.StateCapacity = 64;
        constants.Enabled = 0;
        gpu.Run(graph, constants);
        constants.Enabled = 1;
        gpu.Run(graph, constants, false);
        gpu.Run(graph, constants, true, true);
        graph = MakeBroadVirtualGeometryGraph(1024);
        constants.Capacity = 2048;
        constants.StateCapacity = 2048;
        for (int level : {-1, 0, 1, 2, 3, 31})
        {
            constants.ForceLevel = level;
            gpu.Run(graph, constants);
        }
        graph = {};
        graph.Clusters.resize(200);
        for (uint32_t cluster = 0; cluster < graph.Clusters.size(); ++cluster)
        {
            graph.RootClusters.push_back(cluster);
        }
        constants.ForceLevel = -1;
        gpu.Run(graph, constants); // 末端の根が1つのウェーブまたはバッチに収まらない場合。
        graph = MakeSharedVirtualGeometryGraph();
        for (auto& cluster : graph.Clusters)
        {
            cluster.Bounds.Center = {0, 0, 0.5f};
            cluster.Bounds.Radius = 0.01f;
            cluster.Cone.Axis = {0, 0, -1};
            cluster.Cone.Apex = {0, 0, 0.5f};
            cluster.Cone.Cutoff = 0;
        }
        for (auto& group : graph.Groups)
        {
            group.Bounds.Center = {0, 0, 0.5f};
            group.Bounds.Radius = 0.02f;
        }
        constants.ForceLevel = 0;
        constants.Cone = 1;
        constants.Camera.z = 0;
        gpu.Run(graph, constants); // 正面を向くジオメトリは、法線コーンによる除外を通過する。
        constants.Camera.z = 1;
        gpu.Run(graph, constants, true, false, true); // 裏側を向く葉は除外される。
        constants.Cone = 0;
        constants.Camera.z = 0;
        DirectX::XMStoreFloat4x4(&constants.VP, DirectX::XMMatrixIdentity());
        constants.PreviousVP = constants.VP;
        constants.Width = constants.Height = 1024;
        constants.Occlusion = 1;
        gpu.Run(graph, constants, true, false, true); // 常駐する前フレームのHi-Zにより、部分木が除外される。
        constants.Occlusion = 0;
        gpu.Run(graph, constants); // 履歴が無効、またはカメラ切り替え時には、遮蔽判定を無効にする。
        constants.Frustum = 1;
        for (auto& group : graph.Groups)
        {
            group.Bounds.Center[0] = 10;
        }
        for (auto& cluster : graph.Clusters)
        {
            cluster.Bounds.Center[0] = 10;
        }
        gpu.Run(graph, constants, true, false, true); // 根の細分化時に、視錐台の外にあるグループを除外する。
        std::cout << "PASS: " << gpu.Cases() << " production GPU traversal/reference cases\n";
        gpu.RasterNativeParent();
        gpu.RasterNativeParent(true);
        gpu.RasterNativeParent(false, true);
        gpu.RasterNativeParent(true, true);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
