#include "pch.h"
#include "../cachepaths.h"
#include "materialgraphshader.h"
#include "dxccompiler.h"
#include "persistentpsocache.h"
#include <mutex>

namespace
{
    std::mutex graphCompilerMutex;

    std::string PreviewSource(const MaterialGraphProgram& program)
    {
        std::string source = program.Hlsl;
        source += R"hlsl(
#line 1 "material_preview"
cbuffer GraphPreviewView : register(b1) { float PreviewTime; uint PreviewResolution; uint PreviewShape; uint PreviewNormal; };
struct GraphPreviewInput { float4 Position : SV_Position; float2 UV : TEXCOORD0; };
GraphPreviewInput GraphPreviewVS(uint vertex : SV_VertexID)
{
    GraphPreviewInput result;
    result.UV = float2((vertex << 1) & 2, vertex & 2);
    result.Position = float4(result.UV * float2(2,-2) + float2(-1,1),0,1);
    return result;
}
float4 GraphPreviewPS(GraphPreviewInput input) : SV_Target
{
    MaterialGraphContext context;
    context.UV = input.UV;
    context.WorldPosition = float3(input.UV * float2(2,-2) + float2(-1,1),0);
    context.WorldNormal = float3(0,0,1);
    context.CameraVector = float3(0,0,1);
    context.Time = PreviewTime;
    if (PreviewShape != 0)
    {
        float radius = dot(context.WorldPosition.xy,context.WorldPosition.xy);
        if (radius > 1) return float4(0,0,0,0);
        context.WorldPosition.z = sqrt(saturate(1-radius));
        context.WorldNormal = normalize(context.WorldPosition);
    }
    context.Tangent = normalize(cross(abs(context.WorldNormal.y) < 0.99 ? float3(0,1,0) : float3(1,0,0), context.WorldNormal));
    context.Bitangent = -cross(context.WorldNormal, context.Tangent);
)hlsl";
        if (program.MaterialOutput)
        {
            source += R"hlsl(
    GeneratedMaterial material = EvaluateMaterialGraph(context);
    float3 normal = context.WorldNormal;
    float3 tangent = normalize(cross(abs(normal.y) < 0.99 ? float3(0,1,0) : float3(1,0,0),normal));
    float3 bitangent = cross(normal,tangent);
    float2 scaledNormal = material.Normal.xy * max(material.NormalIntensity, 0.0);
    normal = normalize(tangent * scaledNormal.x + bitangent * scaledNormal.y + normal * material.Normal.z);
    float3 light = normalize(float3(-0.4,0.6,0.8));
    float3 view = float3(0,0,1);
    float3 halfVector = normalize(light + view);
    float nDotL = saturate(dot(normal,light));
    float nDotV = max(saturate(dot(normal,view)),0.001);
    float nDotH = saturate(dot(normal,halfVector));
    float vDotH = saturate(dot(view,halfVector));
    float roughness = clamp(material.Roughness,0.04,1);
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denominator = nDotH*nDotH*(alpha2-1)+1;
    float distribution = alpha2 / max(3.14159265*denominator*denominator,0.0001);
    float k = (roughness+1)*(roughness+1)/8;
    float geometry = (nDotV / (nDotV*(1-k)+k)) * (nDotL / max(nDotL*(1-k)+k,0.001));
    float metallic = saturate(material.Metallic);
    float3 f0 = lerp(0.04.xxx,material.BaseColor.rgb,metallic);
    float3 fresnel = f0 + (1-f0)*pow(1-vDotH,5);
    float3 specular = distribution * geometry * fresnel / max(4*nDotL*nDotV,0.001);
    float3 diffuse = (1-fresnel)*(1-metallic)*material.BaseColor.rgb/3.14159265;
    float3 color = (diffuse+specular)*nDotL*3 + material.BaseColor.rgb*0.08*saturate(material.AO)+material.Emissive;
    color = pow(saturate(color/(1+max(color,0))),1.0/2.2);
    float opacity = saturate(material.BaseColor.a * material.Opacity);
    opacity *= step(0.5,material.OpacityMask);
    float checker = fmod(floor(input.UV.x*12)+floor(input.UV.y*12),2);
    return float4(lerp(lerp(0.12.xxx,0.25.xxx,checker),color,opacity),1);
)hlsl";
        }
        else
        {
            source += R"hlsl(
    float4 color = EvaluateMaterialNode(context);
    if (PreviewNormal != 0) color.rgb = color.rgb * 0.5 + 0.5;
    float checker = fmod(floor(input.UV.x*12)+floor(input.UV.y*12),2);
    color.rgb = lerp(lerp(0.12.xxx,0.25.xxx,checker),color.rgb,saturate(color.a));
    return float4(color.rgb,1);
)hlsl";
        }
        source += "}\n";
        return source;
    }
}

