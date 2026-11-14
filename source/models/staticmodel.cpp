#include "pch.h"
#include "../cachepaths.h"
#include "../cachefile.h"
#include "envflags.h"
#include <unordered_set>
#include <unordered_map>
#include <charconv>
#include <climits>
#include "gpudriven/gpudrivenrenderer.h"
#include "../../External/meshoptimizer/src/meshoptimizer.h"
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <shlwapi.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cctype>

#pragma comment(lib, "shlwapi.lib")

#include "main.h"
#include "staticmodel.h"
#include "modelmapimport.h"
#include "modelmapjson.h"
#include "virtualgeometry/virtualgeometrypagecache.h"
#include "virtualgeometry/virtualgeometryresources.h"
#include "materialmanager.h"
#include <json/json.h>
#include <iomanip>
#include "meshlets/meshletmanager.h"
#include "texturemanager.h"
#include "materialpartresolver.h"
#include "cimport.h"
#include "scene.h"
#include "material.h"
#include "modelimportutils.h"
#include "pmxloader.h"
#include "postprocess.h"
#include "toonoutlinebuilder.h"

#pragma comment(lib, "assimp-vc143-mt.lib")

struct MtlData
{
    XMFLOAT4 Kd = {1.0f, 1.0f, 1.0f, 1.0f};
    string MapKd = "";
};

// OBJ / MTL 文の引数は、行の残りを前後の空白を除いて取得する。ファイル名には空白を許す。
static string ObjStatementArgument(const string& line, size_t keywordLength)
{
    string value = line.size() > keywordLength ? line.substr(keywordLength) : string();
    const auto first = value.find_first_not_of(" \t");
    const auto last = value.find_last_not_of(" \t\r\n");
    return first == string::npos ? string() : value.substr(first, last - first + 1);
}

static unordered_map<string, MtlData> ParseMtl(const string& mtlFilePath)
{
    unordered_map<string, MtlData> materials;
    // 日本語のフォルダ名やファイル名を扱う UTF-8 パス。
    ifstream ifs(ModelPathFromUtf8(mtlFilePath.c_str()));
    if (!ifs)
    {
        return materials;
    }

    string line;
    string currentMtl;

    while (getline(ifs, line))
    {
        if (line.rfind("newmtl ", 0) == 0)
        {
            const string name = ObjStatementArgument(line, 7);
            if (!name.empty())
            {
                currentMtl = name;
                materials[currentMtl] = MtlData();
                materials[currentMtl].Kd = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
            }
        }
        else if (!currentMtl.empty() && line.rfind("Kd ", 0) == 0)
        {
            istringstream iss(line.substr(3));
            float r;
            float g;
            float b;
            if (iss >> r >> g >> b)
            {
                materials[currentMtl].Kd = XMFLOAT4(r, g, b, 1.0f);
            }
        }
        else if (!currentMtl.empty() && line.rfind("map_Kd ", 0) == 0)
        {
            // "-bm 1" などのオプションは未対応。末尾のファイル名を使う。
            const string path = ObjStatementArgument(line, 7);
            if (!path.empty())
            {
                materials[currentMtl].MapKd = path;
            }
        }
    }

    return materials;
}

static bool TryLoadStaticEmbeddedTextureByIndex(const aiScene* scene, const aiString& texPath, const char* modelName,
                                                int& outTexIndex)
{
    if (!scene || texPath.length == 0 || texPath.data[0] != '*')
    {
        return false;
    }

    const int texIdx = atoi(&texPath.data[1]);
    if (texIdx < 0 || texIdx >= static_cast<int>(scene->mNumTextures))
    {
        return false;
    }

    aiTexture* aiTex = scene->mTextures[texIdx];
    string uniqueName = string(modelName) + "_" + texPath.C_Str();
    outTexIndex = TextureManager::LoadTextureFromMemory(uniqueName.c_str(),
                                                        reinterpret_cast<const uint8_t*>(aiTex->pcData), aiTex->mWidth);
    return true;
}

static bool TryLoadStaticEmbeddedTextureByName(const aiScene* scene, const aiString& texPath, const char* modelName,
                                               int& outTexIndex)
{
    if (!scene)
    {
        return false;
    }

    for (unsigned int i = 0; i < scene->mNumTextures; ++i)
    {
        if (strcmp(scene->mTextures[i]->mFilename.C_Str(), texPath.C_Str()) != 0)
        {
            continue;
        }

        aiTexture* aiTex = scene->mTextures[i];
        string uniqueName = string(modelName) + "_" + texPath.C_Str();
        outTexIndex = TextureManager::LoadTextureFromMemory(
            uniqueName.c_str(), reinterpret_cast<const uint8_t*>(aiTex->pcData), aiTex->mWidth);
        return true;
    }
    return false;
}

static int ResolveStaticMeshTextureIndex(const aiScene* scene, const aiMesh* mesh, const char* fileName,
                                         const string& dirPath)
{
    int textureIndex = TextureManager::GetDefaultTextureIndex();
    if (!scene || !mesh || mesh->mMaterialIndex >= scene->mNumMaterials)
    {
        return textureIndex;
    }

    aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
    aiString texPath;
    const bool hasTexture = (material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) ||
                            (material->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS);
    if (!hasTexture)
    {
        return textureIndex;
    }

    if (TryLoadStaticEmbeddedTextureByIndex(scene, texPath, fileName, textureIndex) ||
        TryLoadStaticEmbeddedTextureByName(scene, texPath, fileName, textureIndex))
    {
        return textureIndex;
    }

    auto toLower = [](string value) {
        return MaterialPartToLowerString(value);
    };
    auto isSupportedTextureExtension = [&](const filesystem::path& path) {
        const string ext = toLower(path.extension().string());
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" || ext == ".tif" ||
               ext == ".tiff" || ext == ".dds" || ext == ".sph" || ext == ".spa";
    };
    auto pushCandidate = [](vector<filesystem::path>& paths, const filesystem::path& path) {
        if (find(paths.begin(), paths.end(), path) == paths.end())
        {
            paths.push_back(path);
        }
    };

    filesystem::path texFilePath = ModelPathFromUtf8(texPath.C_Str());
    vector<filesystem::path> candidates;
    if (texFilePath.extension() != ".tga" && texFilePath.extension() != ".TGA")
    {
        filesystem::path tgaPath = texFilePath;
        tgaPath.replace_extension(".tga");
        pushCandidate(candidates, ModelPathFromUtf8(dirPath.c_str()) / tgaPath);
        pushCandidate(candidates, ModelPathFromUtf8(dirPath.c_str()) / tgaPath.filename());
    }
    if (isSupportedTextureExtension(texFilePath))
    {
        if (texFilePath.is_absolute())
        {
            pushCandidate(candidates, texFilePath);
        }
        pushCandidate(candidates, ModelPathFromUtf8(dirPath.c_str()) / texFilePath);
        pushCandidate(candidates, ModelPathFromUtf8(dirPath.c_str()) / texFilePath.filename());
    }

    for (const auto& candidate : candidates)
    {
        if (filesystem::exists(candidate))
        {
            return TextureManager::LoadTexture(candidate);
        }
    }

    Debug::Log("WARNING: Static model texture not found: %s\n", texPath.C_Str());
    return textureIndex;
}

bool StaticModelResource::UploadBufferData(ID3D12Device* device, ID3D12Resource* dstResource, const void* srcData,
                                           UINT sizeInBytes, D3D12_RESOURCE_STATES finalState, const char* logTag)
{
    return UploadBufferData(device, dstResource, srcData, sizeInBytes, D3D12_RESOURCE_STATE_COMMON, finalState, logTag);
}

bool StaticModelResource::UploadBufferData(ID3D12Device* device, ID3D12Resource* dstResource, const void* srcData,
                                           UINT sizeInBytes, D3D12_RESOURCE_STATES beforeState,
                                           D3D12_RESOURCE_STATES finalState, const char* logTag)
{
    if (!device || !dstResource || !srcData || sizeInBytes == 0)
    {
        Debug::Log("ERROR: Invalid upload args (%s)\n", logTag);
        return false;
    }

    D3D12_RESOURCE_DESC desc = dstResource->GetDesc();
    UINT64 uploadBufferSize = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadBufferSize);
    ComPtr<ID3D12Resource> uploadBuffer = TextureManager::AcquireUploadBuffer(device, uploadBufferSize);
    if (!uploadBuffer)
    {
        Debug::Log("ERROR: Failed to acquire upload buffer (%s)\n", logTag);
        return false;
    }

    UINT8* pDest = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    HRESULT hr = uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pDest));
    if (FAILED(hr) || !pDest)
    {
        TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
        Debug::Log("ERROR: Failed to map upload buffer (%s)\n", logTag);
        return false;
    }
    memcpy(pDest, srcData, sizeInBytes);
    uploadBuffer->Unmap(0, nullptr);

    const bool batchMode = TextureManager::IsBatchLoading();
    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;

    if (batchMode)
    {
        cmdList = TextureManager::GetBatchCommandList();
        if (!cmdList)
        {
            TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
            Debug::Log("ERROR: Batch command list not initialized (%s)\n", logTag);
            return false;
        }
    }
    else
    {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
        if (FAILED(hr))
        {
            TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
            Debug::Log("ERROR: CreateCommandAllocator failed (%s)\n", logTag);
            return false;
        }
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr,
                                       IID_PPV_ARGS(&cmdList));
        if (FAILED(hr))
        {
            TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
            Debug::Log("ERROR: CreateCommandList failed (%s)\n", logTag);
            return false;
        }
    }

    D3D12_SUBRESOURCE_DATA data{};
    data.pData = srcData;
    data.RowPitch = sizeInBytes;
    data.SlicePitch = data.RowPitch;

    if (beforeState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        auto toCopyDest =
            CD3DX12_RESOURCE_BARRIER::Transition(dstResource, beforeState, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->ResourceBarrier(1, &toCopyDest);
    }

    UpdateSubresources(cmdList.Get(), dstResource, uploadBuffer.Get(), 0, 0, 1, &data);

    if (finalState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(dstResource, D3D12_RESOURCE_STATE_COPY_DEST, finalState);
        cmdList->ResourceBarrier(1, &barrier);
    }

    if (batchMode)
    {
        TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize, true);
    }
    else
    {
        if (!TextureManager::ExecuteCommandListAndSync(cmdList.Get()))
        {
            TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
            Debug::Log("ERROR: ExecuteCommandListAndSync failed (%s)\n", logTag);
            return false;
        }
        TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
    }

    return true;
}

