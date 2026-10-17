#include "assetregistry.h"
#include <algorithm>
#include <fstream>
#include <random>
#include <cctype>

AssetHandle GenerateAssetHandle()
{
    static std::mt19937_64 generator(std::random_device{}());
    AssetHandle handle = 0;
    while (handle == 0)
    {
        handle = generator();
    }
    return handle;
}

AssetRegistry::AssetRegistry(const std::filesystem::path& root)
    : m_RootSpec(root)
{
}

const std::filesystem::path& AssetRegistry::Root() const
{
    std::error_code error;
    const auto base = std::filesystem::current_path(error);
    if (m_Root.empty() || base != m_RootBase)
    {
        m_RootBase = base;
        const auto absolute = m_RootSpec.is_absolute() ? m_RootSpec : base / m_RootSpec;
        std::error_code canonicalError;
        m_Root = std::filesystem::weakly_canonical(absolute, canonicalError);
        if (canonicalError || m_Root.empty())
        {
            m_Root = absolute.lexically_normal();
        }
    }
    return m_Root;
}

bool AssetRegistry::Contains(const std::filesystem::path& path) const
{
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error)
    {
        return false;
    }
    const auto relative = canonical.lexically_relative(Root());
    return !relative.empty() && *relative.begin() != ".." && *relative.begin() != ".";
}

AssetType AssetRegistry::Classify(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char letter)
    {
        return static_cast<char>(std::tolower(letter));
    });
    if (extension == ".material")
    {
        return AssetType::Material;
    }
    for (const char* texture : {".png", ".jpg", ".jpeg", ".dds", ".tga", ".bmp", ".tif", ".tiff", ".hdr"})
    {
        if (extension == texture)
        {
            return AssetType::Texture;
        }
    }
    for (const char* mesh : {".obj", ".fbx", ".gltf", ".glb", ".vgmesh", ".vgp2"})
    {
        if (extension == mesh) return AssetType::Mesh;
    }
    return AssetType::Unknown;
}

AssetHandle AssetRegistry::Register(const std::filesystem::path& path, AssetType type, std::string& error)
{
    error.clear();
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    std::error_code filesystemError;
    if (!Contains(absolute) || !std::filesystem::is_regular_file(absolute, filesystemError) ||
        type == AssetType::Unknown || Classify(absolute) != type)
    {
        error = "Asset must be a supported file inside the project asset directory";
        return 0;
    }
    const auto metadata = std::filesystem::path(absolute.wstring() + L".meta");
    AssetHandle handle = 0;
    if (std::filesystem::exists(metadata, filesystemError))
    {
        std::string reference;
        std::ifstream stream(metadata);
        stream >> reference;
        handle = ParseAssetHandle(reference);
        if (handle == 0)
        {
            error = "Invalid asset metadata: " + metadata.generic_string();
            return 0;
        }
    }
    else
    {
        do
        {
            handle = GenerateAssetHandle();
        } while (handle == 0 || m_Entries.contains(handle) || m_Collisions.contains(handle));
        if (!WriteAssetFile(metadata, FormatAssetHandle(handle) + "\n", error))
        {
            return 0;
        }
    }
    const auto existing = m_Entries.find(handle);
    if (m_Collisions.contains(handle) || (existing != m_Entries.end() && existing->second.Path != absolute))
    {
        m_Entries.erase(handle);
        m_Collisions.insert(handle);
        error = "Duplicate asset GUID: " + FormatAssetHandle(handle);
        return 0;
    }
    m_Entries[handle] = {absolute, type};
    return handle;
}

std::filesystem::path AssetRegistry::Resolve(AssetHandle handle) const
{
    const auto found = m_Entries.find(handle);
    std::error_code error;
    if (found == m_Entries.end() || !std::filesystem::is_regular_file(found->second.Path, error))
    {
        return {};
    }
    return found->second.Path;
}

AssetType AssetRegistry::TypeOf(AssetHandle handle) const
{
    const auto found = m_Entries.find(handle);
    if (found == m_Entries.end())
    {
        return AssetType::Unknown;
    }
    return found->second.Type;
}

bool AssetRegistry::Refresh(std::string& error)
{
    m_Entries.clear();
    m_Collisions.clear();
    error.clear();
    std::error_code filesystemError;
    std::filesystem::recursive_directory_iterator iterator(Root(), filesystemError);
    for (const auto end = std::filesystem::recursive_directory_iterator(); iterator != end && !filesystemError; iterator.increment(filesystemError))
    {
        const auto type = Classify(iterator->path());
        if (type == AssetType::Unknown || !iterator->is_regular_file(filesystemError))
        {
            continue;
        }
        // Texture IDs are assigned when used or dragged. Scanning the project should
        // not create metadata for every imported texture that no material references.
        if ((type == AssetType::Texture || type == AssetType::Mesh) &&
            !std::filesystem::exists(iterator->path().wstring() + L".meta", filesystemError))
        {
            continue;
        }
        std::string assetError;
        if (!Register(iterator->path(), type, assetError))
        {
            error += assetError + "\n";
        }
    }
    if (filesystemError)
    {
        error += filesystemError.message();
    }
    return error.empty();
}

bool AssetRegistry::Move(AssetHandle handle, const std::filesystem::path& destination, std::string& error)
{
    error.clear();
    const auto source = Resolve(handle);
    const auto target = std::filesystem::absolute(destination).lexically_normal();
    std::error_code filesystemError;
    if (source.empty() || !Contains(target) || Classify(target) != TypeOf(handle) ||
        std::filesystem::exists(target, filesystemError) || std::filesystem::exists(target.wstring() + L".meta", filesystemError))
    {
        error = "Invalid or occupied asset destination";
        return false;
    }
    std::filesystem::create_directories(target.parent_path(), filesystemError);
    if (!filesystemError)
    {
        std::filesystem::rename(source, target, filesystemError);
    }
    if (filesystemError)
    {
        error = filesystemError.message();
        return false;
    }
    std::filesystem::rename(source.wstring() + L".meta", target.wstring() + L".meta", filesystemError);
    if (filesystemError)
    {
        error = filesystemError.message();
        std::error_code rollbackError;
        std::filesystem::rename(target, source, rollbackError);
        return false;
    }
    m_Entries.at(handle).Path = target;
    return true;
}

AssetHandle AssetRegistry::Duplicate(AssetHandle handle, const std::filesystem::path& destination, std::string& error)
{
    error.clear();
    const auto source = Resolve(handle);
    const auto target = std::filesystem::absolute(destination).lexically_normal();
    const auto type = TypeOf(handle);
    std::error_code filesystemError;
    if (source.empty() || !Contains(target) || Classify(target) != type ||
        std::filesystem::exists(target.wstring() + L".meta", filesystemError))
    {
        error = "Invalid duplicate destination";
        return 0;
    }
    if (!std::filesystem::copy_file(source, target, std::filesystem::copy_options::none, filesystemError))
    {
        error = filesystemError.message();
        return 0;
    }
    const auto copy = Register(target, type, error);
    if (!copy)
    {
        std::filesystem::remove(target, filesystemError);
    }
    return copy;
}