MaterialGraphShader MaterialGraphShaderCompiler::Compile(const MaterialGraphProgram& program)
{
    // Keep the existing atomic cache writers single-owner for identical graph variants.
    std::lock_guard lock(graphCompilerMutex);
    MaterialGraphShader shader;
    if (!program.Success)
    {
        shader.Error = program.Error;
        return shader;
    }
    const auto source = PreviewSource(program);
    shader.Hash = 14695981039346656037ull;
    for (const unsigned char value : source)
    {
        shader.Hash = (shader.Hash ^ value) * 1099511628211ull;
    }
    const auto directory = CachePaths::Directory("MaterialGraphs/Shaders");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        shader.Error = error.message();
        return shader;
    }
    const auto path = directory / (std::to_string(shader.Hash) + ".hlsl");
    if (!WriteAssetFile(path, source, shader.Error))
    {
        return shader;
    }
    const auto compile = [&](const char* entry, const char* profile, ComPtr<ID3DBlob>& output) {
        ComPtr<ID3DBlob> diagnostics;
        const auto result = DxcCompileFromFileCompat(
            path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &output, &diagnostics);
        if (FAILED(result))
        {
            if (diagnostics)
            {
                shader.Error.assign(static_cast<const char*>(diagnostics->GetBufferPointer()),
                                    diagnostics->GetBufferSize());
            }
            else
            {
                shader.Error = "DXC failed (HRESULT " + std::to_string(static_cast<unsigned>(result)) + ").";
            }
            return false;
        }
        return true;
    };
    if (compile("GraphPreviewVS", "vs_6_0", shader.Vertex))
    {
        compile("GraphPreviewPS", "ps_6_0", shader.Pixel);
    }
    return shader;
}



// =====================================================================================================
// MaterialGraphGpu
// =====================================================================================================
#include "graphicsdevice.h"
#include "frameconstants.h"
#include "editordescriptors.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>

struct MaterialGraphGpu::Job
{
    BakeDesc Desc;
    std::atomic<int> State{0}; // 0 未記録, 1 記録済み（Fence 待ち）, 2 失敗
    std::atomic<uint64_t> Fence{0};
    int Row = -1;
    ComPtr<ID3D12Resource> Output;
    ComPtr<ID3D12Resource> Readback;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 6> Footprints{};
    uint32_t Slices = 0; // コピーしたスライス（bit）
};

namespace MaterialGraphGpu
{
    namespace
    {
        constexpr int kProgramWidth = MaterialGraphVm::kMaxTexels; // 1024
        constexpr int kPreviewSlots = 256;
        constexpr int kJobRows = 32;
        constexpr int kDynamicRows = 32; // 発光の動的ベイク（同時に焼けるオブジェクト数）
        constexpr int kDynamicRowStart = kPreviewSlots + kJobRows;
        constexpr int kProgramRows = kPreviewSlots + kJobRows + kDynamicRows;
        constexpr UINT kDynamicResolution = 128;      // 発光の動的ベイクの解像度（GI の光源なので低解像度で十分）
        constexpr uint64_t kDynamicKeepFrames = 240;  // 要求されなくなってから解放するまでのフレーム数
        constexpr int kCell = 96;
        constexpr int kCellsPerRow = 16;
        constexpr int kAtlasSize = kCell * kCellsPerRow; // 1536
        constexpr UINT64 kRowPitch = kProgramWidth * 16ull;
        constexpr UINT kRingCount = 3;
        constexpr double kRefreshSeconds = 0.5; // 動かないプレビューも、テクスチャの読み込み完了を拾うため時々描き直す

        struct GpuConstants
        {
            uint32_t Mode = 0;
            uint32_t ProgramRow = 0;
            uint32_t CellX = 0;
            uint32_t CellY = 0;
            uint32_t Resolution = 0;
            float Time = 0.0f;
            uint32_t Flags = 0;
            uint32_t BakeMask = 0;
            float Clip = 0.3333f;
            uint32_t OutputPins = 0;
            float Yaw = 0.6f;
            float Pitch = 0.25f;
            float ViewCamera[3] = {0.0f, 0.0f, 0.0f}; // 5：実カメラ
            float ViewDistance = 0.0f;                // 5：カメラ → オブジェクトの距離
            float ViewDepth = 0.0f;                   // 5：ビュー深度
            float ViewPadding[3] = {0.0f, 0.0f, 0.0f};
        };
        static_assert(sizeof(GpuConstants) == 80, "MaterialGraphGpuCS.hlsl の GraphGpuConstants と揃える");