bool StaticModelResource::CreateDefaultBufferAndUpload(ID3D12Device* device, UINT sizeInBytes, const void* srcData,
                                                       D3D12_RESOURCE_STATES finalState, const char* createErrorLog,
                                                       const char* uploadLogTag, ComPtr<ID3D12Resource>& outResource)
{
    if (!device || !srcData || sizeInBytes == 0)
    {
        Debug::Log("ERROR: Invalid buffer creation args (%s)\n", uploadLogTag);
        return false;
    }

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(&outResource));
    if (FAILED(hr) || !outResource)
    {
        Debug::Log("%s", createErrorLog);
        return false;
    }

    return UploadBufferData(device, outResource.Get(), srcData, sizeInBytes, finalState, uploadLogTag);
}

void StaticModelResource::LoadLODMetadata(const char* path)
{
    m_SourcePath = path;
    std::string error;
    const auto sourcePath = ModelPathFromUtf8(path);
    if (!VirtualGeometryImport::Load(sourcePath, m_VGSettings, error))
    {
        m_VGSettings.Enabled = false;
        Debug::Log("VirtualGeometry settings fallback: %s\n", error.c_str());
    }
    auto lodPath = sourcePath;
    lodPath += ".lod";
    ifstream file(lodPath);
    if (file && !Visibility::LoadLOD(file, m_LODMetadata))
    {
        Debug::Log("Invalid LOD metadata: %s.lod\n", path);
    }
}

bool StaticModelResource::BuildLodIndexBuffers(ID3D12Device* device, const vector<StaticModelVertex>& vertices,
                                               const vector<unsigned int>& indices, StaticMeshData& meshData)
{
    if (!device || vertices.empty() || indices.size() < 6)
    {
        return true;
    }

    constexpr float ratios[StaticMeshData::LodCount - 1] = {0.50f, 0.20f, 0.08f, 0.03f};
    constexpr float errors[StaticMeshData::LodCount - 1] = {0.01f, 0.035f, 0.08f, 0.15f};
    for (UINT lod = 0; lod < StaticMeshData::LodCount - 1; ++lod)
    {
        const size_t targetCount = max<size_t>(3, (static_cast<size_t>(indices.size() * ratios[lod]) / 3) * 3);
        vector<unsigned int> lodIndices(indices.size());
        const size_t resultCount = meshopt_simplify(
            lodIndices.data(), indices.data(), indices.size(), reinterpret_cast<const float*>(vertices.data()),
            vertices.size(), sizeof(StaticModelVertex), targetCount, errors[lod], meshopt_SimplifyRegularize);
        if (resultCount < 3 || resultCount >= indices.size())
        {
            continue;
        }
        lodIndices.resize(resultCount);
        // Perf: LODインデックスにも頂点キャッシュ最適化を適用。
        meshopt_optimizeVertexCache(lodIndices.data(), lodIndices.data(), lodIndices.size(), vertices.size());
        const UINT byteSize = static_cast<UINT>(resultCount * sizeof(unsigned int));
        if (!CreateDefaultBufferAndUpload(device, byteSize, lodIndices.data(), D3D12_RESOURCE_STATE_INDEX_BUFFER,
                                          "ERROR: Failed to create static LOD index buffer\n",
                                          "static LOD index upload", meshData.LodIndexBuffers[lod]))
        {
            return false;
        }
        meshData.LodIndexBufferViews[lod].BufferLocation = meshData.LodIndexBuffers[lod]->GetGPUVirtualAddress();
        meshData.LodIndexBufferViews[lod].Format = DXGI_FORMAT_R32_UINT;
        meshData.LodIndexBufferViews[lod].SizeInBytes = byteSize;
        meshData.LodIndexCounts[lod] = static_cast<UINT>(resultCount);
        meshData.CpuLodIndices[lod].assign(lodIndices.begin(), lodIndices.end());
    }

    // BLAS 頂点の 16bit 量子化判定にも使うため、DXR LOD/プロキシの有無に関係なく境界を求める。
    if (!vertices.empty())
    {
        XMFLOAT3 boundsLow = vertices.front().Position, boundsHigh = boundsLow;
        for (const auto& v : vertices)
        {
            boundsLow.x = (std::min)(boundsLow.x, v.Position.x);
            boundsLow.y = (std::min)(boundsLow.y, v.Position.y);
            boundsLow.z = (std::min)(boundsLow.z, v.Position.z);
            boundsHigh.x = (std::max)(boundsHigh.x, v.Position.x);
            boundsHigh.y = (std::max)(boundsHigh.y, v.Position.y);
            boundsHigh.z = (std::max)(boundsHigh.z, v.Position.z);
        }
        const float dx = boundsHigh.x - boundsLow.x, dy = boundsHigh.y - boundsLow.y, dz = boundsHigh.z - boundsLow.z;
        meshData.RayCenter = {(boundsLow.x + boundsHigh.x) * .5f, (boundsLow.y + boundsHigh.y) * .5f,
                              (boundsLow.z + boundsHigh.z) * .5f};
        meshData.RayRadius = .5f * sqrtf(dx * dx + dy * dy + dz * dz);
    }
    static const bool rayLod = ReadEnvironmentFlagOnce("DX12_DXR_LOD"),
                      rayProxy = ReadEnvironmentFlagOnce("DX12_DXR_PROXY");
    if (rayLod || rayProxy)
    {
        constexpr float ratios[] = {.65f, .3f, .1f}, errors[] = {.005f, .015f, .04f};
        for (UINT lod = 0; lod < StaticMeshData::RayLodCount; ++lod)
        {
            vector<unsigned int> simplified(indices.size());
            const size_t target = (std::max)(size_t(3), size_t(indices.size() * ratios[lod]) / 3 * 3);
            const size_t count = meshopt_simplify(
                simplified.data(), indices.data(), indices.size(), reinterpret_cast<const float*>(vertices.data()),
                vertices.size(), sizeof(StaticModelVertex), target, errors[lod], meshopt_SimplifyRegularize);
            if (count < 3 || count >= indices.size())
            {
                continue;
            }
            simplified.resize(count);
            meshopt_optimizeVertexCache(simplified.data(), simplified.data(), count, vertices.size());
            const UINT bytes = static_cast<UINT>(count * sizeof(unsigned int));
            if (!CreateDefaultBufferAndUpload(device, bytes, simplified.data(), D3D12_RESOURCE_STATE_INDEX_BUFFER,
                                              "Ray LOD allocation failed\n", "ray LOD", meshData.RayLodBuffers[lod]))
            {
                continue;
            }
            meshData.RayLodViews[lod] = {meshData.RayLodBuffers[lod]->GetGPUVirtualAddress(), bytes,
                                         DXGI_FORMAT_R32_UINT};
            meshData.RayLodCounts[lod] = static_cast<UINT>(count);
        }
        XMFLOAT3 lo = vertices.front().Position, hi = lo;
        for (const auto& v : vertices)
        {
            lo.x = (std::min)(lo.x, v.Position.x);
            lo.y = (std::min)(lo.y, v.Position.y);
            lo.z = (std::min)(lo.z, v.Position.z);
            hi.x = (std::max)(hi.x, v.Position.x);
            hi.y = (std::max)(hi.y, v.Position.y);
            hi.z = (std::max)(hi.z, v.Position.z);
        }
        meshData.RayCenter = {(lo.x + hi.x) * .5f, (lo.y + hi.y) * .5f, (lo.z + hi.z) * .5f};
        meshData.RayRadius =
            .5f * sqrtf((hi.x - lo.x) * (hi.x - lo.x) + (hi.y - lo.y) * (hi.y - lo.y) + (hi.z - lo.z) * (hi.z - lo.z));
        if (rayProxy && meshData.RayRadius > 1e-5f)
        {
            vector<StaticModelVertex> proxy(8);
            for (UINT corner = 0; corner < 8; ++corner)
            {
                auto& v = proxy[corner];
                v.Position = {(corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z};
                XMStoreFloat3(&v.Normal,
                              XMVector3Normalize(XMLoadFloat3(&v.Position) - XMLoadFloat3(&meshData.RayCenter)));
                v.Diffuse = {1, 1, 1, 1};
            }
            const unsigned int box[] = {0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4,
                                        2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5};
            if (CreateDefaultBufferAndUpload(device, UINT(proxy.size() * sizeof(StaticModelVertex)), proxy.data(),
                                             D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                             "Ray proxy allocation failed\n", "ray proxy vertices",
                                             meshData.RayProxyVertices) &&
                CreateDefaultBufferAndUpload(device, sizeof(box), box, D3D12_RESOURCE_STATE_INDEX_BUFFER,
                                             "Ray proxy allocation failed\n", "ray proxy indices",
                                             meshData.RayProxyIndices))
            {
                meshData.RayProxyVertexView = {meshData.RayProxyVertices->GetGPUVirtualAddress(),
                                               UINT(proxy.size() * sizeof(StaticModelVertex)),
                                               sizeof(StaticModelVertex)};
                meshData.RayProxyIndexView = {meshData.RayProxyIndices->GetGPUVirtualAddress(), sizeof(box),
                                              DXGI_FORMAT_R32_UINT};
            }
        }
    }
    return true;
}

namespace
{
    // OBJ 高速パーサー用の補助。iostream / istringstream / sscanf を使わず、
    // ファイル全体を 1 回読み込んで std::from_chars で直接数値化する。
    const char* ObjSkipSpaces(const char* p, const char* end)
    {
        while (p < end && (*p == ' ' || *p == '\t'))
        {
            ++p;
        }
        return p;
    }

    bool ObjParseFloat(const char*& p, const char* end, float& value)
    {
        p = ObjSkipSpaces(p, end);
        if (p < end && *p == '+')
        {
            ++p;
        }
        const auto result = std::from_chars(p, end, value);
        if (result.ec != std::errc{})
        {
            return false;
        }
        p = result.ptr;
        return true;
    }

    bool ObjParseInt(const char*& p, const char* end, int& value)
    {
        if (p < end && *p == '+')
        {
            ++p;
        }
        const auto result = std::from_chars(p, end, value);
        if (result.ec != std::errc{})
        {
            return false;
        }
        p = result.ptr;
        return true;
    }

    bool ObjLineStarts(const char* p, const char* end, const char* keyword, size_t length)
    {
        return static_cast<size_t>(end - p) >= length && memcmp(p, keyword, length) == 0;
    }

    struct ObjCornerKey
    {
        unsigned int V, T, N;
        bool operator==(const ObjCornerKey& other) const { return V == other.V && T == other.T && N == other.N; }
    };

    struct ObjCornerHash
    {
        size_t operator()(const ObjCornerKey& key) const
        {
            uint64_t h = key.V * 0x9E3779B97F4A7C15ull;
            h ^= (key.T + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2));
            h ^= (key.N * 0x85EBCA77C2B2AE63ull + (h << 6) + (h >> 2));
            return static_cast<size_t>(h);
        }
    };
}

