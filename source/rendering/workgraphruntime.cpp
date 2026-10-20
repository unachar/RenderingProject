#include "pch.h"
#include "workgraphruntime.h"
#include "graphicslog.h"
#include <d3d12sdklayers.h>
#include <dxcapi.h>
#include <filesystem>

namespace WorkGraph
{
    namespace
    {
        struct PreviewCompiler
        {
            // Keep the module resident until all compiled blobs and COM objects are released.
            HMODULE Module = nullptr;
            Microsoft::WRL::ComPtr<IDxcUtils> Utilities;
            Microsoft::WRL::ComPtr<IDxcCompiler3> Compiler;

            PreviewCompiler()
            {
                wchar_t executable[MAX_PATH]{};
                GetModuleFileNameW(nullptr, executable, MAX_PATH);
                const auto directory = std::filesystem::path(executable).parent_path();
                auto compilerPath = directory / L"MeshNodesDXC" / L"dxcompiler.dll";
                Module = LoadLibraryExW(compilerPath.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (!Module)
                {
                    WriteGraphicsLog("WorkGraph: preview DXC missing (%lu)\n", GetLastError());
                    return;
                }
                const auto createInstance =
                    reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(Module, "DxcCreateInstance"));
                if (createInstance)
                {
                    createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&Utilities));
                    createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&Compiler));
                }
            }
        };

        PreviewCompiler& Compiler()
        {
            static PreviewCompiler compiler;
            return compiler;
        }
    }

    HRESULT CompileSource(const char* source, size_t length, const wchar_t* sourceName, const wchar_t* entry,
                          const wchar_t* profile, Microsoft::WRL::ComPtr<ID3DBlob>& bytecode)
    {
        bytecode.Reset();
        auto& compiler = Compiler();
        if (!source || !sourceName || !profile || !compiler.Compiler || !compiler.Utilities)
        {
            return E_INVALIDARG;
        }
        // Mesh Nodes DXIL belongs to the preview runtime. The retail validator loaded by the
        // application cannot validate this experimental dialect; use the preview internal validator.
        std::vector<LPCWSTR> arguments{sourceName, L"-T", profile, L"-HV", L"2021", L"-O3", L"-select-validator", L"internal"};
        if (entry && entry[0])
        {
            arguments.push_back(L"-E");
            arguments.push_back(entry);
        }
#if defined(RENDERINGPROJECT_USE_DXC) && RENDERINGPROJECT_USE_DXC
        const std::wstring textureHeapDefinition =
            L"RP_TEXTURE_HEAP_BASE=" + std::to_wstring(g_BindlessShaderSettings.TextureHeapBase);
        if (g_BindlessShaderSettings.Enabled)
        {
            arguments.push_back(L"-D");
            arguments.push_back(L"RP_BINDLESS=1");
            arguments.push_back(L"-D");
            arguments.push_back(textureHeapDefinition.c_str());
        }
#endif
        const auto includeDirectory = std::filesystem::path(sourceName).parent_path().wstring();
        arguments.push_back(L"-I");
        arguments.push_back(includeDirectory.empty() ? L"." : includeDirectory.c_str());
        DxcBuffer buffer{source, length, DXC_CP_UTF8};
        Microsoft::WRL::ComPtr<IDxcIncludeHandler> includes;
        HRESULT result = compiler.Utilities->CreateDefaultIncludeHandler(&includes);
        if (FAILED(result))
        {
            return result;
        }
        Microsoft::WRL::ComPtr<IDxcResult> compilation;
        const auto compileBegin = std::chrono::steady_clock::now();
        WriteGraphicsLog("WorkGraph compile begin: %ls (%ls)\n", sourceName, profile);
        result = compiler.Compiler->Compile(&buffer, arguments.data(), static_cast<UINT>(arguments.size()),
                                            includes.Get(), IID_PPV_ARGS(&compilation));
        if (FAILED(result))
        {
            return result;
        }
        Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
        compilation->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (errors && errors->GetStringLength())
        {
            WriteGraphicsLog("WorkGraph shader %ls: %s\n", sourceName, errors->GetStringPointer());
        }
        compilation->GetStatus(&result);
        if (FAILED(result))
        {
            return result;
        }
        Microsoft::WRL::ComPtr<IDxcBlob> object;
        result = compilation->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
        if (FAILED(result))
        {
            return result;
        }
        result = D3DCreateBlob(object->GetBufferSize(), &bytecode);
        if (SUCCEEDED(result))
        {
            memcpy(bytecode->GetBufferPointer(), object->GetBufferPointer(), object->GetBufferSize());
            const double milliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - compileBegin).count();
            WriteGraphicsLog("WorkGraph compile complete: %ls %.3f ms %zu bytes\n", sourceName,
                             milliseconds, object->GetBufferSize());
        }
        return result;
    }

    HRESULT CompileFile(const wchar_t* file, const wchar_t* entry, const wchar_t* profile,
                        Microsoft::WRL::ComPtr<ID3DBlob>& bytecode)
    {
        auto& compiler = Compiler();
        if (!compiler.Utilities || !file)
        {
            return E_INVALIDARG;
        }
        Microsoft::WRL::ComPtr<IDxcBlobEncoding> source;
        HRESULT result = compiler.Utilities->LoadFile(file, nullptr, &source);
        if (FAILED(result))
        {
            WriteGraphicsLog("WorkGraph: cannot read %ls (0x%08lx)\n", file, result);
            return result;
        }
        return CompileSource(static_cast<const char*>(source->GetBufferPointer()), source->GetBufferSize(), file, entry,
                             profile, bytecode);
    }

    bool EnablePreviewFeatures()
    {
        const UUID features[]{D3D12ExperimentalShaderModels, D3D12StateObjectsExperiment};
        const HRESULT result = D3D12EnableExperimentalFeatures(2, features, nullptr, nullptr);
        WriteGraphicsLog("WorkGraph preview features: 0x%08lx (Agility SDK 715)\n", result);
        return SUCCEEDED(result);
    }

    bool IsSupported(ID3D12Device* device, bool requireMeshNodes)
    {
        if (!device)
        {
            return false;
        }
        static constexpr GUID supportCacheId{
            0xe3506674, 0x4ef5, 0x48f0, {0xaf, 0x2d, 0x78, 0xcc, 0xae, 0xac, 0x73, 0x8d}};
        UINT tier = D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED;
        UINT cacheSize = sizeof(tier);
        if (FAILED(device->GetPrivateData(supportCacheId, &cacheSize, &tier)))
        {
            D3D12_FEATURE_DATA_D3D12_OPTIONS21 options{};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &options, sizeof(options))))
            {
                tier = options.WorkGraphsTier;
            }
            device->SetPrivateData(supportCacheId, sizeof(tier), &tier);
        }
        if (requireMeshNodes)
        {
            return tier >= D3D12_WORK_GRAPHS_TIER_1_1;
        }
        return tier >= D3D12_WORK_GRAPHS_TIER_1_0;
    }

    void Graph::Reset()
    {
        m_StateObject.Reset();
        m_Device.Reset();
        for (auto& backing : m_BackingMemory)
        {
            backing.Reset();
        }
        m_ProgramIdentifier = {};
        m_BackingMemoryBytes = 0;
        m_DispatchCount = 0;
    }

    bool Graph::FinishInitialization(ID3D12Device* device, const D3D12_STATE_OBJECT_DESC& description,
                                     const wchar_t* programName, const wchar_t* entryNode, UINT maximumInputRecords)
    {
        Reset();
        Microsoft::WRL::ComPtr<ID3D12Device5> stateDevice;
        if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&stateDevice))))
        {
            return false;
        }
        WriteGraphicsLog("WorkGraph state creation begin: %ls\n", programName);
        HRESULT result = stateDevice->CreateStateObject(&description, IID_PPV_ARGS(&m_StateObject));
        if (FAILED(result))
        {
            WriteGraphicsLog("WorkGraph %ls CreateStateObject failed: 0x%08lx\n", programName, result);
            Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&messages))))
            {
                const UINT64 count = messages->GetNumStoredMessagesAllowedByRetrievalFilter();
                const UINT64 first = count > 12 ? count - 12 : 0;
                for (UINT64 index = first; index < count; ++index)
                {
                    SIZE_T size = 0;
                    messages->GetMessage(index, nullptr, &size);
                    std::vector<unsigned char> storage(size);
                    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                    if (SUCCEEDED(messages->GetMessage(index, message, &size)))
                    {
                        WriteGraphicsLog("WorkGraph D3D12: %s\n", message->pDescription);
                    }
                }
            }
            return false;
        }
        Microsoft::WRL::ComPtr<ID3D12StateObjectProperties1> stateProperties;
        Microsoft::WRL::ComPtr<ID3D12WorkGraphProperties1> graphProperties;
        if (FAILED(m_StateObject.As(&stateProperties)) || FAILED(m_StateObject.As(&graphProperties)))
        {
            Reset();
            return false;
        }
        m_ProgramIdentifier = stateProperties->GetProgramIdentifier(programName);
        const UINT graphIndex = graphProperties->GetWorkGraphIndex(programName);
        const D3D12_NODE_ID entryId{entryNode, 0};
        m_EntrypointIndex = graphProperties->GetEntrypointIndex(graphIndex, entryId);
        if (graphIndex == UINT_MAX || m_EntrypointIndex == UINT_MAX)
        {
            WriteGraphicsLog("WorkGraph %ls: entry %ls not found\n", programName, entryNode);
            Reset();
            return false;
        }
        m_MaximumInputRecords = maximumInputRecords;
        graphProperties->SetMaximumInputRecords(graphIndex, maximumInputRecords, 1);
        D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS requirements{};
        graphProperties->GetWorkGraphMemoryRequirements(graphIndex, &requirements);
        m_BackingMemoryBytes = requirements.MaxSizeInBytes;
        m_Device = device;
        WriteGraphicsLog("WorkGraph %ls ready: entry=%u backing=%llu bytes/slot slots=%u\n", programName,
                         m_EntrypointIndex, m_BackingMemoryBytes, FrameSlots);
        return true;
    }

    bool Graph::Bind(ID3D12GraphicsCommandList* commands, UINT frameIndex,
                     Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList10>& graphCommands)
    {
        if (!m_StateObject || !commands || frameIndex >= FrameSlots ||
            FAILED(commands->QueryInterface(IID_PPV_ARGS(&graphCommands))))
        {
            return false;
        }
        D3D12_SET_PROGRAM_DESC program{};
        program.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
        program.WorkGraph.ProgramIdentifier = m_ProgramIdentifier;
        // Reinitialize for each invocation; a frame can dispatch this graph more than once.
        program.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
        auto& backing = m_BackingMemory[frameIndex];
        if (m_BackingMemoryBytes && !backing)
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(m_BackingMemoryBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            const HRESULT result = m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&backing));
            if (FAILED(result))
            {
                WriteGraphicsLog("WorkGraph backing allocation failed: %llu bytes, HRESULT=0x%08lx\n",
                                 m_BackingMemoryBytes, result);
                return false;
            }
            backing->SetName(L"Work Graph Frame Backing Memory");
        }
        if (backing)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = backing.Get();
            commands->ResourceBarrier(1, &barrier);
            program.WorkGraph.BackingMemory = {backing->GetGPUVirtualAddress(), m_BackingMemoryBytes};
        }
        graphCommands->SetProgram(&program);
        return true;
    }

    bool Graph::DispatchCpu(ID3D12GraphicsCommandList* commands, UINT frameIndex, const void* records,
                            UINT recordStride, UINT numRecords)
    {
        if (!records || numRecords == 0 || numRecords > m_MaximumInputRecords)
        {
            return false;
        }
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList10> graphCommands;
        if (!Bind(commands, frameIndex, graphCommands))
        {
            return false;
        }
        D3D12_DISPATCH_GRAPH_DESC dispatch{};
        dispatch.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        dispatch.NodeCPUInput = {m_EntrypointIndex, numRecords, records, recordStride};
        graphCommands->DispatchGraph(&dispatch);
        RenderProfiler::RecordWorkGraphDispatch();
        ++m_DispatchCount;
        return true;
    }

    bool Graph::DispatchGpu(ID3D12GraphicsCommandList* commands, UINT frameIndex,
                            D3D12_GPU_VIRTUAL_ADDRESS inputAddress)
    {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList10> graphCommands;
        if (!inputAddress || !Bind(commands, frameIndex, graphCommands))
        {
            return false;
        }
        D3D12_DISPATCH_GRAPH_DESC dispatch{};
        dispatch.Mode = D3D12_DISPATCH_MODE_NODE_GPU_INPUT;
        dispatch.NodeGPUInput = inputAddress;
        graphCommands->DispatchGraph(&dispatch);
        RenderProfiler::RecordWorkGraphDispatch();
        ++m_DispatchCount;
        return true;
    }

    bool ComputeGraph::Initialize(ID3D12Device* device, ID3D12RootSignature* rootSignature, const wchar_t* file,
                                  const wchar_t* entryNode)
    {
        if (!IsSupported(device) || !rootSignature)
        {
            return false;
        }
        Microsoft::WRL::ComPtr<ID3DBlob> library;
        if (FAILED(CompileFile(file, nullptr, L"lib_6_9", library)))
        {
            return false;
        }
        return InitializeLibrary(device, rootSignature, library.Get(), entryNode);
    }

    bool ComputeGraph::InitializeLibrary(ID3D12Device* device, ID3D12RootSignature* rootSignature,
                                         ID3DBlob* library, const wchar_t* entryNode)
    {
        if (!IsSupported(device) || !rootSignature || !library)
        {
            return false;
        }
        D3D12_DXIL_LIBRARY_DESC dxil{{library->GetBufferPointer(), library->GetBufferSize()}, 0, nullptr};
        D3D12_GLOBAL_ROOT_SIGNATURE root{rootSignature};
        D3D12_WORK_GRAPH_DESC graph{};
        graph.ProgramName = L"ComputeGraph";
        graph.Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
        D3D12_STATE_SUBOBJECT subobjects[]{{D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &root},
                                           {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &dxil},
                                           {D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH, &graph}};
        D3D12_STATE_OBJECT_DESC state{D3D12_STATE_OBJECT_TYPE_EXECUTABLE, static_cast<UINT>(std::size(subobjects)),
                                      subobjects};
        return FinishInitialization(device, state, graph.ProgramName, entryNode, 1);
    }

    bool MeshGraph::Initialize(ID3D12Device* device, const MeshGraphDescription& description)
    {
        if (!IsSupported(device, true) || !description.RootSignature || !description.Library.pShaderBytecode)
        {
            return false;
        }
        D3D12_DXIL_LIBRARY_DESC library{description.Library, 0, nullptr};
        const D3D12_EXPORT_DESC pixelExport{L"WorkGraphPixel", description.PixelShaderEntry, D3D12_EXPORT_FLAG_NONE};
        D3D12_DXIL_LIBRARY_DESC pixelLibrary{description.PixelShader, 1, &pixelExport};
        D3D12_GLOBAL_ROOT_SIGNATURE root{description.RootSignature};
        D3D12_STATE_OBJECT_CONFIG config{
            D3D12_STATE_OBJECT_FLAG_WORK_GRAPHS_USE_GRAPHICS_STATE_FOR_GLOBAL_ROOT_SIGNATURE};
        const UINT sampleMask = UINT_MAX;
        const DXGI_SAMPLE_DESC samples{1, 0};
        const D3D12_PRIMITIVE_TOPOLOGY_DESC topology{D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE};
        const D3D12_DEPTH_STENCIL_FORMAT depthFormat{description.DepthFormat};
        // Reserve the complete array before linking generic-program subobject pointers.
        std::vector<D3D12_STATE_SUBOBJECT> subobjects;
        subobjects.reserve(16);
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config});
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &root});
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library});
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &pixelLibrary});
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_BLEND, &description.Blend});
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_RASTERIZER, &description.Rasterizer});
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, &description.DepthStencil});
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, &description.RenderTargets});
        if (description.DepthFormat != DXGI_FORMAT_UNKNOWN)
        {
            subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, &depthFormat});
        }
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, &sampleMask});
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, &samples});
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &topology});
        std::vector<const D3D12_STATE_SUBOBJECT*> graphicsState;
        for (size_t index = 4; index < subobjects.size(); ++index)
        {
            graphicsState.push_back(&subobjects[index]);
        }
        LPCWSTR exports[]{description.MeshShader, L"WorkGraphPixel"};
        D3D12_GENERIC_PROGRAM_DESC meshProgram{L"WorkGraphMeshProgram", 2, exports,
                                               static_cast<UINT>(graphicsState.size()), graphicsState.data()};
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_GENERIC_PROGRAM, &meshProgram});
        D3D12_NODE_ID meshId{description.MeshNode, 0};
        D3D12_MESH_LAUNCH_OVERRIDES meshOverrides{};
        meshOverrides.pNewName = &meshId;
        const D3D12_MAX_NODE_INPUT_RECORDS_PER_GRAPH_ENTRY_RECORD meshRecordLimit{
            description.MaximumMeshRecordsPerEntry, FALSE};
        if (description.MaximumMeshRecordsPerEntry != 0)
        {
            meshOverrides.pMaxInputRecordsPerGraphEntryRecord = &meshRecordLimit;
        }
        D3D12_NODE nodes[2]{};
        nodes[0].NodeType = D3D12_NODE_TYPE_SHADER;
        nodes[0].Shader.Shader = description.EntryNode;
        nodes[1].NodeType = D3D12_NODE_TYPE_PROGRAM;
        nodes[1].Program.Program = meshProgram.ProgramName;
        nodes[1].Program.OverridesType = D3D12_PROGRAM_NODE_OVERRIDES_TYPE_MESH_LAUNCH;
        nodes[1].Program.pMeshLaunchOverrides = &meshOverrides;
        D3D12_NODE_ID entryId{description.EntryNode, 0};
        D3D12_WORK_GRAPH_DESC graph{};
        graph.ProgramName = description.ProgramName;
        graph.NumEntrypoints = 1;
        graph.pEntrypoints = &entryId;
        graph.NumExplicitlyDefinedNodes = 2;
        graph.pExplicitlyDefinedNodes = nodes;
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH, &graph});
        D3D12_STATE_OBJECT_DESC state{D3D12_STATE_OBJECT_TYPE_EXECUTABLE, static_cast<UINT>(subobjects.size()),
                                      subobjects.data()};
        return FinishInitialization(device, state, description.ProgramName, description.EntryNode,
                                    description.MaximumInputRecords);
    }
}
