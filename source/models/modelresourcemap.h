#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// CPU metadata only. Indices are owned by a model, and remain valid until reload.
// Lookup is average O(1); duplicate names intentionally return multiple indices.
class ModelResourceMap
{
  public:
    using Indices = std::vector<uint32_t>;
    using NameMap = std::unordered_map<std::string, Indices>;
    using CategoryMap = std::unordered_map<std::string, NameMap>;
    static constexpr const char* Bones = "bone";
    static constexpr const char* Meshes = "mesh";
    static constexpr const char* Materials = "material";
    static constexpr const char* MaterialMeshes = "mesh-material";
    static constexpr const char* Humanoid = "humanoid";
    static constexpr const char* Vmd = "vmd";

    void Clear() { m_Categories.clear(); }
    const CategoryMap& Categories() const { return m_Categories; }
    void Add(const std::string& category, const std::string& name, uint32_t index)
    {
        auto& indices = m_Categories[category][name];
        if (std::find(indices.begin(), indices.end(), index) == indices.end())
        {
            indices.push_back(index);
        }
    }
    void Set(const std::string& category, const std::string& name, uint32_t index)
    {
        m_Categories[category][name] = {index};
    }
    const Indices* Find(const std::string& category, const std::string& name) const
    {
        const auto c = m_Categories.find(category);
        if (c == m_Categories.end()) return nullptr;
        const auto n = c->second.find(name);
        return n == c->second.end() ? nullptr : &n->second;
    }

    uint64_t Fingerprint(const std::string& importIdentity) const
    {
        std::string bytes;
        if (!Encode(bytes)) return 0;
        return Hash(importIdentity, Hash(bytes));
    }

    bool Write(std::ostream& stream, uint64_t fingerprint) const
    {
        std::string payload;
        if (!Encode(payload)) return false;
        stream.write("MDLMAP01", 8);
        Put(stream, 1, 4);
        Put(stream, fingerprint, 8);
        Put(stream, payload.size(), 4);
        Put(stream, Hash(payload), 8);
        stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        return bool(stream);
    }