        // 発光の動的ベイク（オブジェクトごと）
        struct DynamicEntry
        {
            uint64_t Key = 0;
            int Row = -1;
            Texels Program;
            uint64_t ProgramHash = 0;
            uint64_t InputHash = 0;  // 焼いた時の入力（プログラム / カメラ / 距離 / 時刻）
            uint64_t PendingHash = 0;
            GpuConstants Constants;
            ComPtr<ID3D12Resource> Texture; // RGBA16F、焼いていない間は PIXEL_SHADER_RESOURCE
            std::shared_ptr<EditorDescriptorLease> Uav;
            uint64_t Revision = 0;
            uint64_t LastFrame = 0;
            std::chrono::steady_clock::time_point LastBake{};
            bool Upload = true;
            bool Ready = false;
        };
        struct RetiredTexture
        {
            ComPtr<ID3D12Resource> Texture;
            std::shared_ptr<EditorDescriptorLease> Uav;
            uint64_t Fence = 0;
        };

        struct Preview
        {
            const void* Owner = nullptr;
            uint64_t Node = 0;
            uint64_t Hash = 0;
            Texels Program;
            GpuConstants Constants;
            bool Animated = false;
            bool Upload = true;
            bool Dirty = true;
            bool Ready = false;
            bool Used = false;
            uint64_t LastFrame = 0;
            std::chrono::steady_clock::time_point LastDraw{};
        };

        struct State
        {
            bool Initialized = false;
            bool Failed = false;
            std::string Error;
            ComPtr<ID3D12RootSignature> Root;
            ComPtr<ID3D12PipelineState> Pipeline;
            ComPtr<ID3D12Resource> Programs;
            ComPtr<ID3D12Resource> Atlas;
            std::array<ComPtr<ID3D12Resource>, kRingCount> Upload;
            std::array<uint64_t, kRingCount> UploadFence{};
            std::shared_ptr<EditorDescriptorLease> ProgramSrv, AtlasUav, AtlasSrv, NullBakeUav;
            std::array<Preview, kPreviewSlots> Previews;
            std::array<std::shared_ptr<Job>, kJobRows> JobRows; // 行を使っている（記録済み / 待ち）ジョブ
            std::vector<std::shared_ptr<Job>> Pending;
            std::unordered_map<uint64_t, DynamicEntry> Dynamic;
            std::array<bool, kDynamicRows> DynamicRowUsed{};
            std::vector<RetiredTexture> Retired;
            uint64_t Frame = 1;
        };

        std::mutex g_Mutex;
        State g_State;

        uint64_t HashParams(const PreviewRequest& request)
        {
            uint64_t hash = request.ProgramHash ^ 0x9E3779B97F4A7C15ull;
            const uint32_t values[] = {static_cast<uint32_t>(request.PreviewMode), request.Flags, request.OutputPins,
                                       static_cast<uint32_t>(request.Resolution)};
            for (const auto value : values)
            {
                hash = (hash ^ value) * 1099511628211ull;
            }
            uint32_t clip = 0;
            memcpy(&clip, &request.Clip, sizeof(clip));
            return (hash ^ clip) * 1099511628211ull;
        }

