#include "pch.h"
#include "referenceintegrator.h"
#include "referencecapture.h"
#include "referencefixture.h"

#include <vector>

namespace ReferenceIntegrator
{
    namespace
    {
        std::vector<ComPtr<ID3D12Resource>> g_SceneBuffers; // structured data buffers, SceneBufferIndex order
        std::vector<ComPtr<ID3D12Resource>> g_Staging;      // upload staging, kept alive until ReleaseScene
        std::vector<ComPtr<ID3D12Resource>> g_BlasResults;
        ComPtr<ID3D12Resource> g_TlasResult;
        uint32_t g_InstanceCount = 0;
        uint32_t g_LightCount = 0;

        enum SceneBufferIndex
        {
            kTransforms0 = 0,
            kTransforms1,
            kInstanceInfo,
            kMaterials,
            kGeometryInfo,
            kPositions,
            kIndices,
            kLights,
            kSceneBufferCount
        };

        bool CreateDefaultBuffer(ID3D12Device* device, uint64_t size, ComPtr<ID3D12Resource>& output)
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto desc = CD3DX12_RESOURCE_DESC::Buffer(size);
            return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&output)));
        }

        bool UploadBuffer(ID3D12Device* device, ID3D12GraphicsCommandList* list, const void* data, uint64_t size,
                          ComPtr<ID3D12Resource>& output)
        {
            if (!CreateDefaultBuffer(device, size, output))
            {
                return false;
            }
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const auto desc = CD3DX12_RESOURCE_DESC::Buffer(size);
            ComPtr<ID3D12Resource> staging;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&staging))))
            {
                return false;
            }
            void* mapped = nullptr;
            if (FAILED(staging->Map(0, nullptr, &mapped)))
            {
                return false;
            }
            std::memcpy(mapped, data, static_cast<size_t>(size));
            staging->Unmap(0, nullptr);
            list->CopyBufferRegion(output.Get(), 0, staging.Get(), 0, size);
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &barrier);
            g_Staging.push_back(staging);
            return true;
        }

        bool CreateAsBuffer(ID3D12Device* device, uint64_t size, ComPtr<ID3D12Resource>& output)
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            auto desc = CD3DX12_RESOURCE_DESC::Buffer(size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                                             nullptr, IID_PPV_ARGS(&output)));
        }

    } // namespace

    void ReleaseScene()
    {
        g_SceneBuffers.clear();
        g_Staging.clear();
        g_BlasResults.clear();
        g_TlasResult.Reset();
        g_InstanceCount = 0;
        g_LightCount = 0;
    }

    bool HasScene()
    {
        return g_TlasResult != nullptr;
    }

    uint32_t InstanceCount()
    {
        return g_InstanceCount;
    }

    uint32_t LightCount()
    {
        return g_LightCount;
    }

    uint64_t GetTlasAddress()
    {
        return g_TlasResult ? g_TlasResult->GetGPUVirtualAddress() : 0;
    }

    uint64_t GetSceneBufferAddress(int index)
    {
        if (index < 0 || index >= static_cast<int>(g_SceneBuffers.size()) || !g_SceneBuffers[index])
        {
            return 0;
        }
        return g_SceneBuffers[index]->GetGPUVirtualAddress();
    }

    bool BuildFixtureScene(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                           const ReferenceFixture::SceneData& scene)
    {
        ReleaseScene();
        if (device == nullptr || commandList == nullptr || scene.Positions.empty() || scene.Indices.empty())
        {
            return false;
        }
        ComPtr<ID3D12Device5> device5;
        ComPtr<ID3D12GraphicsCommandList4> list4;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) ||
            FAILED(commandList->QueryInterface(IID_PPV_ARGS(&list4))))
        {
            return false;
        }
        g_SceneBuffers.resize(kSceneBufferCount);
        const struct
        {
            const void* Data;
            uint64_t Size;
            int Index;
        } uploads[] = {
            {scene.InstanceTransforms0.data(), scene.InstanceTransforms0.size() * sizeof(float), kTransforms0},
            {scene.InstanceTransforms1.data(), scene.InstanceTransforms1.size() * sizeof(float), kTransforms1},
            {scene.InstanceInfo.data(), scene.InstanceInfo.size() * sizeof(uint32_t), kInstanceInfo},
            {scene.MaterialData.data(), scene.MaterialData.size() * sizeof(float), kMaterials},
            {scene.GeometryInfo.data(), scene.GeometryInfo.size() * sizeof(uint32_t), kGeometryInfo},
            {scene.Positions.data(), scene.Positions.size() * sizeof(float), kPositions},
            {scene.Indices.data(), scene.Indices.size() * sizeof(uint32_t), kIndices},
            {scene.Lights.data(), scene.Lights.size() * sizeof(float), kLights},
        };
        for (const auto& upload : uploads)
        {
            if (upload.Size == 0 || !UploadBuffer(device, commandList, upload.Data, upload.Size, g_SceneBuffers[upload.Index]))
            {
                ReleaseScene();
                return false;
            }
        }
        g_InstanceCount = static_cast<uint32_t>(scene.InstanceInfo.size() / 4);
        g_LightCount = static_cast<uint32_t>(scene.Lights.size() / 8);

        // One BLAS per geometry.
        const uint32_t geometryCount = static_cast<uint32_t>(scene.GeometryInfo.size() / 4);
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs(geometryCount);
        for (uint32_t g = 0; g < geometryCount; ++g)
        {
            const uint32_t indexStart = scene.GeometryInfo[g * 4 + 0];
            const uint32_t vertexStart = scene.GeometryInfo[g * 4 + 1];
            const uint32_t indexCount = scene.GeometryInfo[g * 4 + 2];
            D3D12_RAYTRACING_GEOMETRY_DESC& desc = geometryDescs[g];
            desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            desc.Triangles.VertexBuffer.StartAddress =
                g_SceneBuffers[kPositions]->GetGPUVirtualAddress() + vertexStart * sizeof(float) * 3;
            desc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
            desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            desc.Triangles.IndexBuffer = g_SceneBuffers[kIndices]->GetGPUVirtualAddress() + indexStart * sizeof(uint32_t);
            desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            desc.Triangles.IndexCount = indexCount;
            // VertexCount must be the geometry's vertex span; the
            // fixture packs vertices contiguously per geometry, so
            // derive it from the next geometry's start (or the end).
            const uint32_t nextVertexStart =
                (g + 1 < geometryCount) ? scene.GeometryInfo[(g + 1) * 4 + 1]
                                        : static_cast<uint32_t>(scene.Positions.size() / 3);
            desc.Triangles.VertexCount = nextVertexStart - vertexStart;
        }
        g_BlasResults.resize(geometryCount);
        for (uint32_t g = 0; g < geometryCount; ++g)
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            inputs.NumDescs = 1;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.pGeometryDescs = &geometryDescs[g];
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
            device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
            ComPtr<ID3D12Resource> scratch;
            {
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                auto desc = CD3DX12_RESOURCE_DESC::Buffer(prebuild.ScratchDataSizeInBytes,
                                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                if (FAILED(device5->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&scratch))))
                {
                    ReleaseScene();
                    return false;
                }
            }
            g_Staging.push_back(scratch);
            if (!CreateAsBuffer(device, prebuild.ResultDataMaxSizeInBytes, g_BlasResults[g]))
            {
                ReleaseScene();
                return false;
            }
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs = inputs;
            build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
            build.DestAccelerationStructureData = g_BlasResults[g]->GetGPUVirtualAddress();
            list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(g_BlasResults[g].Get());
            commandList->ResourceBarrier(1, &barrier);
        }

        // TLAS over all instances.
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs(g_InstanceCount);
        for (uint32_t i = 0; i < g_InstanceCount; ++i)
        {
            D3D12_RAYTRACING_INSTANCE_DESC& desc = instanceDescs[i];
            const float* rows0 = &scene.InstanceTransforms0[i * 8];
            const float* rows1 = &scene.InstanceTransforms1[i * 8];
            // Instance transform rows take the translation in the 4th
            // column, which the fixture packing keeps separately.
            desc.Transform[0][0] = rows0[0];
            desc.Transform[0][1] = rows0[1];
            desc.Transform[0][2] = rows0[2];
            desc.Transform[0][3] = rows1[4 + 0];
            desc.Transform[1][0] = rows0[4];
            desc.Transform[1][1] = rows0[5];
            desc.Transform[1][2] = rows0[6];
            desc.Transform[1][3] = rows1[4 + 1];
            desc.Transform[2][0] = rows1[0];
            desc.Transform[2][1] = rows1[1];
            desc.Transform[2][2] = rows1[2];
            desc.Transform[2][3] = rows1[4 + 2];
            desc.InstanceID = i;
            desc.InstanceMask = 0xff;
            desc.AccelerationStructure = g_BlasResults[scene.InstanceInfo[i * 4 + 0]]->GetGPUVirtualAddress();
        }
        ComPtr<ID3D12Resource> instanceBuffer;
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const auto desc = CD3DX12_RESOURCE_DESC::Buffer(instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&instanceBuffer))))
            {
                ReleaseScene();
                return false;
            }
            void* mapped = nullptr;
            if (FAILED(instanceBuffer->Map(0, nullptr, &mapped)))
            {
                ReleaseScene();
                return false;
            }
            std::memcpy(mapped, instanceDescs.data(), instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
            instanceBuffer->Unmap(0, nullptr);
        }
        g_Staging.push_back(instanceBuffer);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
        tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        tlasInputs.NumDescs = g_InstanceCount;
        tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlasInputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuild{};
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasPrebuild);
        ComPtr<ID3D12Resource> tlasScratch;
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            auto desc = CD3DX12_RESOURCE_DESC::Buffer(tlasPrebuild.ScratchDataSizeInBytes,
                                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (FAILED(device5->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                        IID_PPV_ARGS(&tlasScratch))))
            {
                ReleaseScene();
                return false;
            }
        }
        g_Staging.push_back(tlasScratch);
        if (!CreateAsBuffer(device, tlasPrebuild.ResultDataMaxSizeInBytes, g_TlasResult))
        {
            ReleaseScene();
            return false;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasBuild{};
        tlasBuild.Inputs = tlasInputs;
        tlasBuild.ScratchAccelerationStructureData = tlasScratch->GetGPUVirtualAddress();
        tlasBuild.DestAccelerationStructureData = g_TlasResult->GetGPUVirtualAddress();
        list4->BuildRaytracingAccelerationStructure(&tlasBuild, 0, nullptr);
        return true;
    }

    namespace
    {
        ComPtr<ID3D12RootSignature> g_RootSignature;
        ComPtr<ID3D12PipelineState> g_Pso;
        ComPtr<ID3D12DescriptorHeap> g_UavHeap;      // shader-visible, slot 0 = accumulation UAV
        ComPtr<ID3D12DescriptorHeap> g_UavHeapCpu;   // non-visible twin for ClearUnorderedAccessViewFloat

        bool EnsurePipeline(ID3D12Device* device)
        {
            if (g_Pso)
            {
                return true;
            }
            CD3DX12_ROOT_PARAMETER parameters[11];
            parameters[0].InitAsConstants(30, 0);
            for (int i = 0; i < 9; ++i)
            {
                parameters[1 + i].InitAsShaderResourceView(i);
            }
            CD3DX12_DESCRIPTOR_RANGE uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            parameters[10].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);
            CD3DX12_ROOT_SIGNATURE_DESC rootDesc(_countof(parameters), parameters, 0, nullptr,
                                                 D3D12_ROOT_SIGNATURE_FLAG_NONE);
            ComPtr<ID3DBlob> serialized;
            ComPtr<ID3DBlob> errors;
            if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
                FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                   IID_PPV_ARGS(&g_RootSignature))))
            {
                return false;
            }
            ComPtr<ID3DBlob> shader;
            if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/ReferencePathTraceCS.cso", &shader)))
            {
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = g_RootSignature.Get();
            pso.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
            if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_Pso))))
            {
                return false;
            }
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = 1;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_UavHeap))))
            {
                return false;
            }
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            return SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_UavHeapCpu)));
        }

        bool WaitForQueue(ID3D12Device* device, ID3D12CommandQueue* queue)
        {
            ComPtr<ID3D12Fence> fence;
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            {
                return false;
            }
            HANDLE eventHandle = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
            if (eventHandle == nullptr)
            {
                return false;
            }
            bool ok = SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
                      SUCCEEDED(fence->SetEventOnCompletion(1, eventHandle));
            if (ok)
            {
                WaitForSingleObject(eventHandle, INFINITE);
            }
            CloseHandle(eventHandle);
            return ok;
        }
    } // namespace

    bool RunCornellCapture(ID3D12Device* device, ID3D12CommandQueue* queue,
                           const ReferenceCapture::Settings& settings, CaptureResult& result)
    {
        result = {};
        if (device == nullptr || queue == nullptr || !EnsurePipeline(device))
        {
            return false;
        }
        if (!ReferenceCapture::Begin(settings))
        {
            return false;
        }
        struct JobGuard
        {
            bool Done = false;
            ~JobGuard()
            {
                if (!Done)
                {
                    ReferenceCapture::Cancel();
                }
            }
        } guard;
        if (!ReferenceCapture::EnsureTargets(device))
        {
            return false;
        }
        ID3D12Resource* accumulation = ReferenceCapture::GetAccumulationResource();
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(accumulation, nullptr, &uavDesc, g_UavHeap->GetCPUDescriptorHandleForHeapStart());
            device->CreateUnorderedAccessView(accumulation, nullptr, &uavDesc, g_UavHeapCpu->GetCPUDescriptorHandleForHeapStart());
        }

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))))
        {
            return false;
        }
        const ReferenceFixture::SceneData scene = ReferenceFixture::BuildCornellBox();
        if (!BuildFixtureScene(device, list.Get(), scene))
        {
            return false;
        }
        const float clearValue[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        list->ClearUnorderedAccessViewFloat(g_UavHeap->GetGPUDescriptorHandleForHeapStart(),
                                            g_UavHeapCpu->GetCPUDescriptorHandleForHeapStart(), accumulation,
                                            clearValue, 0, nullptr);

        // Camera matrices from the fixture suggestion.
        const XMFLOAT3 cameraPosFloat(scene.CameraPosition[0], scene.CameraPosition[1], scene.CameraPosition[2]);
        const XMFLOAT3 cameraTargetFloat(scene.CameraTarget[0], scene.CameraTarget[1], scene.CameraTarget[2]);
        const XMVECTOR cameraPos = XMLoadFloat3(&cameraPosFloat);
        const XMVECTOR cameraTarget = XMLoadFloat3(&cameraTargetFloat);
        const XMMATRIX view = XMMatrixLookAtLH(cameraPos, cameraTarget, XMVectorSet(0, 1, 0, 0));
        const float aspect = static_cast<float>(settings.Width) / static_cast<float>(settings.Height);
        const XMMATRIX projection = XMMatrixPerspectiveFovLH(scene.CameraFovYRadians, aspect, 0.1f, 100.0f);
        XMFLOAT4X4 invViewProjection{};
        XMStoreFloat4x4(&invViewProjection, XMMatrixTranspose(XMMatrixInverse(nullptr, view * projection)));

        struct Constants
        {
            float InvViewProjection[16];
            float CameraPosition[3];
            uint32_t SampleOffset;
            uint32_t Resolution[2];
            uint32_t SamplesInDispatch;
            uint32_t MaxDepth;
            uint32_t JobSeed;
            uint32_t LightCount;
            float SkyRadiance[3];
            uint32_t Padding;
        };
        list->SetComputeRootSignature(g_RootSignature.Get());
        list->SetPipelineState(g_Pso.Get());
        ID3D12DescriptorHeap* heaps[] = {g_UavHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootShaderResourceView(1, GetTlasAddress());
        for (int i = 0; i < 8; ++i)
        {
            list->SetComputeRootShaderResourceView(2 + i, GetSceneBufferAddress(i));
        }
        list->SetComputeRootDescriptorTable(10, g_UavHeap->GetGPUDescriptorHandleForHeapStart());
        bool finished = false;
        while (!finished)
        {
            Constants constants{};
            std::memcpy(constants.InvViewProjection, &invViewProjection, sizeof(invViewProjection));
            constants.CameraPosition[0] = scene.CameraPosition[0];
            constants.CameraPosition[1] = scene.CameraPosition[1];
            constants.CameraPosition[2] = scene.CameraPosition[2];
            constants.SampleOffset = ReferenceCapture::GetAccumulatedSamples();
            constants.Resolution[0] = settings.Width;
            constants.Resolution[1] = settings.Height;
            constants.SamplesInDispatch = settings.SamplesPerDispatch;
            constants.MaxDepth = settings.MaxDepth;
            constants.JobSeed = settings.Seed;
            constants.LightCount = LightCount();
            list->SetComputeRoot32BitConstants(0, 30, &constants, 0);
            list->Dispatch((settings.Width + 7) / 8, (settings.Height + 7) / 8, 1);
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(accumulation);
            list->ResourceBarrier(1, &barrier);
            finished = ReferenceCapture::RecordDispatchComplete();
        }

        // Readback: accumulation -> readback buffer.
        const uint64_t rowPitch = (static_cast<uint64_t>(settings.Width) * 16 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                                  ~(static_cast<uint64_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1);
        const uint64_t readbackSize = rowPitch * settings.Height;
        ComPtr<ID3D12Resource> readback;
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
            const auto desc = CD3DX12_RESOURCE_DESC::Buffer(readbackSize);
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback))))
            {
                return false;
            }
        }
        D3D12_RESOURCE_BARRIER toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
            accumulation, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &toCopy);
        D3D12_TEXTURE_COPY_LOCATION dest{};
        dest.pResource = readback.Get();
        dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dest.PlacedFootprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        dest.PlacedFootprint.Width = settings.Width;
        dest.PlacedFootprint.Height = settings.Height;
        dest.PlacedFootprint.Depth = 1;
        dest.PlacedFootprint.RowPitch = static_cast<UINT>(rowPitch);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = accumulation;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dest, 0, 0, 0, &src, nullptr);
        if (FAILED(list->Close()))
        {
            return false;
        }
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (!WaitForQueue(device, queue))
        {
            return false;
        }
        void* mapped = nullptr;
        const D3D12_RANGE readRange{0, static_cast<SIZE_T>(readbackSize)};
        if (FAILED(readback->Map(0, &readRange, &mapped)))
        {
            return false;
        }
        result.Width = settings.Width;
        result.Height = settings.Height;
        result.Samples = ReferenceCapture::GetAccumulatedSamples();
        result.Pixels.resize(static_cast<size_t>(settings.Width) * settings.Height * 4);
        const float invSamples = 1.0f / static_cast<float>(result.Samples);
        for (uint32_t y = 0; y < settings.Height; ++y)
        {
            const float* srcRow = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped) + rowPitch * y);
            float* dstRow = &result.Pixels[static_cast<size_t>(y) * settings.Width * 4];
            for (size_t i = 0; i < static_cast<size_t>(settings.Width) * 4; ++i)
            {
                dstRow[i] = srcRow[i] * invSamples;
            }
        }
        readback->Unmap(0, nullptr);
        guard.Done = true;
        return true;
    }

    bool RunCornellCaptureToFiles(ID3D12Device* device, ID3D12CommandQueue* queue,
                                  const ReferenceCapture::Settings& settings, const char* pfmPath, const char* ppmPath)
    {
        CaptureResult result;
        if (!RunCornellCapture(device, queue, settings, result))
        {
            return false;
        }
        return ReferenceImageIO::WritePfm(pfmPath, result) && ReferenceImageIO::WritePpmPreview(ppmPath, result);
    }
} // namespace ReferenceIntegrator
