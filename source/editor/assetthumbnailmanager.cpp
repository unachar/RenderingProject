#include "pch.h"
#include "assetthumbnailmanager.h"
#include "materialmanager.h"
#include "texturemanager.h"
#include "graphicsdevice.h"
#include "frameconstants.h"
#include <chrono>
#include <unordered_set>

namespace
{
    struct Entry
    {
        std::shared_ptr<ThumbnailGpuImage> Image;
        std::filesystem::path Path;
        std::array<AssetHandle, static_cast<size_t>(MaterialTextureSlot::Count)> Dependencies{};
        std::array<uint64_t, static_cast<size_t>(MaterialTextureSlot::Count)> DependencyRevisions{};
        uint64_t SourceRevision = 0;
        uint64_t RuntimeRevision = 0;
        uint64_t ShaderRevision = 0;
        uint64_t LastUsedFrame = 0;
        uint64_t NextCheckFrame = 0;
        bool ChangedSource = false;
        uint64_t CacheKey = 0;
        uint64_t CacheBaseKey = 0;
        bool CacheNeeded = false;
        bool ForceRegenerate = false;
        std::vector<std::pair<std::filesystem::path, uint64_t>> SourceDependencies;
    };

    struct RetiredImage
    {
        uint64_t Fence;
        std::shared_ptr<ThumbnailGpuImage> Image;
    };

    struct CacheWrite
    {
        ThumbnailPrepared Metadata;
        std::shared_ptr<ThumbnailGpuImage> Image;
        std::future<void> Future;
    };

    ThumbnailCache cache;
    ThumbnailSettings settings;
    std::unordered_map<AssetHandle, Entry> entries;
    std::vector<RetiredImage> retired;
    std::vector<CacheWrite> writes;
    std::future<ThumbnailPrepared> preparation;
    std::optional<ThumbnailTicket> active;
    std::optional<ThumbnailPrepared> prepared;
    uint64_t frame = 0;
    UINT previousSize = 128;
    uint64_t cacheHits = 0;

    uint64_t SourceVersion(const std::filesystem::path& path)
    {
        return ThumbnailRenderer::SourceRevision(path) ^ ThumbnailRenderer::SourceRevision(path.wstring() + L".meta");
    }

    uint64_t ShaderVersion()
    {
        return ThumbnailRenderer::SourceRevision("shader/hlsl/build/modelshaderVS.cso") ^
               ThumbnailRenderer::SourceRevision("shader/hlsl/build/modelshaderPS.cso") ^
               ThumbnailRenderer::SourceRevision("shader/hlsl/ProceduralTexture.hlsli");
    }

    void Retire(std::shared_ptr<ThumbnailGpuImage>& image)
    {
        if (image)
        {
            retired.push_back({GraphicsDevice::GetPendingSubmissionFence(), std::move(image)});
        }
    }

    bool EvictOldest()
    {
        auto oldest = entries.end();
        for (auto current = entries.begin(); current != entries.end(); ++current)
        {
            if (!current->second.Image || current->second.LastUsedFrame + 1 >= frame)
            {
                continue;
            }
            if (oldest == entries.end() || current->second.LastUsedFrame < oldest->second.LastUsedFrame)
            {
                oldest = current;
            }
        }
        if (oldest == entries.end())
        {
            return false;
        }
        Retire(oldest->second.Image);
        cache.Remove(oldest->first);
        entries.erase(oldest);
        return true;
    }

