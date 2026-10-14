#include "assetregistry.h"
#include <algorithm>
#include <fstream>
#include <random>
#include <cctype>

AssetHandle GenerateAssetHandle()
{
    static mt19937_64 generator(random_device{}());
    AssetHandle handle = 0;
    while (handle == 0)
    {
        handle = generator();
    }
    return handle;
}

AssetRegistry::AssetRegistry(const filesystem::path& root)
    : m_Root(filesystem::weakly_canonical(filesystem::absolute(root)))
{
}

bool AssetRegistry::Contains(const filesystem::path& path) const
{
    error_code error;
    const auto canonical = filesystem::weakly_canonical(path, error);
    if (error)
    {
        return false;
    }
    const auto relative = canonical.lexically_relative(m_Root);
    return !relative.empty() && *relative.begin() != "..";
}

AssetType AssetRegistry::Classify(const filesystem::path& path)
{
    string extension = path.extension().string();
    transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char letter)
    {
        return static_cast<char>(tolower(letter));
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
    return AssetType::Unknown;
}

AssetHandle AssetRegistry::Register(const filesystem::path& path, AssetType type, string& error)
{
    error.clear();
    const auto absolute = filesystem::absolute(path).lexically_normal();
    error_code filesystemError;
    if (!Contains(absolute) || !filesystem::is_regular_file(absolute, filesystemError) ||
        type == AssetType::Unknown || Classify(absolute) != type)
    {
        error = "Asset must be a supported file inside the project asset directory";
        return 0;
    }
    const auto metadata = filesystem::path(absolute.wstring() + L".meta");
    AssetHandle handle = 0;
    if (filesystem::exists(metadata, filesystemError))
    {
        string reference;
        ifstream stream(metadata);
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

filesystem::path AssetRegistry::Resolve(AssetHandle handle) const
{
    const auto found = m_Entries.find(handle);
    error_code error;
    if (found == m_Entries.end() || !filesystem::is_regular_file(found->second.Path, error))
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

bool AssetRegistry::Refresh(string& error)
{
    m_Entries.clear();
    m_Collisions.clear();
    error.clear();
    error_code filesystemError;
    filesystem::recursive_directory_iterator iterator(m_Root, filesystemError);
    for (const auto end = filesystem::recursive_directory_iterator(); iterator != end && !filesystemError; iterator.increment(filesystemError))
    {
        const auto type = Classify(iterator->path());
        if (type == AssetType::Unknown || !iterator->is_regular_file(filesystemError))
        {
            continue;
        }
        // Texture IDs are assigned when used or dragged. Scanning the project should
        // not create metadata for every imported texture that no material references.
        if (type == AssetType::Texture &&
            !filesystem::exists(iterator->path().wstring() + L".meta", filesystemError))
        {
            continue;
        }
        string assetError;
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

bool AssetRegistry::Move(AssetHandle handle, const filesystem::path& destination, string& error)
{
    error.clear();
    const auto source = Resolve(handle);
    const auto target = filesystem::absolute(destination).lexically_normal();
    error_code filesystemError;
    if (source.empty() || !Contains(target) || Classify(target) != TypeOf(handle) ||
        filesystem::exists(target, filesystemError) || filesystem::exists(target.wstring() + L".meta", filesystemError))
    {
        error = "Invalid or occupied asset destination";
        return false;
    }
    filesystem::create_directories(target.parent_path(), filesystemError);
    if (!filesystemError)
    {
        filesystem::rename(source, target, filesystemError);
    }
    if (filesystemError)
    {
        error = filesystemError.message();
        return false;
    }
    filesystem::rename(source.wstring() + L".meta", target.wstring() + L".meta", filesystemError);
    if (filesystemError)
    {
        error = filesystemError.message();
        error_code rollbackError;
        filesystem::rename(target, source, rollbackError);
        return false;
    }
    m_Entries.at(handle).Path = target;
    return true;
}

AssetHandle AssetRegistry::Duplicate(AssetHandle handle, const filesystem::path& destination, string& error)
{
    error.clear();
    const auto source = Resolve(handle);
    const auto target = filesystem::absolute(destination).lexically_normal();
    const auto type = TypeOf(handle);
    error_code filesystemError;
    if (source.empty() || !Contains(target) || Classify(target) != type ||
        filesystem::exists(target.wstring() + L".meta", filesystemError))
    {
        error = "Invalid duplicate destination";
        return 0;
    }
    if (!filesystem::copy_file(source, target, filesystem::copy_options::none, filesystemError))
    {
        error = filesystemError.message();
        return 0;
    }
    const auto copy = Register(target, type, error);
    if (!copy)
    {
        filesystem::remove(target, filesystemError);
    }
    return copy;
}
