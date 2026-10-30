#include "pch.h"
#include "referenceintegrator.h"
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
} // namespace ReferenceIntegrator