        bool Initialize()
        {
            auto& s = g_State;
            if (s.Initialized || s.Failed)
            {
                return s.Initialized;
            }
            ID3D12Device* device = GraphicsDevice::GetDevice();
            const auto fail = [&](const std::string& message) {
                s.Failed = true;
                s.Error = message;
                return false;
            };
            if (!device)
            {
                return fail("GPU デバイスがありません");
            }
            // ---- ルートシグネチャ：定数 / プログラム / プレビュー表 / ベイク出力 / マテリアルのテクスチャ表
            CD3DX12_DESCRIPTOR_RANGE programRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0);
            CD3DX12_DESCRIPTOR_RANGE previewRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0);
            CD3DX12_DESCRIPTOR_RANGE bakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE textureRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 512, 0, 1);
            CD3DX12_ROOT_PARAMETER parameters[5];
            parameters[0].InitAsConstants(sizeof(GpuConstants) / 4, 0);
            parameters[1].InitAsDescriptorTable(1, &programRange);
            parameters[2].InitAsDescriptorTable(1, &previewRange);
            parameters[3].InitAsDescriptorTable(1, &bakeRange);
            parameters[4].InitAsDescriptorTable(1, &textureRange);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
            const CD3DX12_ROOT_SIGNATURE_DESC description(5, parameters, 1, &sampler);
            ComPtr<ID3DBlob> signature, diagnostics;
            if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &diagnostics)) ||
                FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                   IID_PPV_ARGS(&s.Root))))
            {
                return fail("ルートシグネチャを作れません");
            }
            ComPtr<ID3DBlob> shader;
            diagnostics.Reset();
            const HRESULT compiled = DxcCompileFromFileCompat(
                L"shader/hlsl/MaterialGraphGpuCS.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_6_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &diagnostics);
            if (FAILED(compiled))
            {
                return fail(diagnostics ? std::string(static_cast<const char*>(diagnostics->GetBufferPointer()),
                                                      diagnostics->GetBufferSize())
                                        : std::string("MaterialGraphGpuCS.hlsl のコンパイルに失敗しました"));
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = s.Root.Get();
            pipeline.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
            if (FAILED(PersistentPsoCache::Compute(device, pipeline, s.Pipeline)))
            {
                return fail("パイプラインを作れません");
            }
            // ---- リソース
            const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const auto programDesc =
                CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, kProgramWidth, kProgramRows, 1, 1);
            auto atlasDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, kAtlasSize, kAtlasSize, 1, 1);
            atlasDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            const auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(kRowPitch * kProgramRows);
            if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &programDesc,
                                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                       IID_PPV_ARGS(&s.Programs))) ||
                FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &atlasDesc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                       IID_PPV_ARGS(&s.Atlas))))
            {
                return fail("プレビュー用のテクスチャを作れません");
            }
            for (auto& upload : s.Upload)
            {
                if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&upload))))
                {
                    return fail("アップロード用のバッファを作れません");
                }
            }
            s.Programs->SetName(L"MaterialGraphGpuPrograms");
            s.Atlas->SetName(L"MaterialGraphGpuPreviews");
            s.ProgramSrv = EditorDescriptors::Allocate();
            s.AtlasUav = EditorDescriptors::Allocate();
            s.AtlasSrv = EditorDescriptors::Allocate();
            s.NullBakeUav = EditorDescriptors::Allocate();
            if (!s.ProgramSrv || !s.AtlasUav || !s.AtlasSrv || !s.NullBakeUav)
            {
                return fail("ディスクリプタが足りません");
            }
            device->CreateShaderResourceView(s.Programs.Get(), nullptr, s.ProgramSrv->Cpu);
            device->CreateUnorderedAccessView(s.Atlas.Get(), nullptr, nullptr, s.AtlasUav->Cpu);
            device->CreateShaderResourceView(s.Atlas.Get(), nullptr, s.AtlasSrv->Cpu);
            D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};
            nullUav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            nullUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            nullUav.Texture2DArray.ArraySize = 1;
            device->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, s.NullBakeUav->Cpu);
            s.Initialized = true;
            return true;
        }

        void WriteRow(uint8_t* mapped, int row, const Texels& program)
        {
            const size_t count = (std::min)(program.size(), static_cast<size_t>(kProgramWidth));
            memcpy(mapped + kRowPitch * row, program.data(), count * sizeof(program[0]));
        }

        void CopyRow(ID3D12GraphicsCommandList* commands, ID3D12Resource* upload, int row, size_t texels)
        {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            footprint.Offset = kRowPitch * row;
            footprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT,
                                   static_cast<UINT>(std::clamp<size_t>(texels, 1, kProgramWidth)), 1, 1,
                                   static_cast<UINT>(kRowPitch)};
            const CD3DX12_TEXTURE_COPY_LOCATION destination(g_State.Programs.Get(), 0);
            const CD3DX12_TEXTURE_COPY_LOCATION source(upload, footprint);
            commands->CopyTextureRegion(&destination, 0, static_cast<UINT>(row), 0, &source, nullptr);
        }
    } // namespace

    bool IsAvailable()
    {
        std::lock_guard lock(g_Mutex);
        return !g_State.Failed;
    }

    bool PrepareStartupAssets()
    {
        std::lock_guard lock(g_Mutex);
        return Initialize();
    }

    const char* LastError()
    {
        std::lock_guard lock(g_Mutex);
        return g_State.Error.c_str();
    }

    bool RequestPreview(const PreviewRequest& request, PreviewImage& image)
    {
        std::lock_guard lock(g_Mutex);
        auto& s = g_State;
        if (s.Failed || !request.Program || request.Program->empty() ||
            request.Program->size() > static_cast<size_t>(kProgramWidth))
        {
            return false;
        }
        // スロットを探す（無ければ空き、さらに無ければ最も長く使っていないもの）
        int slot = -1;
        int freeSlot = -1;
        int oldest = -1;
        for (int i = 0; i < kPreviewSlots; ++i)
        {
            const auto& preview = s.Previews[i];
            if (preview.Used && preview.Owner == request.Owner && preview.Node == request.Node)
            {
                slot = i;
                break;
            }
            if (!preview.Used && freeSlot < 0)
            {
                freeSlot = i;
            }
            if (preview.Used && preview.LastFrame + 1 < s.Frame &&
                (oldest < 0 || preview.LastFrame < s.Previews[oldest].LastFrame))
            {
                oldest = i;
            }
        }
        if (slot < 0)
        {
            slot = freeSlot >= 0 ? freeSlot : oldest;
            if (slot < 0)
            {
                return false;
            }
            s.Previews[slot] = Preview{};
            s.Previews[slot].Used = true;
            s.Previews[slot].Owner = request.Owner;
            s.Previews[slot].Node = request.Node;
            s.Previews[slot].Hash = ~0ull;
        }
        auto& preview = s.Previews[slot];
        const uint64_t hash = HashParams(request);
        const int resolution = std::clamp(request.Resolution, 8, kCell);
        if (hash != preview.Hash)
        {
            preview.Hash = hash;
            preview.Program = *request.Program;
            preview.Upload = true;
            preview.Dirty = true;
        }
        auto& constants = preview.Constants;
        constants.Mode = static_cast<uint32_t>(request.PreviewMode);
        constants.ProgramRow = static_cast<uint32_t>(slot);
        constants.CellX = static_cast<uint32_t>((slot % kCellsPerRow) * kCell);
        constants.CellY = static_cast<uint32_t>((slot / kCellsPerRow) * kCell);
        constants.Resolution = static_cast<uint32_t>(resolution);
        constants.Time = request.Time;
        constants.Flags = request.Flags;
        constants.Clip = request.Clip;
        constants.OutputPins = request.OutputPins;
        preview.Animated = request.Animated;
        preview.LastFrame = s.Frame;
        image.TextureId = s.AtlasSrv ? s.AtlasSrv->Gpu.ptr : 0;
        image.U0 = static_cast<float>(constants.CellX) / kAtlasSize;
        image.V0 = static_cast<float>(constants.CellY) / kAtlasSize;
        image.U1 = static_cast<float>(constants.CellX + resolution) / kAtlasSize;
        image.V1 = static_cast<float>(constants.CellY + resolution) / kAtlasSize;
        image.Ready = preview.Ready && image.TextureId != 0;
        return true;
    }

    void ForgetOwner(const void* owner)
    {
        std::lock_guard lock(g_Mutex);
        for (auto& preview : g_State.Previews)
        {
            if (preview.Used && preview.Owner == owner)
            {
                preview = Preview{};
            }
        }
    }

    bool RequestDynamicEmission(const DynamicEmissionRequest& request, DynamicEmissionTexture& result)
    {
        std::lock_guard lock(g_Mutex);
        auto& s = g_State;
        if (s.Failed || request.Key == 0 || !request.Program || request.Program->empty() ||
            request.Program->size() > static_cast<size_t>(kProgramWidth))
        {
            return false;
        }
        auto found = s.Dynamic.find(request.Key);
        if (found == s.Dynamic.end())
        {
            // 行が空いていなければ動的にしない（従来のベイク値のまま）
            int row = -1;
            for (int i = 0; i < kDynamicRows; ++i)
            {
                if (!s.DynamicRowUsed[i])
                {
                    row = i;
                    break;
                }
            }
            if (row < 0)
            {
                return false;
            }
            s.DynamicRowUsed[row] = true;
            DynamicEntry entry;
            entry.Key = request.Key;
            entry.Row = kDynamicRowStart + row;
            found = s.Dynamic.emplace(request.Key, std::move(entry)).first;
        }
        auto& entry = found->second;
        if (entry.ProgramHash != request.ProgramHash || entry.Program.empty())
        {
            entry.Program = *request.Program;
            entry.ProgramHash = request.ProgramHash;
            entry.Upload = true;
        }
        // 入力のハッシュ：変わったときだけ焼き直す（静止したカメラ・Time 不使用なら GPU の仕事は無い）
        uint64_t hash = request.ProgramHash ^ 0xC2B2AE3D27D4EB4Full;
        const auto mix = [&hash](const void* data, size_t bytes) {
            const auto* p = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < bytes; ++i)
            {
                hash = (hash ^ p[i]) * 1099511628211ull;
            }
        };
        if (request.UsesView)
        {
            mix(request.Camera.data(), sizeof(float) * 3);
            mix(&request.Distance, sizeof(float));
            mix(&request.Depth, sizeof(float));
        }
        if (request.UsesTime)
        {
            mix(&request.Time, sizeof(float));
        }
        auto& constants = entry.Constants;
        constants = GpuConstants{};
        constants.Mode = static_cast<uint32_t>(Mode::DynamicEmission);
        constants.ProgramRow = static_cast<uint32_t>(entry.Row);
        constants.Resolution = kDynamicResolution;
        constants.Time = request.Time;
        constants.ViewCamera[0] = request.Camera[0];
        constants.ViewCamera[1] = request.Camera[1];
        constants.ViewCamera[2] = request.Camera[2];
        constants.ViewDistance = request.Distance;
        constants.ViewDepth = request.Depth;
        // 焼き直しは最大 30 回/秒：カメラを回し続けても GI / Lumen のキャッシュを毎フレーム作り直さない
        const auto now = std::chrono::steady_clock::now();
        if (!entry.Ready || entry.Upload || now - entry.LastBake >= std::chrono::milliseconds(33))
        {
            entry.PendingHash = hash;
            if (hash != entry.InputHash)
            {
                entry.LastBake = now;
            }
        }
        entry.LastFrame = s.Frame;
        if (!entry.Ready || !entry.Texture)
        {
            return false;
        }
        result.Texture = entry.Texture;
        result.Revision = entry.Revision;
        return true;
    }

    std::shared_ptr<Job> Submit(BakeDesc desc)
    {
        std::lock_guard lock(g_Mutex);
        if (g_State.Failed || desc.Program.empty() || desc.Program.size() > static_cast<size_t>(kProgramWidth))
        {
            return nullptr;
        }
        desc.Resolution = std::clamp(desc.Resolution, 16, 4096);
        auto job = std::make_shared<Job>();
        job->Desc = std::move(desc);
        g_State.Pending.push_back(job);
        return job;
    }

    bool Wait(const std::shared_ptr<Job>& job, double timeoutSeconds, std::array<Texels, 6>& slices)
    {
        if (!job)
        {
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        while (true)
        {
            const int state = job->State.load();
            if (state == 2)
            {
                return false;
            }
            if (state == 1 && GraphicsDevice::GetCompletedFence() >= job->Fence.load())
            {
                break;
            }
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > timeoutSeconds)
            {
                // まだ記録されていなければ取り消す（記録済みなら GPU の完了後に Execute が片付ける）
                int expected = 0;
                job->State.compare_exchange_strong(expected, 2);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        uint8_t* data = nullptr;
        const CD3DX12_RANGE range(0, static_cast<SIZE_T>(job->Readback->GetDesc().Width));
        if (FAILED(job->Readback->Map(0, &range, reinterpret_cast<void**>(&data))) || !data)
        {
            return false;
        }
        const int resolution = job->Desc.Resolution;
        for (int slice = 0; slice < 6; ++slice)
        {
            slices[slice].clear();
            if (!(job->Slices & (1u << slice)))
            {
                continue;
            }
            const auto& footprint = job->Footprints[slice];
            auto& pixels = slices[slice];
            pixels.resize(static_cast<size_t>(resolution) * resolution);
            for (int y = 0; y < resolution; ++y)
            {
                memcpy(&pixels[static_cast<size_t>(y) * resolution],
                       data + footprint.Offset + static_cast<size_t>(footprint.Footprint.RowPitch) * y,
                       static_cast<size_t>(resolution) * 16u);
            }
        }
        const CD3DX12_RANGE noWrite(0, 0);
        job->Readback->Unmap(0, &noWrite);
        return true;
    }

    void Execute(ID3D12GraphicsCommandList* commands)
    {
        std::lock_guard lock(g_Mutex);
        auto& s = g_State;
        const uint64_t frame = s.Frame++;
        const uint64_t completed = GraphicsDevice::GetCompletedFence();
        // ---- 発光の動的ベイク：要求されなくなったものを片付ける（GPU の完了後に解放）
        for (auto it = s.Dynamic.begin(); it != s.Dynamic.end();)
        {
            auto& entry = it->second;
            if (entry.LastFrame + kDynamicKeepFrames < frame)
            {
                s.DynamicRowUsed[entry.Row - kDynamicRowStart] = false;
                if (entry.Texture)
                {
                    s.Retired.push_back({entry.Texture, entry.Uav, GraphicsDevice::GetPendingSubmissionFence()});
                }
                it = s.Dynamic.erase(it);
                continue;
            }
            ++it;
        }
        std::erase_if(s.Retired, [&](const RetiredTexture& retired) { return completed >= retired.Fence; });
        const bool dynamicWork = std::any_of(s.Dynamic.begin(), s.Dynamic.end(), [&](const auto& item) {
            const auto& entry = item.second;
            return entry.LastFrame == frame && (!entry.Ready || entry.Upload || entry.PendingHash != entry.InputHash);
        });
        const bool hasWork = !s.Pending.empty() || dynamicWork ||
                             std::any_of(s.Previews.begin(), s.Previews.end(), [&](const Preview& preview) {
                                 return preview.Used && preview.LastFrame == frame;
                             });
        // ---- 完了したジョブの行を返す（出力テクスチャも解放。読み戻しバッファはワーカーが持つ）
        for (auto& row : s.JobRows)
        {
            if (row && (row->State.load() != 0 && completed >= row->Fence.load()))
            {
                row->Output.Reset();
                row.reset();
            }
        }
        if (!commands || !hasWork)
        {
            return;
        }
        if (!Initialize())
        {
            for (auto& job : s.Pending)
            {
                job->State = 2;
            }
            s.Pending.clear();
            return;
        }
        const UINT ring = static_cast<UINT>(frame % kRingCount);
        if (s.UploadFence[ring] > completed)
        {
            return; // アップロード用バッファがまだ GPU で使われている（次のフレームで行う）
        }
        ID3D12Device* device = GraphicsDevice::GetDevice();
        const auto now = std::chrono::steady_clock::now();

        // ---- 今回描くプレビューとジョブを決める
        std::vector<int> draws;
        for (int i = 0; i < kPreviewSlots; ++i)
        {
            auto& preview = s.Previews[i];
            if (!preview.Used || preview.LastFrame != frame)
            {
                continue;
            }
            if (preview.Dirty || preview.Animated ||
                std::chrono::duration<double>(now - preview.LastDraw).count() >= kRefreshSeconds)
            {
                draws.push_back(i);
            }
        }
        std::vector<std::shared_ptr<Job>> jobs;
        for (auto it = s.Pending.begin(); it != s.Pending.end();)
        {
            auto& job = *it;
            if (job->State.load() == 2)
            {
                it = s.Pending.erase(it);
                continue;
            }
            const auto free = std::find_if(s.JobRows.begin(), s.JobRows.end(), [](const auto& row) { return !row; });
            if (free == s.JobRows.end())
            {
                break; // 行が空くまで待つ
            }
            *free = job;
            job->Row = kPreviewSlots + static_cast<int>(free - s.JobRows.begin());
            jobs.push_back(job);
            it = s.Pending.erase(it);
        }
        std::vector<DynamicEntry*> dynamics;
        for (auto& [key, entry] : s.Dynamic)
        {
            if (entry.LastFrame == frame && (!entry.Ready || entry.Upload || entry.PendingHash != entry.InputHash))
            {
                dynamics.push_back(&entry);
            }
        }
        if (draws.empty() && jobs.empty() && dynamics.empty())
        {
            return;
        }

        // ---- プログラムのアップロード
        bool uploads = false;
        for (const int i : draws)
        {
            uploads |= s.Previews[i].Upload;
        }
        uploads |= !jobs.empty();
        for (const auto* entry : dynamics)
        {
            uploads |= entry->Upload;
        }
        ID3D12DescriptorHeap* heaps[] = {FrameConstants::GetCbvHeap()};
        commands->SetDescriptorHeaps(1, heaps);
        if (uploads)
        {
            uint8_t* mapped = nullptr;
            const CD3DX12_RANGE noRead(0, 0);
            if (FAILED(s.Upload[ring]->Map(0, &noRead, reinterpret_cast<void**>(&mapped))) || !mapped)
            {
                return;
            }
            for (const int i : draws)
            {
                if (s.Previews[i].Upload)
                {
                    WriteRow(mapped, i, s.Previews[i].Program);
                }
            }
            for (const auto& job : jobs)
            {
                WriteRow(mapped, job->Row, job->Desc.Program);
            }
            for (const auto* entry : dynamics)
            {
                if (entry->Upload)
                {
                    WriteRow(mapped, entry->Row, entry->Program);
                }
            }
            s.Upload[ring]->Unmap(0, nullptr);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                s.Programs.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            commands->ResourceBarrier(1, &barrier);
            for (const int i : draws)
            {
                if (s.Previews[i].Upload)
                {
                    CopyRow(commands, s.Upload[ring].Get(), i, s.Previews[i].Program.size());
                    s.Previews[i].Upload = false;
                }
            }
            for (const auto& job : jobs)
            {
                CopyRow(commands, s.Upload[ring].Get(), job->Row, job->Desc.Program.size());
            }
            for (auto* entry : dynamics)
            {
                if (entry->Upload)
                {
                    CopyRow(commands, s.Upload[ring].Get(), entry->Row, entry->Program.size());
                    entry->Upload = false;
                }
            }
            barrier = CD3DX12_RESOURCE_BARRIER::Transition(s.Programs.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            commands->ResourceBarrier(1, &barrier);
            s.UploadFence[ring] = GraphicsDevice::GetPendingSubmissionFence();
        }

        commands->SetComputeRootSignature(s.Root.Get());
        commands->SetPipelineState(s.Pipeline.Get());
        commands->SetComputeRootDescriptorTable(1, s.ProgramSrv->Gpu);
        commands->SetComputeRootDescriptorTable(2, s.AtlasUav->Gpu);
        commands->SetComputeRootDescriptorTable(
            4, CD3DX12_GPU_DESCRIPTOR_HANDLE(FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart(),
                                             FrameConstants::g_kTEXTURE_SRV_START_INDEX,
                                             FrameConstants::GetCbvIncrementSize()));

        // ---- プレビュー
        if (!draws.empty())
        {
            commands->SetComputeRootDescriptorTable(3, s.NullBakeUav->Gpu);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(s.Atlas.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->ResourceBarrier(1, &barrier);
            for (const int i : draws)
            {
                auto& preview = s.Previews[i];
                commands->SetComputeRoot32BitConstants(0, sizeof(GpuConstants) / 4, &preview.Constants, 0);
                const UINT groups = (preview.Constants.Resolution + 7u) / 8u;
                commands->Dispatch(groups, groups, 1);
                preview.Dirty = false;
                preview.Ready = true;
                preview.LastDraw = now;
            }
            barrier = CD3DX12_RESOURCE_BARRIER::Transition(s.Atlas.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commands->ResourceBarrier(1, &barrier);
        }

        // ---- 発光の動的ベイク（GI / 反射 / Lumen が読む発光テクスチャを焼き直す）
        for (auto* entry : dynamics)
        {
            bool created = false;
            if (!entry->Texture)
            {
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, kDynamicResolution,
                                                         kDynamicResolution, 1, 1);
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                entry->Uav = EditorDescriptors::Allocate();
                if (!entry->Uav ||
                    FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                           IID_PPV_ARGS(&entry->Texture))))
                {
                    entry->Texture.Reset();
                    entry->Uav.reset();
                    continue;
                }
                entry->Texture->SetName(L"MaterialGraphDynamicEmission");
                D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uavDesc.Texture2DArray.ArraySize = 1;
                device->CreateUnorderedAccessView(entry->Texture.Get(), nullptr, &uavDesc, entry->Uav->Cpu);
                created = true;
            }
            if (!created)
            {
                auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    entry->Texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                commands->ResourceBarrier(1, &barrier);
            }
            commands->SetComputeRootDescriptorTable(3, entry->Uav->Gpu);
            commands->SetComputeRoot32BitConstants(0, sizeof(GpuConstants) / 4, &entry->Constants, 0);
            const UINT groups = (kDynamicResolution + 7u) / 8u;
            commands->Dispatch(groups, groups, 1);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                entry->Texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commands->ResourceBarrier(1, &barrier);
            entry->InputHash = entry->PendingHash;
            entry->Ready = true;
            ++entry->Revision;
        }

        // ---- ベイク / ノードを焼く
        for (const auto& job : jobs)
        {
            const auto& desc = job->Desc;
            const UINT resolution = static_cast<UINT>(desc.Resolution);
            const bool nodeMode = desc.JobMode == Mode::BakeNode;
            const UINT slices = nodeMode ? 1u : 6u;
            const uint32_t sliceMask = nodeMode ? 1u : (desc.SlotMask & 63u);
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            auto outputDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, resolution, resolution,
                                                           static_cast<UINT16>(slices), 1);
            outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 6> footprints{};
            UINT64 readbackBytes = 0;
            device->GetCopyableFootprints(&outputDesc, 0, slices, 0, footprints.data(), nullptr, nullptr, &readbackBytes);
            const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
            const auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(readbackBytes);
            auto uav = EditorDescriptors::Allocate();
            if (!uav ||
                FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &outputDesc,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&job->Output))) ||
                FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&job->Readback))))
            {
                job->State = 2;
                continue;
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uavDesc.Texture2DArray.ArraySize = slices;
            device->CreateUnorderedAccessView(job->Output.Get(), nullptr, &uavDesc, uav->Cpu);
            commands->SetComputeRootDescriptorTable(3, uav->Gpu);
            GpuConstants constants;
            constants.Mode = static_cast<uint32_t>(desc.JobMode);
            constants.ProgramRow = static_cast<uint32_t>(job->Row);
            constants.Resolution = resolution;
            constants.Time = desc.Time;
            constants.Flags = desc.Flags;
            constants.BakeMask = desc.SlotMask;
            commands->SetComputeRoot32BitConstants(0, sizeof(GpuConstants) / 4, &constants, 0);
            const UINT groups = (resolution + 7u) / 8u;
            commands->Dispatch(groups, groups, 1);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(job->Output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->ResourceBarrier(1, &barrier);
            for (UINT slice = 0; slice < slices; ++slice)
            {
                if (!(sliceMask & (1u << slice)))
                {
                    continue;
                }
                const CD3DX12_TEXTURE_COPY_LOCATION destination(job->Readback.Get(), footprints[slice]);
                const CD3DX12_TEXTURE_COPY_LOCATION source(job->Output.Get(), slice);
                commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
            job->Footprints = footprints;
            job->Slices = sliceMask;
            job->Fence = GraphicsDevice::GetPendingSubmissionFence();
            job->State = 1;
            // uav のディスクリプタはここで返しても、このフレームの完了までは再利用されない（EditorDescriptors）
        }
    }

    void Shutdown()
    {
        std::lock_guard lock(g_Mutex);
        for (auto& job : g_State.Pending)
        {
            job->State = 2;
        }
        g_State = State{};
    }
} // namespace MaterialGraphGpu