bool StaticModelResource::LoadObj(const char* fileName, ID3D12Device* device)
{
    LoadLODMetadata(fileName);
    const filesystem::path objPath = ModelPathFromUtf8(fileName);

    // ファイルを 1 回だけ読み込む（従来は usemtl 走査と本解析で 2 回、getline で読んでいた）。
    string fileData;
    {
        ifstream ifs(objPath, ios::binary | ios::ate);
        if (!ifs)
        {
            char buf[512];
            sprintf_s(buf, "ERROR: Failed to open OBJ file: %s\n", fileName);
            Debug::Log("%s", buf);
            return false;
        }
        const auto size = static_cast<size_t>(ifs.tellg());
        fileData.resize(size);
        ifs.seekg(0);
        if (size > 0 && !ifs.read(fileData.data(), static_cast<streamsize>(size)))
        {
            Debug::Log("ERROR: Failed to read OBJ file: %s\n", fileName);
            return false;
        }
    }
    const char* const dataBegin = fileData.data();
    const char* const dataEnd = dataBegin + fileData.size();
    const auto nextLine = [dataEnd](const char* p, const char*& lineEnd) {
        const char* newline = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(dataEnd - p)));
        lineEnd = newline ? newline : dataEnd;
        const char* following = newline ? newline + 1 : dataEnd;
        if (lineEnd > p && lineEnd[-1] == '\r')
        {
            --lineEnd;
        }
        return following;
    };

    // 前走査: マテリアルセクション数と各要素数を数え、配列を一括確保する。
    // 旧 OBJ インポーターは全ポリゴンを単一セクションにまとめる。その単一マテリアルの
    // 動作を保持しつつ、Assimp 経由では設定済みのマテリアルセクションを保持する。
    unordered_set<string> sectionMaterials;
    size_t positionCount = 0, texCoordCount = 0, normalCount = 0, faceCount = 0;
    for (const char* p = dataBegin; p < dataEnd;)
    {
        const char* lineEnd;
        const char* following = nextLine(p, lineEnd);
        if (lineEnd - p >= 2)
        {
            if (p[0] == 'v' && p[1] == ' ') ++positionCount;
            else if (p[0] == 'v' && p[1] == 't') ++texCoordCount;
            else if (p[0] == 'v' && p[1] == 'n') ++normalCount;
            else if (p[0] == 'f' && p[1] == ' ') ++faceCount;
            else if (ObjLineStarts(p, lineEnd, "usemtl ", 7))
            {
                sectionMaterials.insert(ObjStatementArgument(string(p, lineEnd), 7));
            }
        }
        p = following;
    }
    if (sectionMaterials.size() > 1)
    {
        return LoadAssimpModel(fileName, device, false);
    }
    m_MaterialPath.clear();

    Reserve();
    m_Positions.reserve(positionCount);
    m_TexCoords.reserve(texCoordCount);
    m_Normals.reserve(normalCount);
    // 頂点は (v, vt, vn) の組で共有する。従来は角ごとに頂点を複製していたため、
    // 頂点数が約 6 倍になり、TEO 生成・LOD 簡略化・メッシュレット構築・GPU 転送がすべて重くなっていた。
    m_Vertices.reserve((std::max)(positionCount, normalCount));
    m_Indices.reserve(faceCount * 3);

    unordered_map<string, MtlData> materialMap;
    XMFLOAT4 currentKd = {1.0f, 1.0f, 1.0f, 1.0f};
    // 任意の頂点色拡張 "v x y z r g b"（Blender、MeshLab、Procedural Studio）。
    // 位置と同じインデックスを使い、マテリアルの Kd と乗算する。
    vector<XMFLOAT3> vertexColors;
    vertexColors.reserve(positionCount);
    bool hasVertexColors = false;

    // v == vt == vn の角（Procedural Studio の出力はすべてこれ）は配列で、それ以外はハッシュで共有する。
    vector<unsigned int> uniformCorners;
    unordered_map<ObjCornerKey, unsigned int, ObjCornerHash> mixedCorners;
    const auto resetCornerCache = [&]() {
        uniformCorners.assign(uniformCorners.size(), UINT_MAX);
        mixedCorners.clear();
    };

    vector<unsigned int> vi, ti, ni;
    for (const char* lineStart = dataBegin; lineStart < dataEnd;)
    {
        const char* lineEnd;
        const char* following = nextLine(lineStart, lineEnd);
        const char* p = lineStart;
        if (lineEnd - p < 2)
        {
            lineStart = following;
            continue;
        }
        if (p[0] == 'v' && p[1] == ' ')
        {
            p += 2;
            XMFLOAT3 pos{};
            XMFLOAT3 color{1.0f, 1.0f, 1.0f};
            (void)(ObjParseFloat(p, lineEnd, pos.x) && ObjParseFloat(p, lineEnd, pos.y) && ObjParseFloat(p, lineEnd, pos.z));
            XMFLOAT3 parsedColor{};
            if (ObjParseFloat(p, lineEnd, parsedColor.x) && ObjParseFloat(p, lineEnd, parsedColor.y) &&
                ObjParseFloat(p, lineEnd, parsedColor.z))
            {
                color = parsedColor;
                hasVertexColors = true;
            }
            m_Positions.push_back(pos);
            vertexColors.push_back(color);
        }
        else if (p[0] == 'v' && p[1] == 't' && lineEnd - p >= 3 && (p[2] == ' ' || p[2] == '\t'))
        {
            p += 3;
            XMFLOAT2 tex{};
            (void)(ObjParseFloat(p, lineEnd, tex.x) && ObjParseFloat(p, lineEnd, tex.y));
            tex.y = 1.0f - tex.y;
            m_TexCoords.push_back(tex);
        }
        else if (p[0] == 'v' && p[1] == 'n' && lineEnd - p >= 3 && (p[2] == ' ' || p[2] == '\t'))
        {
            p += 3;
            XMFLOAT3 normal{};
            (void)(ObjParseFloat(p, lineEnd, normal.x) && ObjParseFloat(p, lineEnd, normal.y) &&
                   ObjParseFloat(p, lineEnd, normal.z));
            m_Normals.push_back(normal);
        }
        else if (p[0] == 'f' && p[1] == ' ')
        {
            p += 2;
            // 任意の頂点数のポリゴンを扇状に三角形化する。負のインデックスは現在の末尾が基準。
            const auto resolve = [](int value, size_t size) -> unsigned int {
                if (value < 0)
                {
                    const long long index = static_cast<long long>(size) + value + 1;
                    return index > 0 ? static_cast<unsigned int>(index) : 0u;
                }
                return static_cast<unsigned int>(value);
            };
            vi.clear();
            ti.clear();
            ni.clear();
            while (true)
            {
                p = ObjSkipSpaces(p, lineEnd);
                if (p >= lineEnd)
                {
                    break;
                }
                // 書式: v, v/vt, v//vn, v/vt/vn
                int v = 0, tc = 0, n = 0;
                const char* tokenStart = p;
                ObjParseInt(p, lineEnd, v);
                if (p < lineEnd && *p == '/')
                {
                    ++p;
                    if (p < lineEnd && *p != '/')
                    {
                        ObjParseInt(p, lineEnd, tc);
                    }
                    if (p < lineEnd && *p == '/')
                    {
                        ++p;
                        ObjParseInt(p, lineEnd, n);
                    }
                }
                // 解析できない文字は飛ばして無限ループを防ぐ。
                while (p < lineEnd && *p != ' ' && *p != '\t')
                {
                    ++p;
                }
                if (p == tokenStart)
                {
                    ++p;
                }
                vi.push_back(resolve(v, m_Positions.size()));
                ti.push_back(resolve(tc, m_TexCoords.size()));
                ni.push_back(resolve(n, m_Normals.size()));
            }

            const auto corner = [&](size_t idx) -> unsigned int {
                const unsigned int v = vi[idx], t = ti[idx], n = ni[idx];
                unsigned int* slot = nullptr;
                if (v == t && v == n)
                {
                    if (v >= uniformCorners.size())
                    {
                        uniformCorners.resize((std::max)(static_cast<size_t>(v) + 1, m_Positions.size() + 1),
                                              UINT_MAX);
                    }
                    slot = &uniformCorners[v];
                }
                else
                {
                    slot = &mixedCorners.try_emplace(ObjCornerKey{v, t, n}, UINT_MAX).first->second;
                }
                if (*slot != UINT_MAX)
                {
                    return *slot;
                }
                StaticModelVertex vertex{};
                if (v > 0 && v <= (unsigned int)m_Positions.size())
                {
                    vertex.Position = m_Positions[v - 1];
                }
                if (n > 0 && n <= (unsigned int)m_Normals.size())
                {
                    vertex.Normal = m_Normals[n - 1];
                }
                if (t > 0 && t <= (unsigned int)m_TexCoords.size())
                {
                    vertex.TexCoord = m_TexCoords[t - 1];
                }
                vertex.Diffuse = currentKd;
                if (hasVertexColors && v > 0 && v <= (unsigned int)vertexColors.size())
                {
                    const XMFLOAT3& color = vertexColors[v - 1];
                    vertex.Diffuse.x *= color.x;
                    vertex.Diffuse.y *= color.y;
                    vertex.Diffuse.z *= color.z;
                }
                *slot = static_cast<unsigned int>(m_Vertices.size());
                m_Vertices.push_back(vertex);
                return *slot;
            };
            for (size_t i = 0; i + 2 < vi.size(); ++i)
            {
                m_Indices.push_back(corner(0));
                m_Indices.push_back(corner(i + 1));
                m_Indices.push_back(corner(i + 2));
            }
        }
        else if (ObjLineStarts(p, lineEnd, "mtllib ", 7))
        {
            const string mtlName = ObjStatementArgument(string(p, lineEnd), 7);
            const auto dir = objPath.parent_path();
            const filesystem::path mtlPath =
                dir.empty() ? ModelPathFromUtf8(mtlName.c_str()) : dir / ModelPathFromUtf8(mtlName.c_str());

            materialMap = ParseMtl(ModelPathToUtf8(mtlPath));

            for (auto const& pair : materialMap)
            {
                const MtlData& data = pair.second;
                if (!data.MapKd.empty() && m_MaterialPath.empty())
                {
                    const auto texture = ModelPathFromUtf8(data.MapKd.c_str());
                    m_MaterialPath = ModelPathToUtf8(dir.empty() || texture.is_absolute() ? texture : dir / texture);
                }
            }
        }
        else if (ObjLineStarts(p, lineEnd, "usemtl ", 7))
        {
            const string mtlName = ObjStatementArgument(string(p, lineEnd), 7);
            if (!mtlName.empty())
            {
                auto it = materialMap.find(mtlName);
                if (it != materialMap.end())
                {
                    currentKd = it->second.Kd;
                    // Kd が変わると頂点色も変わるため、以降の角は共有しない。
                    resetCornerCache();
                }
            }
        }
        lineStart = following;
    }

    if (m_Vertices.empty())
    {
        Debug::Log("ERROR: OBJ file has no vertices\n");
        return false;
    }

    XMFLOAT3 minPos = {FLT_MAX, FLT_MAX, FLT_MAX};
    XMFLOAT3 maxPos = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const auto& v : m_Vertices)
    {
        if (v.Position.x < minPos.x)
        {
            minPos.x = v.Position.x;
        }
        if (v.Position.y < minPos.y)
        {
            minPos.y = v.Position.y;
        }
        if (v.Position.z < minPos.z)
        {
            minPos.z = v.Position.z;
        }

        if (v.Position.x > maxPos.x)
        {
            maxPos.x = v.Position.x;
        }
        if (v.Position.y > maxPos.y)
        {
            maxPos.y = v.Position.y;
        }
        if (v.Position.z > maxPos.z)
        {
            maxPos.z = v.Position.z;
        }
    }

    m_AabbCenter.x = (minPos.x + maxPos.x) * 0.5f;
    m_AabbCenter.y = (minPos.y + maxPos.y) * 0.5f;
    m_AabbCenter.z = (minPos.z + maxPos.z) * 0.5f;

    m_AabbExtents.x = (maxPos.x - minPos.x) * 0.5f;
    m_AabbExtents.y = (maxPos.y - minPos.y) * 0.5f;
    m_AabbExtents.z = (maxPos.z - minPos.z) * 0.5f;

    StaticMeshData meshData{};
    meshData.MaterialIndex = 0;
    meshData.MeshName = "OBJ Mesh";
    meshData.MaterialName = "";
    meshData.MaterialPartId = 10.0f;
    meshData.AppliedMaterialPartId = meshData.MaterialPartId;
    meshData.DefaultToonOutlineEnabled = true;
    meshData.CpuVertices = m_Vertices;

    const UINT vertexBufferSize = sizeof(StaticModelVertex) * (UINT)m_Vertices.size();
    {
        D3D12_RESOURCE_DESC vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr =
            device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_COMMON,
                                            nullptr, IID_PPV_ARGS(&meshData.VertexBuffer));
        if (FAILED(hr) || !meshData.VertexBuffer)
        {
            Debug::Log("ERROR: Failed to create DEFAULT vertex buffer for static model\n");
            return false;
        }

        if (!UploadBufferData(device, meshData.VertexBuffer.Get(), m_Vertices.data(), vertexBufferSize,
                              D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "static vertex"))
        {
            return false;
        }
    }

    meshData.VertexBufferView.BufferLocation = meshData.VertexBuffer->GetGPUVirtualAddress();
    meshData.VertexBufferView.StrideInBytes = sizeof(StaticModelVertex);
    meshData.VertexBufferView.SizeInBytes = vertexBufferSize;
    meshData.VertexCount = (UINT)m_Vertices.size();

    // Perf: OBJパスも Assimp パスと同様にインデックス順を最適化。
    if (!m_Indices.empty() && !m_Vertices.empty())
    {
        meshopt_optimizeVertexCache(m_Indices.data(), m_Indices.data(), m_Indices.size(), m_Vertices.size());
        meshopt_optimizeOverdraw(m_Indices.data(), m_Indices.data(), m_Indices.size(),
                                 reinterpret_cast<const float*>(m_Vertices.data()), m_Vertices.size(),
                                 sizeof(StaticModelVertex), 1.05f);
    }

    const UINT indexBufferSize = sizeof(unsigned int) * (UINT)m_Indices.size();
    {
        D3D12_RESOURCE_DESC ibDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr =
            device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &ibDesc, D3D12_RESOURCE_STATE_COMMON,
                                            nullptr, IID_PPV_ARGS(&meshData.IndexBuffer));
        if (FAILED(hr) || !meshData.IndexBuffer)
        {
            Debug::Log("ERROR: Failed to create DEFAULT index buffer for static model\n");
            return false;
        }

        if (!UploadBufferData(device, meshData.IndexBuffer.Get(), m_Indices.data(), indexBufferSize,
                              D3D12_RESOURCE_STATE_INDEX_BUFFER, "static index"))
        {
            return false;
        }
    }

    meshData.IndexBufferView.BufferLocation = meshData.IndexBuffer->GetGPUVirtualAddress();
    meshData.IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    meshData.IndexBufferView.SizeInBytes = indexBufferSize;
    meshData.IndexCount = (UINT)m_Indices.size();
    meshData.CpuIndices.assign(m_Indices.begin(), m_Indices.end());

    {
        for (int mode = 0; mode < kToonOutlineModeCount; ++mode)
        {
            vector<StaticModelVertex> teoVertices;
            vector<unsigned int> teoIndices;
            BuildTeoMesh(m_Vertices, m_Indices, teoVertices, teoIndices, static_cast<ToonOutlineMeshMode>(mode));
            if (teoVertices.empty() || teoIndices.empty())
            {
                continue;
            }
            meshData.CpuTeoVerticesByMode[mode] = teoVertices;

            const UINT teoVertexBufferSize = sizeof(StaticModelVertex) * (UINT)teoVertices.size();
            if (!CreateDefaultBufferAndUpload(device, teoVertexBufferSize, teoVertices.data(),
                                              D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                              "ERROR: Failed to create DEFAULT TEO vertex buffer for static model\n",
                                              "static teo vertex", meshData.TeoVertexBuffers[mode]))
            {
                return false;
            }
            meshData.TeoVertexBufferViews[mode].BufferLocation =
                meshData.TeoVertexBuffers[mode]->GetGPUVirtualAddress();
            meshData.TeoVertexBufferViews[mode].StrideInBytes = sizeof(StaticModelVertex);
            meshData.TeoVertexBufferViews[mode].SizeInBytes = teoVertexBufferSize;
            meshData.TeoVertexCounts[mode] = (UINT)teoVertices.size();

            const UINT teoIndexBufferSize = sizeof(unsigned int) * (UINT)teoIndices.size();
            if (!CreateDefaultBufferAndUpload(device, teoIndexBufferSize, teoIndices.data(),
                                              D3D12_RESOURCE_STATE_INDEX_BUFFER,
                                              "ERROR: Failed to create DEFAULT TEO index buffer for static model\n",
                                              "static teo index", meshData.TeoIndexBuffers[mode]))
            {
                return false;
            }
            meshData.TeoIndexBufferViews[mode].BufferLocation = meshData.TeoIndexBuffers[mode]->GetGPUVirtualAddress();
            meshData.TeoIndexBufferViews[mode].Format = DXGI_FORMAT_R32_UINT;
            meshData.TeoIndexBufferViews[mode].SizeInBytes = teoIndexBufferSize;
            meshData.TeoIndexCounts[mode] = (UINT)teoIndices.size();

            if (mode == static_cast<int>(ToonOutlineMeshMode::Balanced))
            {
                meshData.TeoVertexBuffer = meshData.TeoVertexBuffers[mode];
                meshData.TeoIndexBuffer = meshData.TeoIndexBuffers[mode];
                meshData.TeoVertexBufferView = meshData.TeoVertexBufferViews[mode];
                meshData.TeoIndexBufferView = meshData.TeoIndexBufferViews[mode];
                meshData.TeoVertexCount = meshData.TeoVertexCounts[mode];
                meshData.TeoIndexCount = meshData.TeoIndexCounts[mode];
            }
        }
    }

    if (!m_MaterialPath.empty())
    {
        meshData.TextureIndex = TextureManager::LoadTexture(m_MaterialPath.c_str());
    }
    if (!BuildLodIndexBuffers(device, m_Vertices, m_Indices, meshData))
    {
        return false;
    }

    meshData.VirtualGeometrySettings = m_VGSettings;
    MeshletManager::BuildImportedMesh(meshData);
    m_Meshes.push_back(move(meshData));
    ModelMapImport::AddMeshes(m_ResourceMap, m_Meshes);

    return true;
}