    UINT RequiredDescriptors(const ThumbnailPrepared& input)
    {
        UINT count = input.Type == AssetType::Texture && input.Textures[0].Borrowed && !input.Cached ? 0 : 1;
        if (input.Cached || input.Type == AssetType::Texture)
        {
            return count;
        }
        std::unordered_set<std::string> counted;
        const auto countTextures = [&counted](const auto& textures) {
            UINT total = 0;
            for (const auto& source : textures)
            {
                if (AssetRegistry::Classify(source.Path) == AssetType::Noise ||
                    (source.Path.empty() && !source.Decoded && source.Encoded.empty()))
                {
                    continue;
                }
                const auto key = source.IdentityKey();
                if (key.empty() || counted.insert(key).second)
                {
                    ++total;
                }
            }
            return total;
        };
        if (input.Type == AssetType::Material)
        {
            count += countTextures(input.Textures);
        }
        for (const auto& part : input.MeshParts)
        {
            count += countTextures(part.Textures);
        }
        return count;
    }

    uint64_t ReleaseUploads(const std::shared_ptr<ThumbnailGpuImage>& image, uint64_t completed)
    {
        if (!image)
        {
            return 0;
        }
        uint64_t released = 0;
        for (const auto& dependency : image->Dependencies)
        {
            released += ReleaseUploads(dependency, completed);
        }
        if (image->Upload && image->SubmissionFence <= completed)
        {
            const auto description = image->Upload->GetDesc();
            released += GraphicsDevice::GetDevice()->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
            image->Upload.Reset();
        }
        image->Bytes -= std::min(image->Bytes, released);
        return released;
    }