    // Failed reads leave the previous map intact. Never deserialize native STL memory.
    bool Read(std::istream& stream, std::optional<uint64_t> expectedFingerprint = std::nullopt)
    {
        char magic[8]{};
        stream.read(magic, 8);
        uint64_t version = 0, fingerprint = 0, size = 0, checksum = 0;
        if (std::string(magic, 8) != "MDLMAP01" || !Get(stream, version, 4) || version != 1 ||
            !Get(stream, fingerprint, 8) || (expectedFingerprint && fingerprint != *expectedFingerprint) ||
            !Get(stream, size, 4) || size > MaxPayload || !Get(stream, checksum, 8)) return false;
        std::string payload(static_cast<size_t>(size), '\0');
        if (!stream.read(payload.data(), static_cast<std::streamsize>(size)) || Hash(payload) != checksum ||
            stream.peek() != std::char_traits<char>::eof()) return false;
        std::istringstream input(payload, std::ios::binary);
        ModelResourceMap candidate;
        uint64_t count = 0;
        if (!Get(input, count, 4) || count > MaxEntries) return false;
        for (uint64_t entry = 0; entry < count; ++entry)
        {
            std::string category, name;
            uint64_t indices = 0;
            if (!GetString(input, category) || !GetString(input, name) || !Get(input, indices, 4) ||
                indices == 0 || indices > MaxEntries || candidate.Find(category, name)) return false;
            // The payload bound also caps total allocation across all records.
            if (indices > static_cast<uint64_t>(input.rdbuf()->in_avail()) / 4) return false;
            auto& values = candidate.m_Categories[category][name];
            values.reserve(static_cast<size_t>(indices));
            for (uint64_t i = 0; i < indices; ++i)
            {
                uint64_t value = 0;
                if (!Get(input, value, 4)) return false;
                values.push_back(static_cast<uint32_t>(value));
            }
            auto sorted = values;
            std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) return false;
        }
        if (input.peek() != std::char_traits<char>::eof()) return false;
        m_Categories = std::move(candidate.m_Categories);
        return true;
    }

    // Validate the cached data against the actual imported layout, not timestamps alone.
    // This is metadata persistence, not a replacement for model/GPU import.
    bool Persist(const std::filesystem::path& source, const std::string& importIdentity,
                 const std::filesystem::path& directory = "Data/Cache/ModelMaps")
    {
        try
        {
            const auto utf8 = std::filesystem::weakly_canonical(source).generic_u8string();
            const std::string path(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            std::ostringstream name;
            name << std::hex << std::setw(16) << std::setfill('0') << Hash(path + "|" + importIdentity);
            const auto file = directory / (name.str() + ".modelmap");
            const uint64_t fingerprint = Fingerprint(importIdentity);
            ModelResourceMap cached;
            std::ifstream input(file, std::ios::binary);
            if (input && cached.Read(input, fingerprint) && cached.Categories() == Categories()) return true;
            input.close();
            std::filesystem::create_directories(directory);
            auto temporary = file;
            temporary += ".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output || !Write(output, fingerprint)) return false;
                output.flush();
                if (!output) return false;
            }
            std::error_code error;
            std::filesystem::rename(temporary, file, error);
            if (error)
            {
                // Windows rename cannot replace an existing destination.
                std::filesystem::remove(file, error);
                error.clear();
                std::filesystem::rename(temporary, file, error);
            }
            return !error;
        }
        catch (const std::exception&) { return false; }
    }

  private:
    static constexpr uint64_t MaxPayload = 16 * 1024 * 1024;
    static constexpr uint64_t MaxEntries = 1024 * 1024;
    static constexpr uint64_t MaxString = 65536;
    CategoryMap m_Categories;

    static uint64_t Hash(const std::string& bytes, uint64_t hash = 14695981039346656037ull)
    {
        for (unsigned char c : bytes) { hash ^= c; hash *= 1099511628211ull; }
        return hash;
    }
    static void Put(std::ostream& stream, uint64_t value, unsigned bytes)
    {
        for (unsigned i = 0; i < bytes; ++i) stream.put(static_cast<char>((value >> (i * 8)) & 255));
    }
    static bool Get(std::istream& stream, uint64_t& value, unsigned bytes)
    {
        value = 0;
        for (unsigned i = 0; i < bytes; ++i)
        {
            int c = stream.get();
            if (c == std::char_traits<char>::eof()) return false;
            value |= uint64_t(static_cast<unsigned char>(c)) << (i * 8);
        }
        return true;
    }
    static bool GetString(std::istream& stream, std::string& value)
    {
        uint64_t size = 0;
        if (!Get(stream, size, 4) || size > MaxString ||
            size > static_cast<uint64_t>(stream.rdbuf()->in_avail())) return false;
        value.resize(static_cast<size_t>(size));
        return bool(stream.read(value.data(), static_cast<std::streamsize>(size)));
    }
    bool Encode(std::string& payload) const
    {
        std::vector<std::pair<std::string, std::string>> keys;
        for (const auto& category : m_Categories)
            for (const auto& name : category.second) keys.emplace_back(category.first, name.first);
        if (keys.size() > MaxEntries) return false;
        std::sort(keys.begin(), keys.end());
        std::ostringstream stream(std::ios::binary);
        Put(stream, keys.size(), 4);
        for (const auto& key : keys)
        {
            const auto& values = m_Categories.at(key.first).at(key.second);
            if (key.first.size() > MaxString || key.second.size() > MaxString ||
                values.empty() || values.size() > MaxEntries) return false;
            for (const auto* text : {&key.first, &key.second})
            {
                Put(stream, text->size(), 4);
                stream.write(text->data(), static_cast<std::streamsize>(text->size()));
            }
            Put(stream, values.size(), 4);
            for (auto value : values) Put(stream, value, 4);
            if (stream.tellp() > static_cast<std::streamoff>(MaxPayload)) return false;
        }
        payload = stream.str();
        return true;
    }
};