bool AppendVirtualGeometryCluster(const VirtualGeometryGPUCluster& cluster, std::span<const uint8_t> page,
                                  std::vector<StaticModelVertex>& vertices, std::vector<uint32_t>& indices)
{
    if (vertices.size() + cluster.VertexCount > UINT32_MAX ||
        indices.size() + uint64_t(cluster.TriangleCount) * 3 > UINT32_MAX)
    {
        return false;
    }
    const uint32_t first = static_cast<uint32_t>(vertices.size());
    const bool encoded = (cluster.Flags & VirtualGeometryCompression::ClusterFlag) != 0;
    const auto payload = page.subspan(cluster.VertexOffset);
    for (uint32_t index = 0; index < cluster.VertexCount; ++index)
    {
        StaticModelVertex vertex;
        if (encoded)
        {
            const auto decoded = VirtualGeometryCompression::DecodeVertexCPU(payload, index);
            vertex.Position = {decoded.Position[0], decoded.Position[1], decoded.Position[2]};
            vertex.Normal = {decoded.Normal[0], decoded.Normal[1], decoded.Normal[2]};
            if (!decoded.Texcoords.empty())
            {
                vertex.TexCoord = {decoded.Texcoords[0][0], decoded.Texcoords[0][1]};
            }
            vertex.Diffuse = {decoded.Color[0], decoded.Color[1], decoded.Color[2], decoded.Color[3]};
        }
        else
        {
            static_assert(sizeof(StaticModelVertex) == sizeof(VirtualGeometryVertex));
            memcpy(&vertex, payload.data() + size_t(index) * sizeof(StaticModelVertex), sizeof(vertex));
        }
        vertices.push_back(vertex);
    }
    for (uint32_t triangle = 0; triangle < cluster.TriangleCount; ++triangle)
    {
        std::array<uint32_t, 3> triangleIndices;
        if (encoded)
        {
            triangleIndices = VirtualGeometryCompression::LoadTriangleCPU(payload, triangle);
        }
        else
        {
            const size_t offset = size_t(cluster.TriangleOffset) + size_t(triangle) * 3;
            triangleIndices = {page[offset], page[offset + 1], page[offset + 2]};
        }
        for (const auto index : triangleIndices)
        {
            if (index >= cluster.VertexCount)
            {
                return false;
            }
            indices.push_back(first + index);
        }
    }
    return true;
}