    uint64_t RequiredMemory(const ThumbnailPrepared& input)
    {
        std::unordered_set<std::string> counted;
        const auto imageBytes = [&counted](const ThumbnailSourceImage& source) {
            if (source.Borrowed || !source.Decoded)
            {
                return uint64_t{0};
            }
            const auto key = source.IdentityKey();
            if (!key.empty() && !counted.insert(key).second)
            {
                return uint64_t{0};
            }
            const auto* pixels = source.Decoded->GetImage(0, 0, 0);
            const uint64_t rowBytes = (pixels->rowPitch + 255) & ~uint64_t{255};
            const uint64_t uploadBytes = (rowBytes * pixels->height + 65535) & ~uint64_t{65535};
            // この小さな通常の 2D リソースは単一 mip なので、64 KiB のアラインメントで足りる。
            return 2 * uploadBytes;
        };
        if (input.Type == AssetType::Texture || input.Cached)
        {
            return imageBytes(input.Textures[0]);
        }
        const auto allocationBytes = [](const D3D12_RESOURCE_DESC& description) {
            return GraphicsDevice::GetDevice()->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
        };
        const auto target = CD3DX12_RESOURCE_DESC::Tex2D(RendererState::GetSceneColorFormat(), input.Size, input.Size,
                                                         1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const auto depth = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, input.Size, input.Size, 1, 1, 1, 0,
                                                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        uint64_t bytes = allocationBytes(target) + allocationBytes(depth);
        bytes += allocationBytes(CD3DX12_RESOURCE_DESC::Buffer(32768ull * RendererState::g_kFRAME_COUNT));
        const auto parts = std::max<size_t>(1, input.MeshParts.size());
        bytes += allocationBytes(CD3DX12_RESOURCE_DESC::Buffer(parts * RendererState::g_kPBR_CB_ALIGNED_SIZE *
                                                               RendererState::g_kFRAME_COUNT));
        // コンテキストは最初に球を作り、描画前にメッシュのアップロードで置き換える。
        const auto sphereBytes = 16ull * 32 * 6 * sizeof(StaticModelVertex);
        bytes += allocationBytes(CD3DX12_RESOURCE_DESC::Buffer(
            std::max<uint64_t>(sphereBytes, input.Geometry.size() * sizeof(StaticModelVertex))));
        for (const auto& source : input.Textures)
        {
            bytes += imageBytes(source);
        }
        for (const auto& part : input.MeshParts)
        {
            for (const auto& source : part.Textures)
            {
                bytes += imageBytes(source);
            }
        }
        return bytes;
    }

    ThumbnailPrepared MakeInput(AssetHandle handle)
    {
        auto& registry = MaterialManager::Registry();
        auto& entry = entries[handle];
        ThumbnailPrepared input;
        input.Handle = handle;
        input.Type = registry.TypeOf(handle);
        input.Size = settings.Size;
        input.SourcePath = registry.Resolve(handle);
        input.AllowBorrow = !entry.ChangedSource;
        input.UseDiskCache = settings.DiskCache;
        input.BypassDiskCache = entry.ForceRegenerate;
        if (input.SourcePath.empty())
        {
            input.Error = "Missing asset";
            return input;
        }
        entry.Path = input.SourcePath;
        const auto sourceRevision = SourceVersion(entry.Path);
        entry.SourceRevision = sourceRevision;
        entry.ShaderRevision = ShaderVersion();
        input.Key =
            input.Type == AssetType::Texture ? entry.SourceRevision : entry.SourceRevision ^ entry.ShaderRevision;
        if (input.Type == AssetType::Material)
        {
            const auto* runtime = MaterialManager::FindRuntime(handle);
            if (runtime)
            {
                entry.RuntimeRevision = runtime->Revision;
            }
            if (runtime && runtime->Dirty)
            {
                input.Material = runtime->Editing;
                input.UseDiskCache = input.UseDiskCache && !runtime->Dirty;
            }
            else if (!MaterialSerializer::Load(entry.Path, input.Material, input.Error))
            {
                return input;
            }
            entry.Dependencies = input.Material.Textures;
            for (size_t slot = 0; slot < input.Textures.size(); ++slot)
            {
                auto& source = input.Textures[slot];
                source.Path = registry.Resolve(input.Material.Textures[slot]);
                if (input.Material.Textures[slot] && source.Path.empty())
                {
                    input.Error = "Missing material texture";
                }
                entry.DependencyRevisions[slot] = SourceVersion(source.Path);
                const auto sourceEntry = entries.find(input.Material.Textures[slot]);
                const bool changedTexture = sourceEntry != entries.end() && sourceEntry->second.ChangedSource;
                if (!changedTexture && !source.Path.empty())
                {
                    source.BorrowedDescriptor = TextureManager::FindLoadedTexture(source.Path);
                }
                if (source.BorrowedDescriptor >= 0)
                {
                    source.Borrowed = TextureManager::GetTextureResource(source.BorrowedDescriptor);
                }
            }
        }
        else if (input.Type == AssetType::Texture)
        {
            input.Textures[0].Path = entry.Path;
            if (input.AllowBorrow)
            {
                input.Textures[0].BorrowedDescriptor = TextureManager::FindLoadedTexture(entry.Path);
            }
            if (input.Textures[0].BorrowedDescriptor >= 0)
            {
                input.Textures[0].Borrowed = TextureManager::GetTextureResource(input.Textures[0].BorrowedDescriptor);
            }
        }
        return input;
    }

    void CheckEntry(AssetHandle handle, Entry& entry)
    {
        if (entry.NextCheckFrame > frame || entry.Path.empty())
        {
            return;
        }
        entry.NextCheckFrame = frame + 30;
        const auto path = MaterialManager::Registry().Resolve(handle);
        const auto source = SourceVersion(path);
        bool changed = source != entry.SourceRevision;
        if (source == 0)
        {
            AssetThumbnailManager::Remove(handle);
            return;
        }
        const auto type = MaterialManager::Registry().TypeOf(handle);
        if (type == AssetType::Mesh && path.parent_path() != entry.Path.parent_path())
        {
            changed = true;
        }
        entry.Path = path;
        const auto* runtime = MaterialManager::FindRuntime(handle);
        changed = changed || (runtime && runtime->Revision != entry.RuntimeRevision);
        changed = changed || (type != AssetType::Texture && ShaderVersion() != entry.ShaderRevision);
        for (size_t slot = 0; slot < entry.Dependencies.size(); ++slot)
        {
            if (!entry.Dependencies[slot])
            {
                continue;
            }
            const auto version = SourceVersion(MaterialManager::Registry().Resolve(entry.Dependencies[slot]));
            if (version != entry.DependencyRevisions[slot])
            {
                changed = true;
            }
        }
        for (const auto& dependency : entry.SourceDependencies)
        {
            if (SourceVersion(dependency.first) != dependency.second)
            {
                changed = true;
            }
        }
        if (changed)
        {
            entry.ChangedSource = true;
            AssetThumbnailManager::MarkDirty(handle);
        }
    }
}

ThumbnailSettings& AssetThumbnailManager::Settings()
{
    return settings;
}

uint64_t AssetThumbnailManager::CacheHits()
{
    return cacheHits;
}

uint64_t AssetThumbnailManager::LastCacheKey(AssetHandle guid)
{
    const auto found = entries.find(guid);
    return found == entries.end() ? 0 : found->second.CacheKey;
}

ThumbnailHandle AssetThumbnailManager::Request(AssetHandle guid, ThumbnailPriority priority)
{
    if (!guid)
    {
        return {};
    }
    auto& entry = entries[guid];
    entry.LastUsedFrame = frame;
    const auto& record = cache.Request(guid, priority);
    if (record.State == ThumbnailState::Ready && entry.Image && !entry.Image->Descriptor)
    {
        const auto offset = entry.Image->BorrowedDescriptor.ptr -
                            FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart().ptr;
        TextureManager::TouchTexture(static_cast<int>(offset / FrameConstants::GetCbvIncrementSize()));
    }
    if (record.State == ThumbnailState::Ready && entry.Image)
    {
        const auto heapStart = FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart().ptr;
        for (const auto& dependency : entry.Image->Dependencies)
        {
            if (!dependency->BorrowedDescriptor.ptr)
            {
                continue;
            }
            const auto index = (dependency->BorrowedDescriptor.ptr - heapStart) / FrameConstants::GetCbvIncrementSize();
            TextureManager::TouchTexture(static_cast<int>(index));
        }
    }
    return {guid, record.State, record.Revision, record.State == ThumbnailState::Ready ? entry.Image : nullptr};
}

void AssetThumbnailManager::MarkDirty(AssetHandle guid)
{
    cache.MarkDirty(guid);
    if (const auto found = entries.find(guid); found != entries.end())
    {
        found->second.ForceRegenerate = true;
        if (MaterialManager::Registry().TypeOf(guid) == AssetType::Texture)
        {
            found->second.ChangedSource = true;
        }
    }
    // 逆依存関係は要求されたマテリアルだけを対象とし、プロジェクト全体は走査しない。
    for (auto& [handle, entry] : entries)
    {
        if (std::find(entry.Dependencies.begin(), entry.Dependencies.end(), guid) == entry.Dependencies.end())
        {
            continue;
        }
        entry.ForceRegenerate = true;
        cache.MarkDirty(handle);
    }
}

void AssetThumbnailManager::Remove(AssetHandle guid)
{
    const auto found = entries.find(guid);
    if (found != entries.end())
    {
        Retire(found->second.Image);
        entries.erase(found);
    }
    cache.Remove(guid);
    MarkDirty(guid);
}

uint64_t AssetThumbnailManager::MemoryBytes()
{
    std::unordered_set<const ThumbnailGpuImage*> counted;
    uint64_t bytes = ThumbnailRenderer::InFlightBytes();
    auto add = [&](const auto& image) {
        if (image && counted.insert(image.get()).second)
        {
            bytes += image->Bytes;
        }
    };
    for (const auto& [handle, entry] : entries)
    {
        add(entry.Image);
    }
    for (const auto& item : retired)
    {
        add(item.Image);
    }
    for (const auto& item : writes)
    {
        add(item.Image);
    }
    return bytes;
}

void AssetThumbnailManager::BeginFrame(uint64_t frameIndex)
{
    frame = frameIndex;
    cache.BeginFrame(frame);
    EditorDescriptors::Collect();
    ThumbnailRenderer::Collect();
    const auto completed = GraphicsDevice::GetCompletedFence();
    std::erase_if(retired, [completed](const auto& item) {
        return item.Fence <= completed;
    });
    for (auto current = writes.begin(); current != writes.end();)
    {
        if (current->Future.valid())
        {
            if (current->Future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            {
                ++current;
                continue;
            }
            try
            {
                current->Future.get();
            }
            catch (const std::exception& error)
            {
                Debug::Log("Thumbnail cache: %s\n", error.what());
            }
            if (current->Image->Readback)
            {
                const auto description = current->Image->Readback->GetDesc();
                const auto bytes =
                    GraphicsDevice::GetDevice()->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
                current->Image->Bytes -= std::min(current->Image->Bytes, bytes);
                current->Image->Readback.Reset();
            }
            current = writes.erase(current);
        }
        else if (current->Image->SubmissionFence <= completed)
        {
            current->Future = ThumbnailRenderer::SaveCache(current->Metadata, current->Image);
            ++current;
        }
        else
        {
            ++current;
        }
    }
    for (const auto& [handle, entry] : entries)
    {
        ReleaseUploads(entry.Image, completed);
    }
    for (auto& [handle, entry] : entries)
    {
        if (writes.size() >= 2)
        {
            break;
        }
        if (!entry.CacheNeeded || !entry.Image)
        {
            continue;
        }
        if (std::any_of(writes.begin(), writes.end(), [handle](const auto& write) {
                return write.Metadata.Handle == handle;
            }))
        {
            continue;
        }
        if (MemoryBytes() + entry.Image->Width * entry.Image->Height * 16ull + 65536 > settings.MemoryBudget)
        {
            continue;
        }
        if (!ThumbnailRenderer::BeginReadback(entry.Image))
        {
            continue;
        }
        ThumbnailPrepared metadata;
        metadata.Handle = handle;
        metadata.Key = entry.CacheKey;
        metadata.BaseKey = entry.CacheBaseKey;
        metadata.Type = MaterialManager::Registry().TypeOf(handle);
        metadata.SourceDependencies = entry.SourceDependencies;
        metadata.SourcePath = entry.Path;
        writes.push_back({std::move(metadata), entry.Image, {}});
        entry.CacheNeeded = false;
    }
    settings.Size = std::clamp(settings.Size, 32u, 256u);
    if (previousSize != settings.Size)
    {
        Refresh();
        previousSize = settings.Size;
    }
    // 前フレームで参照された項目は、クリップされた UI 描画も含めて LRU から除外する。
    while (MemoryBytes() > settings.MemoryBudget)
    {
        if (!EvictOldest())
        {
            break;
        }
    }
}

void AssetThumbnailManager::ProcessGenerationBudget()
{
    if (!settings.MaxGenerationsPerFrame)
    {
        return;
    }
    // 最近要求された項目だけを確認し、ファイルシステムの確認は 1 フレーム最大 8 件に制限する。
    std::vector<AssetHandle> checks;
    for (const auto& [handle, entry] : entries)
    {
        if (checks.size() == 8)
        {
            break;
        }
        if (entry.LastUsedFrame + 1 >= frame && entry.NextCheckFrame <= frame)
        {
            checks.push_back(handle);
        }
    }
    for (auto handle : checks)
    {
        if (entries.contains(handle))
        {
            CheckEntry(handle, entries.at(handle));
        }
    }
    for (UINT generated = 0; generated < settings.MaxGenerationsPerFrame; ++generated)
    {
        if (active)
        {
            if (!prepared)
            {
                if (preparation.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                {
                    return;
                }
                try
                {
                    prepared = preparation.get();
                }
                catch (const std::exception& error)
                {
                    prepared.emplace();
                    prepared->Error = error.what();
                }
            }
            const auto ticket = *active;
            const auto* record = cache.Find(ticket.Handle);
            if (!record || record->Revision != ticket.Revision || record->State != ThumbnailState::Generating)
            {
                active.reset();
                prepared.reset();
                continue;
            }
            const auto required = RequiredDescriptors(*prepared);
            if (required > RendererState::g_kTHUMBNAIL_DESCRIPTOR_COUNT)
            {
                prepared->Error = "Mesh uses too many thumbnail texture descriptors";
            }
            else if (prepared->Error.empty() && EditorDescriptors::Available() < required)
            {
                if (EvictOldest() || !retired.empty() || ThumbnailRenderer::InFlightBytes())
                {
                    return;
                }
                cache.Defer(ticket, 30);
                active.reset();
                prepared.reset();
                continue;
            }
            const auto requiredMemory = RequiredMemory(*prepared);
            if (requiredMemory > settings.MemoryBudget)
            {
                prepared->Error = "Asset preview exceeds thumbnail memory budget";
            }
            else if (prepared->Error.empty() && MemoryBytes() + requiredMemory > settings.MemoryBudget)
            {
                if (EvictOldest() || !retired.empty() || ThumbnailRenderer::InFlightBytes())
                {
                    return;
                }
                cache.Defer(ticket, 30);
                active.reset();
                prepared.reset();
                continue;
            }
            ThumbnailPrepared input = std::move(*prepared);
            active.reset();
            prepared.reset();
            const auto resolved = MaterialManager::Registry().Resolve(ticket.Handle);
            if (resolved.empty())
            {
                Remove(ticket.Handle);
                continue;
            }
            if (!input.Error.empty() && !input.SourcePath.empty() && resolved != input.SourcePath)
            {
                cache.MarkDirty(ticket.Handle);
                continue;
            }
            std::shared_ptr<ThumbnailGpuImage> image;
            try
            {
                image = ThumbnailRenderer::Generate(input);
            }
            catch (const std::exception& error)
            {
                input.Error = error.what();
            }
            if (cache.Complete(ticket, image != nullptr))
            {
                auto& entry = entries[ticket.Handle];
                Retire(entry.Image);
                entry.Image = image;
                entry.CacheKey = input.Key;
                entry.CacheBaseKey = input.BaseKey;
                entry.ForceRegenerate = false;
                entry.SourceDependencies = std::move(input.SourceDependencies);
                entry.CacheNeeded =
                    image && settings.DiskCache && input.UseDiskCache && !input.Cached && image->Descriptor;
                if (input.Cached && image)
                {
                    ++cacheHits;
                }
                if (!image)
                {
                    Debug::Log("Thumbnail %s failed: %s\n", FormatAssetHandle(ticket.Handle).c_str(),
                               input.Error.empty() ? "GPU resource or descriptor allocation/render failed"
                                                   : input.Error.c_str());
                }
            }
        }
        if (MemoryBytes() >= settings.MemoryBudget)
        {
            EvictOldest();
            return;
        }
        const auto next = cache.TakeNext();
        if (!next)
        {
            return;
        }
        active = next;
        auto input = MakeInput(next->Handle);
        preparation = std::async(std::launch::async, [input = std::move(input)]() mutable {
            return ThumbnailRenderer::Prepare(std::move(input));
        });
    }
}

void AssetThumbnailManager::Refresh()
{
    for (auto& [handle, entry] : entries)
    {
        entry.ForceRegenerate = true;
        cache.MarkDirty(handle);
    }
}

void AssetThumbnailManager::Shutdown()
{
    if (preparation.valid())
    {
        try
        {
            preparation.get();
        }
        catch (...)
        {
        }
    }
    for (auto& write : writes)
    {
        if (write.Future.valid())
        {
            try
            {
                write.Future.get();
            }
            catch (...)
            {
            }
        }
    }
    writes.clear();
    entries.clear();
    retired.clear();
    active.reset();
    prepared.reset();
    cache = ThumbnailCache{};
    ThumbnailRenderer::Shutdown();
    EditorDescriptors::Shutdown();
}