bool StaticModelResource::LoadVirtualGeometry(const char* fileName, ID3D12Device* device)
{
    try
    {
        LoadLODMetadata(fileName);
        auto manifest = std::make_shared<VirtualGeometryPageCache::PageManifest>();
        std::string error;
        if (!device || !VirtualGeometryPageCache::Open(ModelPathFromUtf8(fileName), *manifest, error))
        {
            Debug::Log("VG import failed: %s\n", error.c_str());
            return false;
        }
        manifest->StreamingPriority = m_VGSettings.StreamingPriority;
        StaticMeshData mesh;
        mesh.VirtualGeometryRootFallback = true;
        mesh.MeshName = ModelPathToUtf8(ModelPathFromUtf8(fileName).stem());
        mesh.VirtualGeometrySettings = m_VGSettings;
        mesh.AppliedMaterialPartId = float(ShaderClass::PBR);
        mesh.MaterialPartId = mesh.AppliedMaterialPartId;
        // 非対応デバイス・影・ピッキングの代替経路では、常駐固定されたルートだけを展開する。
        // 詳細ジオメトリはディスク上の符号化状態を保ち、完全な頂点バッファとしてはアップロードしない。
        std::vector<uint8_t> page(VirtualGeometryPages::PageBytes);
        uint32_t lastPage = UINT32_MAX;
        XMFLOAT3 lower{INFINITY, INFINITY, INFINITY}, upper{-INFINITY, -INFINITY, -INFINITY};
        for (const auto root : manifest->Metadata.Roots)
        {
            const auto& cluster = manifest->Metadata.Clusters.at(root);
            if (cluster.PageIndex != lastPage)
            {
                if (!VirtualGeometryPageCache::ReadPage(*manifest, cluster.PageIndex, page, error))
                {
                    Debug::Log("VG root fallback page failed: %s\n", error.c_str());
                    return false;
                }
                lastPage = cluster.PageIndex;
            }
            if (!AppendVirtualGeometryCluster(cluster, page, mesh.CpuVertices, mesh.CpuIndices))
            {
                return false;
            }
            mesh.VirtualGeometryRootTriangleCounts.push_back(cluster.TriangleCount);
            lower.x = std::min(lower.x, cluster.Bounds[0] - cluster.Bounds[3]);
            lower.y = std::min(lower.y, cluster.Bounds[1] - cluster.Bounds[3]);
            lower.z = std::min(lower.z, cluster.Bounds[2] - cluster.Bounds[3]);
            upper.x = std::max(upper.x, cluster.Bounds[0] + cluster.Bounds[3]);
            upper.y = std::max(upper.y, cluster.Bounds[1] + cluster.Bounds[3]);
            upper.z = std::max(upper.z, cluster.Bounds[2] + cluster.Bounds[3]);
        }
        if (mesh.CpuVertices.empty() || mesh.CpuIndices.empty() ||
            mesh.CpuVertices.size() > UINT32_MAX / sizeof(StaticModelVertex) || mesh.CpuIndices.size() > UINT32_MAX / 4)
        {
            return false;
        }
        mesh.VertexCount = static_cast<UINT>(mesh.CpuVertices.size());
        mesh.IndexCount = static_cast<UINT>(mesh.CpuIndices.size());
        const UINT vertexBytes = mesh.VertexCount * sizeof(StaticModelVertex), indexBytes = mesh.IndexCount * 4;
        if (!CreateDefaultBufferAndUpload(device, vertexBytes, mesh.CpuVertices.data(),
                                          D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                          "VG root vertex allocation failed", "VG root vertex", mesh.VertexBuffer) ||
            !CreateDefaultBufferAndUpload(device, indexBytes, mesh.CpuIndices.data(), D3D12_RESOURCE_STATE_INDEX_BUFFER,
                                          "VG root index allocation failed", "VG root index", mesh.IndexBuffer))
        {
            return false;
        }
        mesh.VertexBufferView = {mesh.VertexBuffer->GetGPUVirtualAddress(), vertexBytes, sizeof(StaticModelVertex)};
        mesh.IndexBufferView = {mesh.IndexBuffer->GetGPUVirtualAddress(), indexBytes, DXGI_FORMAT_R32_UINT};
        auto runtime = std::make_shared<VirtualGeometryRuntime::RuntimeAsset>();
        runtime->Key = manifest->Identity;
        runtime->StreamRayGeometry = true;
        runtime->CachePath = manifest->Path;
        runtime->ImportSettings = m_VGSettings;
        runtime->Hierarchy.Stats.SourceTriangles = manifest->SourceTriangles;
        runtime->Hierarchy.Stats.SourceVertices = manifest->SourceVertices;
        runtime->Hierarchy.Stats.TotalClusters = static_cast<uint32_t>(manifest->Metadata.Clusters.size());
        runtime->Hierarchy.Stats.TotalGroups = static_cast<uint32_t>(manifest->Metadata.Groups.size());
        for (const auto& cluster : manifest->Metadata.Clusters)
        {
            runtime->Hierarchy.Stats.HierarchyLevels =
                std::max(runtime->Hierarchy.Stats.HierarchyLevels, cluster.Level + 1);
        }
        runtime->Pages = std::move(manifest);
        mesh.VirtualGeometryLODs[0] = std::move(runtime);
        m_AabbCenter = {(lower.x + upper.x) * 0.5f, (lower.y + upper.y) * 0.5f, (lower.z + upper.z) * 0.5f};
        m_AabbExtents = {(upper.x - lower.x) * 0.5f, (upper.y - lower.y) * 0.5f, (upper.z - lower.z) * 0.5f};
        m_ResourceMap.Clear();
        m_Meshes.clear();
        mesh.MaterialIndex = 0;
        m_Meshes.push_back(std::move(mesh));
        ModelMapImport::AddMeshes(m_ResourceMap, m_Meshes);
        return true;
    }
    catch (const std::exception& error)
    {
        Debug::Log("VG import failed safely: %s\n", error.what());
        return false;
    }
}

namespace
{
    uint64_t HashSourceFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return 0;
        }
        uint64_t hash = 14695981039346656037ull;
        std::array<char, 65536> bytes;
        while (input)
        {
            input.read(bytes.data(), bytes.size());
            for (std::streamsize index = 0; index < input.gcount(); ++index)
            {
                hash = (hash ^ static_cast<unsigned char>(bytes[index])) * 1099511628211ull;
            }
        }
        return input.bad() ? 0 : hash;
    }

    std::vector<std::filesystem::path> GeometryDependencies(const std::filesystem::path& source)
    {
        std::vector<std::filesystem::path> paths{source};
        std::string extension = source.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char letter) {
            return static_cast<char>(std::tolower(letter));
        });
        if (extension == ".gltf" || extension == ".glb")
        {
            std::ifstream input(source, std::ios::binary);
            Json::Value document;
            Json::CharReaderBuilder reader;
            std::string error;
            if (extension == ".glb")
            {
                std::array<uint32_t, 5> header{};
                input.read(reinterpret_cast<char*>(header.data()), sizeof(header));
                if (!input || header[0] != 0x46546c67u || header[1] != 2 || header[4] != 0x4e4f534au ||
                    header[3] > 16u * 1024 * 1024 || header[2] < sizeof(header) ||
                    header[3] > header[2] - sizeof(header))
                {
                    return {};
                }
                std::string json(header[3], '\0');
                if (!input.read(json.data(), json.size()))
                {
                    return {};
                }
                std::istringstream jsonStream(json);
                if (!Json::parseFromStream(reader, jsonStream, &document, &error))
                {
                    return {};
                }
            }
            else if (!Json::parseFromStream(reader, input, &document, &error))
            {
                return {};
            }
            for (const auto& buffer : document["buffers"])
            {
                if (extension == ".glb" && !buffer.isMember("uri"))
                {
                    continue;
                }
                const auto uri = buffer["uri"].asString();
                if (uri.rfind("data:", 0) != 0)
                {
                    // URI エスケープにはインポーターが必要。依存関係を見落とさないよう高速キャッシュを使わない。
                    if (uri.empty() || uri.find('%') != std::string::npos)
                    {
                        return {};
                    }
                    paths.push_back(source.parent_path() / std::filesystem::u8path(uri));
                }
            }
        }
        if (extension == ".obj")
        {
            std::ifstream input(source);
            std::string line;
            while (std::getline(input, line))
            {
                if (line.rfind("mtllib ", 0) == 0)
                {
                    // CRLF ファイルの UTF-8 名から前後の空白を除く。残った '\r' が毎回依存ハッシュを不一致にしていた。
                    paths.push_back(source.parent_path() / ModelPathFromUtf8(ObjStatementArgument(line, 7).c_str()));
                }
            }
        }
        return paths;
    }
}

// 元インポーターの生成内容を変更したら更新する（3: 共有マップ・元マテリアル番号の保存）。
// 旧インポーターが作成した派生仮想ジオメトリキャッシュはソースから再構築する。
constexpr unsigned kSourceImporterRevision = 3;

bool StaticModelResource::LoadDerivedVirtualGeometry(const char* fileName, ID3D12Device* device, bool convert)
{
    try
    {
        LoadLODMetadata(fileName);
        if (!m_VGSettings.Enabled || GetGeometryRenderingMode() == GeometryRenderingMode::Legacy)
        {
            return false;
        }
        char compressionOverride[8]{};
        if (GetEnvironmentVariableA("DX12_VG_COMPRESSION_RAW", compressionOverride, sizeof(compressionOverride)))
        {
            return false;
        }
        const auto sourcePath = ModelPathFromUtf8(fileName);
        const auto indexPath = CachePaths::SourceEntry("VirtualGeometry/Models", sourcePath, ".vgderived");
        auto legacyPath = sourcePath;
        legacyPath += ".vgderived";
        std::error_code migrationError;
        if (!std::filesystem::exists(indexPath) && std::filesystem::exists(legacyPath))
        {
            std::filesystem::create_directories(indexPath.parent_path(), migrationError);
            std::filesystem::rename(legacyPath, indexPath, migrationError);
        }
        std::vector<char> bytes;
        if (!CacheFile::Load(indexPath, bytes, 64 * 1024 * 1024))
        {
            return false;
        }
        std::istringstream input(std::string(bytes.begin(), bytes.end()));
        Json::Value index;
        Json::CharReaderBuilder reader;
        std::string error;
        if (!Json::parseFromStream(reader, input, &index, &error) ||
            index["builder"].asUInt() != VirtualGeometryConfig::BuilderVersion ||
            index["compression"].asUInt() != VirtualGeometryCompression::CompressionVersion ||
            index["convert"].asBool() != convert ||
            index["settings"].asString() != std::to_string(VirtualGeometryImport::SettingsKey(m_VGSettings)) ||
            index["sourceImporter"].asUInt() != kSourceImporterRevision)
        {
            return false;
        }
        const auto dependencies = GeometryDependencies(ModelPathFromUtf8(fileName));
        if (dependencies.empty() || index["dependencies"].size() != dependencies.size())
        {
            return false;
        }
        for (Json::ArrayIndex entry = 0; entry < dependencies.size(); ++entry)
        {
            const auto hash = HashSourceFile(dependencies[entry]);
            if (!hash || index["dependencies"][entry].asString() != std::to_string(hash))
            {
                return false;
            }
        }
        if (!index["meshes"].isArray() || index["meshes"].empty() || index["meshes"].size() > 65536)
        {
            return false;
        }
        ModelResourceMap sourceMap;
        if (!ModelMapJson::Read(index["resourceMap"], sourceMap)) return false;
        const std::string originalSource = fileName;
        const auto settings = m_VGSettings;
        std::vector<StaticMeshData> meshes;
        XMFLOAT3 lower{INFINITY, INFINITY, INFINITY}, upper{-INFINITY, -INFINITY, -INFINITY};
        for (const auto& entry : index["meshes"])
        {
            std::filesystem::path path = std::filesystem::u8path(entry["path"].asString());
            const auto oldCache = std::filesystem::weakly_canonical("asset/virtualgeometry-cache");
            const auto oldRelative = std::filesystem::weakly_canonical(path).lexically_relative(oldCache);
            if (!oldRelative.empty() && !oldRelative.is_absolute() && *oldRelative.begin() != "..")
            {
                path = CachePaths::Directory("VirtualGeometry") / oldRelative;
            }
            const auto cache = std::filesystem::weakly_canonical(CachePaths::Directory("VirtualGeometry"));
            const auto relative = std::filesystem::weakly_canonical(path).lexically_relative(cache);
            if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            {
                return false;
            }
            if (!LoadVirtualGeometry(ModelPathToUtf8(path).c_str(), device))
            {
                return false;
            }
            auto mesh = std::move(m_Meshes.front());
            mesh.MeshName = entry["name"].asString();
            mesh.MaterialName = entry["material"].asString();
            if (!entry["materialIndex"].isInt() || entry["materialIndex"].asInt() < 0) return false;
            mesh.MaterialIndex = entry["materialIndex"].asInt();
            const auto* materialIds = sourceMap.Find(ModelResourceMap::Materials, mesh.MaterialName);
            if (!materialIds || std::find(materialIds->begin(), materialIds->end(),
                                          static_cast<uint32_t>(mesh.MaterialIndex)) == materialIds->end()) return false;
            mesh.MaterialPartId = entry["part"].asFloat();
            mesh.AppliedMaterialPartId = mesh.MaterialPartId;
            {
                MeshSemanticInfo semantic = ResolveMeshSemantics(mesh.MaterialName, mesh.MeshName,
                                                                 static_cast<unsigned int>(mesh.MaterialIndex));
                if (semantic.MaterialPartId >= static_cast<float>(MeshShadingOverride::ToonSclera) &&
                    mesh.MaterialPartId != semantic.MaterialPartId)
                {
                    return false;
                }
                mesh.SemanticFlags = semantic.Flags;
                mesh.SemanticKey = std::move(semantic.Key);
            }
            mesh.VirtualGeometrySettings = settings;
            auto runtime = std::make_shared<VirtualGeometryRuntime::RuntimeAsset>(*mesh.VirtualGeometryLODs[0]);
            runtime->ImportSettings = settings;
            mesh.VirtualGeometryLODs[0] = std::move(runtime);
            if (!std::isfinite(mesh.MaterialPartId))
            {
                return false;
            }
            const auto texture = entry["texture"].asString();
            if (!texture.empty())
            {
                mesh.TextureIndex = TextureManager::LoadTexture(std::filesystem::u8path(texture));
            }
            if (m_MaterialPath.empty() && !texture.empty())
            {
                m_MaterialPath = texture;
            }
            meshes.push_back(std::move(mesh));
            lower.x = std::min(lower.x, m_AabbCenter.x - m_AabbExtents.x);
            lower.y = std::min(lower.y, m_AabbCenter.y - m_AabbExtents.y);
            lower.z = std::min(lower.z, m_AabbCenter.z - m_AabbExtents.z);
            upper.x = std::max(upper.x, m_AabbCenter.x + m_AabbExtents.x);
            upper.y = std::max(upper.y, m_AabbCenter.y + m_AabbExtents.y);
            upper.z = std::max(upper.z, m_AabbCenter.z + m_AabbExtents.z);
        }
        m_Meshes = std::move(meshes);
        m_ResourceMap = std::move(sourceMap);
        m_AabbCenter = {(lower.x + upper.x) * 0.5f, (lower.y + upper.y) * 0.5f, (lower.z + upper.z) * 0.5f};
        m_AabbExtents = {(upper.x - lower.x) * 0.5f, (upper.y - lower.y) * 0.5f, (upper.z - lower.z) * 0.5f};
        LoadLODMetadata(originalSource.c_str());
        m_UsesDerivedVG = true;
        m_DerivedVGPublished = true;
        CaptureSourceIdentity(ModelPathFromUtf8(originalSource.c_str()));
        return true;
    }
    catch (const std::exception&)
    {
        return false; // キャッシュが不正なら、いずれの場合もソースのインポーターへ進む。
    }
}

void StaticModelResource::PublishDerivedVirtualGeometry(bool convert)
{
    if (m_DerivedVGPublished || m_Meshes.empty() || m_SourcePath.empty() ||
        std::filesystem::path(m_SourcePath).extension() == ".vgmesh" ||
        std::filesystem::path(m_SourcePath).extension() == ".vgp2")
    {
        return;
    }
    try
    {
        for (const auto& mesh : m_Meshes)
        {
            const auto& runtime = mesh.VirtualGeometryLODs[0];
            if (mesh.MaterialPartId != mesh.AppliedMaterialPartId)
            {
                return;
            }
            if (!runtime || (!runtime->Pages && !VirtualGeometryResources::ManifestFor(runtime.get())))
            {
                return;
            }
        }
        Json::Value index;
        index["builder"] = VirtualGeometryConfig::BuilderVersion;
        index["compression"] = VirtualGeometryCompression::CompressionVersion;
        index["convert"] = convert;
        index["settings"] = std::to_string(VirtualGeometryImport::SettingsKey(m_VGSettings));
        index["sourceImporter"] = kSourceImporterRevision;
        index["resourceMap"] = ModelMapJson::Write(m_ResourceMap);
        if (index["resourceMap"].isNull()) return;
        const auto dependencies = GeometryDependencies(ModelPathFromUtf8(m_SourcePath.c_str()));
        if (dependencies.empty())
        {
            return;
        }
        if (dependencies != m_VGDependencies || dependencies.size() != m_VGDependencyHashes.size())
        {
            return;
        }
        for (size_t dependency = 0; dependency < dependencies.size(); ++dependency)
        {
            const uint64_t hash = HashSourceFile(dependencies[dependency]);
            if (!hash || hash != m_VGDependencyHashes[dependency])
            {
                return;
            }
            index["dependencies"].append(std::to_string(hash));
        }
        const auto textures = TextureManager::GetLoadedTextureInfos();
        for (const auto& mesh : m_Meshes)
        {
            const auto& runtime = mesh.VirtualGeometryLODs[0];
            const auto manifest =
                runtime ? (runtime->Pages ? runtime->Pages : VirtualGeometryResources::ManifestFor(runtime.get()))
                        : nullptr;
            if (!manifest)
            {
                return;
            }
            std::string registryError;
            MaterialManager::Registry().Register(manifest->Path, AssetType::Mesh, registryError);
            Json::Value entry;
            entry["path"] = manifest->Path.generic_string();
            entry["name"] = mesh.MeshName;
            entry["material"] = mesh.MaterialName;
            entry["materialIndex"] = mesh.MaterialIndex;
            entry["part"] = mesh.MaterialPartId;
            if (mesh.TextureIndex >= 0)
            {
                bool found = false;
                for (const auto& texture : textures)
                {
                    if (texture.SrvIndex == mesh.TextureIndex &&
                        std::filesystem::is_regular_file(std::filesystem::u8path(texture.Path)))
                    {
                        entry["texture"] = texture.Path;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    m_DerivedVGPublished = true; // 埋め込みテクスチャは元のインポーターが必要。
                    return;
                }
            }
            index["meshes"].append(entry);
        }
        const auto path =
            CachePaths::SourceEntry("VirtualGeometry/Models", ModelPathFromUtf8(m_SourcePath.c_str()), ".vgderived");
        std::ostringstream output;
        output << index;
        const std::string contents = output.str();
        m_DerivedVGPublished = CacheFile::Store(path, contents);
    }
    catch (const std::exception& error)
    {
        Debug::Log("VG source index publication skipped: %s\n", error.what());
    }
}

void StaticModelResource::CaptureSourceIdentity(const std::filesystem::path& path)
{
    try
    {
        m_VGDependencies = GeometryDependencies(path);
    }
    catch (const std::exception&)
    {
        m_VGDependencies.clear(); // 依存メタデータの不正は、通常のインポーターへエラーとして伝える。
    }
    m_VGDependencyHashes.clear();
    m_VGDependencyTimes.clear();
    for (const auto& dependency : m_VGDependencies)
    {
        m_VGDependencyHashes.push_back(HashSourceFile(dependency));
        std::error_code ec;
        m_VGDependencyTimes.push_back(std::filesystem::last_write_time(dependency, ec));
    }
}

bool StaticModelResource::SourceDependenciesChanged() const
{
    for (size_t index = 0; index < m_VGDependencies.size(); ++index)
    {
        std::error_code ec;
        if (std::filesystem::last_write_time(m_VGDependencies[index], ec) != m_VGDependencyTimes[index])
        {
            return true;
        }
    }
    return false;
}

bool StaticModelResource::LoadAssimpModel(const char* fileName, ID3D12Device* device, bool isConvert)
{
    LoadLODMetadata(fileName);
    if (!device)
    {
        Debug::Log("ERROR: Invalid device for static model: %s\n", fileName);
        return false;
    }

    m_ResourceMap.Clear();
    m_Meshes.clear();
    m_MaterialPath.clear();
    Clear();

    const filesystem::path modelPath = ModelPathFromUtf8(fileName);
    const bool isPmxModel = ModelPathLowerExtension(modelPath) == ".pmx";
    PmxModel pmxModel{};
    vector<vector<uint32_t>> pmxMeshVertexIndices{};
    bool ownsGeneratedScene = false;
    const aiScene* scene = nullptr;
    if (isPmxModel)
    {
        if (!LoadPmxModel(fileName, pmxModel))
        {
            return false;
        }
        scene = CreatePmxGeneratedScene(pmxModel, pmxMeshVertexIndices);
        ownsGeneratedScene = true;
    }
    else
    {
        unsigned int flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality |
                             aiProcess_GenSmoothNormals;
        if (isConvert)
        {
            flags |= aiProcess_ConvertToLeftHanded;
        }
        scene = ImportModelScene(fileName, flags);
    }
    auto releaseScene = [&]() {
        if (!scene)
        {
            return;
        }
        if (ownsGeneratedScene)
        {
            DestroyPmxGeneratedScene(const_cast<aiScene*>(scene));
        }
        else
        {
            ReleaseModelScene(scene);
        }
        scene = nullptr;
    };
    if (!scene || !scene->HasMeshes())
    {
        Debug::Log("ERROR: Failed to load static model: %s (%s)\n", fileName, aiGetErrorString());
        releaseScene();
        return false;
    }
    uint32_t nextBone = 0;
    ModelMapImport::AddNodes(m_ResourceMap, scene->mRootNode, nextBone);
    ModelMapImport::AddMaterials(m_ResourceMap, scene);
    const string dirPath = ModelPathToUtf8(modelPath.parent_path());

    XMFLOAT3 minPos = {FLT_MAX, FLT_MAX, FLT_MAX};
    XMFLOAT3 maxPos = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    const bool objVertexColors = ModelPathLowerExtension(modelPath) == ".obj";
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m)
    {
        aiMesh* mesh = scene->mMeshes[m];
        if (!mesh || mesh->mNumVertices == 0 || mesh->mNumFaces == 0)
        {
            continue;
        }

        aiColor3D diffuse(1.0f, 1.0f, 1.0f);
        float opacity = 1.0f;
        aiString materialName;
        if (mesh->mMaterialIndex < scene->mNumMaterials)
        {
            aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
            material->Get(AI_MATKEY_NAME, materialName);
            material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);
            material->Get(AI_MATKEY_OPACITY, opacity);
        }
        // 名前は上で取得済み。部位 ID と意味フラグを 1 回の走査で求める（マテリアル名の再取得を省く）。
        MeshSemanticInfo semantic{};
        if (mesh->mMaterialIndex < scene->mNumMaterials)
        {
            semantic = ResolveMeshSemantics(materialName.C_Str(), mesh->mName.C_Str(), mesh->mMaterialIndex);
        }
        const float materialPartId = semantic.MaterialPartId;
        // 頭部メッシュ（Face/Skin）は SNI 球面法線補間を有効にするため、Diffuse.w にフラグを埋め込む
        // materialPartId + 100.0f で頭部フラグを表現。
        const float diffuseW = (semantic.Flags & MeshSemantic_Head) != 0 ? materialPartId + 100.0f : materialPartId;
        const XMFLOAT4 meshDiffuse(diffuse.r, diffuse.g, diffuse.b, diffuseW);

        vector<StaticModelVertex> vertices(mesh->mNumVertices);
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
        {
            StaticModelVertex& vertex = vertices[v];
            vertex.Position = XMFLOAT3(mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z);
            if (mesh->HasNormals())
            {
                vertex.Normal = XMFLOAT3(mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z);
            }
            else
            {
                vertex.Normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
            }

            const auto texCoords = mesh->mTextureCoords[0];
            if (texCoords)
            {
                vertex.TexCoord = XMFLOAT2(texCoords[v].x, texCoords[v].y);
            }
            else
            {
                vertex.TexCoord = XMFLOAT2(0.0f, 0.0f);
            }
            vertex.Diffuse = meshDiffuse;
            if (objVertexColors && mesh->HasVertexColors(0))
            {
                // OBJ の "v x y z r g b"（複数マテリアルの OBJ は Assimp 経由）。OBJ だけを対象にする。
                // FBX / VRM には未使用の黒い頂点色が含まれることが多く、適用すると既存アセットが暗くなる。
                const aiColor4D& color = mesh->mColors[0][v];
                vertex.Diffuse.x *= color.r;
                vertex.Diffuse.y *= color.g;
                vertex.Diffuse.z *= color.b;
            }

            const XMFLOAT3& pos = vertex.Position;
            if (pos.x < minPos.x)
            {
                minPos.x = pos.x;
            }
            if (pos.y < minPos.y)
            {
                minPos.y = pos.y;
            }
            if (pos.z < minPos.z)
            {
                minPos.z = pos.z;
            }
            if (pos.x > maxPos.x)
            {
                maxPos.x = pos.x;
            }
            if (pos.y > maxPos.y)
            {
                maxPos.y = pos.y;
            }
            if (pos.z > maxPos.z)
            {
                maxPos.z = pos.z;
            }
        }

        vector<unsigned int> indices;
        indices.reserve(mesh->mNumFaces * 3);
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
        {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices != 3)
            {
                continue;
            }
            indices.push_back(face.mIndices[0]);
            indices.push_back(face.mIndices[1]);
            indices.push_back(face.mIndices[2]);
        }

        if (indices.empty())
        {
            continue;
        }

        // Perf: GPU頂点キャッシュ効率とオーバードロー削減のためインデックス順を最適化。
        // 頂点自体は並べ替えないので描画結果は不変。
        if (!vertices.empty())
        {
            meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), vertices.size());
            meshopt_optimizeOverdraw(indices.data(), indices.data(), indices.size(),
                                     reinterpret_cast<const float*>(vertices.data()), vertices.size(),
                                     sizeof(StaticModelVertex), 1.05f);
        }

        StaticMeshData meshData{};
        meshData.MaterialIndex = static_cast<int>(mesh->mMaterialIndex);
        meshData.MeshName = mesh->mName.C_Str();
        meshData.MaterialName = materialName.C_Str();
        meshData.MaterialPartId = materialPartId;
        meshData.AppliedMaterialPartId = meshData.MaterialPartId;
        meshData.SemanticFlags = semantic.Flags;
        meshData.SemanticKey = std::move(semantic.Key);
        meshData.DefaultToonOutlineEnabled = materialPartId != 3.0f;
        meshData.CpuVertices = vertices;
        meshData.CpuIndices.assign(indices.begin(), indices.end());
        const UINT vertexBufferSize = sizeof(StaticModelVertex) * (UINT)vertices.size();
        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
        HRESULT hr =
            device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_COMMON,
                                            nullptr, IID_PPV_ARGS(&meshData.VertexBuffer));
        if (FAILED(hr) || !meshData.VertexBuffer)
        {
            Debug::Log("ERROR: Failed to create DEFAULT vertex buffer for static model\n");
            releaseScene();
            return false;
        }
        if (!UploadBufferData(device, meshData.VertexBuffer.Get(), vertices.data(), vertexBufferSize,
                              D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "static fbx vertex"))
        {
            releaseScene();
            return false;
        }

        meshData.VertexBufferView.BufferLocation = meshData.VertexBuffer->GetGPUVirtualAddress();
        meshData.VertexBufferView.StrideInBytes = sizeof(StaticModelVertex);
        meshData.VertexBufferView.SizeInBytes = vertexBufferSize;
        meshData.VertexCount = (UINT)vertices.size();

        const UINT indexBufferSize = sizeof(unsigned int) * (UINT)indices.size();
        auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
        hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &ibDesc, D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&meshData.IndexBuffer));
        if (FAILED(hr) || !meshData.IndexBuffer)
        {
            Debug::Log("ERROR: Failed to create DEFAULT index buffer for static model\n");
            releaseScene();
            return false;
        }
        if (!UploadBufferData(device, meshData.IndexBuffer.Get(), indices.data(), indexBufferSize,
                              D3D12_RESOURCE_STATE_INDEX_BUFFER, "static fbx index"))
        {
            releaseScene();
            return false;
        }

        meshData.IndexBufferView.BufferLocation = meshData.IndexBuffer->GetGPUVirtualAddress();
        meshData.IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
        meshData.IndexBufferView.SizeInBytes = indexBufferSize;
        meshData.IndexCount = (UINT)indices.size();
        meshData.TextureIndex = ResolveStaticMeshTextureIndex(scene, mesh, fileName, dirPath);

        {
            for (int mode = 0; mode < kToonOutlineModeCount; ++mode)
            {
                vector<StaticModelVertex> teoVertices;
                vector<unsigned int> teoIndices;
                BuildTeoMesh(vertices, indices, teoVertices, teoIndices, static_cast<ToonOutlineMeshMode>(mode));
                if (teoVertices.empty() || teoIndices.empty())
                {
                    continue;
                }
                meshData.CpuTeoVerticesByMode[mode] = teoVertices;

                const UINT teoVertexBufferSize = sizeof(StaticModelVertex) * (UINT)teoVertices.size();
                if (!CreateDefaultBufferAndUpload(
                        device, teoVertexBufferSize, teoVertices.data(),
                        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                        "ERROR: Failed to create DEFAULT TEO vertex buffer for static model\n", "static fbx teo vertex",
                        meshData.TeoVertexBuffers[mode]))
                {
                    releaseScene();
                    return false;
                }
                meshData.TeoVertexBufferViews[mode].BufferLocation =
                    meshData.TeoVertexBuffers[mode]->GetGPUVirtualAddress();
                meshData.TeoVertexBufferViews[mode].StrideInBytes = sizeof(StaticModelVertex);
                meshData.TeoVertexBufferViews[mode].SizeInBytes = teoVertexBufferSize;
                meshData.TeoVertexCounts[mode] = (UINT)teoVertices.size();

                const UINT teoIndexBufferSize = sizeof(unsigned int) * (UINT)teoIndices.size();
                if (!CreateDefaultBufferAndUpload(device, teoIndexBufferSize, teoIndices.data(),
                                                  D3D12_RESOURCE_STATE_INDEX_BUFFER,
                                                  "ERROR: Failed to create DEFAULT TEO index buffer for static model\n",
                                                  "static fbx teo index", meshData.TeoIndexBuffers[mode]))
                {
                    releaseScene();
                    return false;
                }
                meshData.TeoIndexBufferViews[mode].BufferLocation =
                    meshData.TeoIndexBuffers[mode]->GetGPUVirtualAddress();
                meshData.TeoIndexBufferViews[mode].Format = DXGI_FORMAT_R32_UINT;
                meshData.TeoIndexBufferViews[mode].SizeInBytes = teoIndexBufferSize;
                meshData.TeoIndexCounts[mode] = (UINT)teoIndices.size();

                if (mode == static_cast<int>(ToonOutlineMeshMode::Balanced))
                {
                    meshData.TeoVertexBuffer = meshData.TeoVertexBuffers[mode];
                    meshData.TeoIndexBuffer = meshData.TeoIndexBuffers[mode];
                    meshData.TeoVertexBufferView = meshData.TeoVertexBufferViews[mode];
                    meshData.TeoIndexBufferView = meshData.TeoIndexBufferViews[mode];
                    meshData.TeoVertexCount = meshData.TeoVertexCounts[mode];
                    meshData.TeoIndexCount = meshData.TeoIndexCounts[mode];
                }
            }
        }

        if (!BuildLodIndexBuffers(device, vertices, indices, meshData))
        {
            releaseScene();
            return false;
        }
        meshData.VirtualGeometrySettings = m_VGSettings;
        MeshletManager::BuildImportedMesh(meshData);
        m_Meshes.push_back(move(meshData));
    }

    if (m_Meshes.empty())
    {
        Debug::Log("ERROR: Static model has no drawable meshes: %s\n", fileName);
        releaseScene();
        return false;
    }

    m_AabbCenter.x = (minPos.x + maxPos.x) * 0.5f;
    m_AabbCenter.y = (minPos.y + maxPos.y) * 0.5f;
    m_AabbCenter.z = (minPos.z + maxPos.z) * 0.5f;
    m_AabbExtents.x = (maxPos.x - minPos.x) * 0.5f;
    m_AabbExtents.y = (maxPos.y - minPos.y) * 0.5f;
    m_AabbExtents.z = (maxPos.z - minPos.z) * 0.5f;

    ModelMapImport::AddMeshes(m_ResourceMap, m_Meshes);
    releaseScene();
    return true;
}

bool StaticModelResource::ApplyMeshShadingOverridePartIds(ID3D12Device* device, const vector<int>& overridePartIds)
{
    if (!device)
    {
        return false;
    }

    bool success = true;
    for (UINT meshIndex = 0; meshIndex < (UINT)m_Meshes.size(); ++meshIndex)
    {
        StaticMeshData& meshData = m_Meshes[meshIndex];
        float targetPartId;
        if ((meshIndex < overridePartIds.size() && overridePartIds[meshIndex] >= 0))
        {
            targetPartId = static_cast<float>(overridePartIds[meshIndex]);
        }
        else
        {
            targetPartId = meshData.MaterialPartId;
        }
        if (fabsf(meshData.AppliedMaterialPartId - targetPartId) < 0.001f)
        {
            continue;
        }

        for (StaticModelVertex& vertex : meshData.CpuVertices)
        {
            vertex.Diffuse.w = targetPartId;
        }
        if (meshData.VertexBuffer && !meshData.CpuVertices.empty())
        {
            const UINT vertexBufferSize = sizeof(StaticModelVertex) * (UINT)meshData.CpuVertices.size();
            success &= UploadBufferData(device, meshData.VertexBuffer.Get(), meshData.CpuVertices.data(),
                                        vertexBufferSize, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "static mesh shading vertex");
        }

        for (int mode = 0; mode < kToonOutlineModeCount; ++mode)
        {
            auto& teoVertices = meshData.CpuTeoVerticesByMode[mode];
            for (StaticModelVertex& vertex : teoVertices)
            {
                vertex.Diffuse.w = targetPartId;
            }
            if (meshData.TeoVertexBuffers[mode] && !teoVertices.empty())
            {
                const UINT teoVertexBufferSize = sizeof(StaticModelVertex) * (UINT)teoVertices.size();
                success &=
                    UploadBufferData(device, meshData.TeoVertexBuffers[mode].Get(), teoVertices.data(),
                                     teoVertexBufferSize, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "static mesh shading teo vertex");
            }
        }

        meshData.AppliedMaterialPartId = targetPartId;
        MeshletManager::BuildImportedMesh(meshData);
        GPUDrivenRenderer::UpdateGeometry(meshData);
    }

    return success;
}

void StaticModelResource::Uninit()
{
    m_ResourceMap.Clear();
    m_Meshes.clear();
    Clear();
}

bool StaticModelResource::PollVirtualGeometry()
{
    bool changed = false;
    for (auto& mesh : m_Meshes)
    {
        changed = MeshletManager::PollImportedMesh(mesh) || changed;
    }
    return changed;
}

void StaticModelResource::RebuildVirtualGeometry(const VGImportSettings& settings)
{
    m_DerivedVGPublished = false;
    m_VGSettings = settings;
    for (auto& mesh : m_Meshes)
    {
        mesh.VirtualGeometrySettings = settings;
        if (mesh.VirtualGeometryRootFallback && mesh.VirtualGeometryLODs[0] && mesh.VirtualGeometryLODs[0]->Pages)
        {
            auto runtime = std::make_shared<VirtualGeometryRuntime::RuntimeAsset>(*mesh.VirtualGeometryLODs[0]);
            runtime->ImportSettings = settings;
            mesh.VirtualGeometryLODs[0] = std::move(runtime);
            continue; // 実行時の方針が変わっても、事前構築したページは固有の階層を保持する。
        }
        MeshletManager::BuildImportedMesh(mesh);
        GPUDrivenRenderer::UpdateGeometry(mesh);
    }
}

void StaticModelResource::RefreshResourceMap(const char* fileName, bool isConvert)
{
    // Derived geometry has its own drawable order. Never reuse original mesh indices.
    int nextMaterial = 0;
    std::unordered_map<std::string, int> fallbackMaterials;
    for (auto& mesh : m_Meshes)
    {
        if (mesh.MaterialIndex >= 0) nextMaterial = std::max(nextMaterial, mesh.MaterialIndex + 1);
    }
    for (auto& mesh : m_Meshes)
    {
        if (mesh.MaterialIndex < 0)
        {
            auto [it, inserted] = fallbackMaterials.emplace(mesh.MaterialName, nextMaterial);
            if (inserted) ++nextMaterial;
            mesh.MaterialIndex = it->second;
        }
    }
    ModelMapImport::AddMeshes(m_ResourceMap, m_Meshes);
    const std::string identity = std::string(m_UsesDerivedVG ? "derived-static|" : "static|") +
                                 (isConvert ? "left-handed|v1" : "raw|v1");
    if (!m_ResourceMap.Persist(ModelPathFromUtf8(fileName), identity))
    {
        Debug::Log("WARNING: Model map cache could not be saved: %s\n", fileName);
    }
}


