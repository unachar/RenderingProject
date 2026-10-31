#include "pch.h"
#include "aclcompression.h"

#include <algorithm>
#include <cmath>

namespace ACLCompression
{

// ============================================================================
// AclCompressor Implementation (Stub - to be replaced with actual ACL integration)
// ============================================================================

AclCompressor::AclCompressor()
{
}

AclCompressor::~AclCompressor()
{
}

bool AclCompressor::Compress(const VmdAnimation& animation, const CompressionSettings& settings, CompressedAnimation& outCompressed)
{
    // TODO: Implement actual ACL compression when ACL library is integrated
    // This is a placeholder implementation that copies the raw data
    
    m_lastError.clear();
    
    outCompressed.name = animation.ModelName;
    outCompressed.maxFrame = animation.MaxFrame;
    outCompressed.duration = animation.MaxFrame / 30.0f; // Assuming 30 FPS
    outCompressed.boneCount = static_cast<uint32_t>(animation.BoneTracks.size());
    outCompressed.morphCount = static_cast<uint32_t>(animation.MorphTracks.size());
    
    // Extract bone names
    outCompressed.boneNames.reserve(animation.BoneTracks.size());
    for (const auto& pair : animation.BoneTracks)
    {
        outCompressed.boneNames.push_back(pair.first);
    }
    
    // Extract morph names
    outCompressed.morphNames.reserve(animation.MorphTracks.size());
    for (const auto& pair : animation.MorphTracks)
    {
        outCompressed.morphNames.push_back(pair.first);
    }
    
    // Calculate original size estimate
    size_t originalSize = 0;
    for (const auto& pair : animation.BoneTracks)
    {
        originalSize += pair.second.size() * sizeof(VmdKeyframe);
    }
    for (const auto& pair : animation.MorphTracks)
    {
        originalSize += pair.second.size() * sizeof(VmdScalarKeyframe);
    }
    outCompressed.originalSize = originalSize;
    
    // Placeholder: just serialize the raw data for now
    // In real implementation, this would be ACL compressed data
    std::vector<uint8_t> rawData;
    
    // Serialize header
    uint32_t version = 1;
    rawData.insert(rawData.end(), 
        reinterpret_cast<const uint8_t*>(&version), 
        reinterpret_cast<const uint8_t*>(&version) + sizeof(version));
    
    // Serialize bone tracks
    uint32_t boneCount = static_cast<uint32_t>(animation.BoneTracks.size());
    rawData.insert(rawData.end(), 
        reinterpret_cast<const uint8_t*>(&boneCount), 
        reinterpret_cast<const uint8_t*>(&boneCount) + sizeof(boneCount));
    
    for (const auto& pair : animation.BoneTracks)
    {
        // Bone name
        const std::string& name = pair.first;
        uint32_t nameLen = static_cast<uint32_t>(name.size());
        rawData.insert(rawData.end(), 
            reinterpret_cast<const uint8_t*>(&nameLen), 
            reinterpret_cast<const uint8_t*>(&nameLen) + sizeof(nameLen));
        rawData.insert(rawData.end(), name.begin(), name.end());
        
        // Keyframe count
        uint32_t keyCount = static_cast<uint32_t>(pair.second.size());
        rawData.insert(rawData.end(), 
            reinterpret_cast<const uint8_t*>(&keyCount), 
            reinterpret_cast<const uint8_t*>(&keyCount) + sizeof(keyCount));
        
        // Keyframes
        for (const auto& kf : pair.second)
        {
            rawData.insert(rawData.end(), 
                reinterpret_cast<const uint8_t*>(&kf), 
                reinterpret_cast<const uint8_t*>(&kf) + sizeof(VmdKeyframe));
        }
    }
    
    // Serialize morph tracks
    uint32_t morphCount = static_cast<uint32_t>(animation.MorphTracks.size());
    rawData.insert(rawData.end(), 
        reinterpret_cast<const uint8_t*>(&morphCount), 
        reinterpret_cast<const uint8_t*>(&morphCount) + sizeof(morphCount));
    
    for (const auto& pair : animation.MorphTracks)
    {
        const std::string& name = pair.first;
        uint32_t nameLen = static_cast<uint32_t>(name.size());
        rawData.insert(rawData.end(), 
            reinterpret_cast<const uint8_t*>(&nameLen), 
            reinterpret_cast<const uint8_t*>(&nameLen) + sizeof(nameLen));
        rawData.insert(rawData.end(), name.begin(), name.end());
        
        uint32_t keyCount = static_cast<uint32_t>(pair.second.size());
        rawData.insert(rawData.end(), 
            reinterpret_cast<const uint8_t*>(&keyCount), 
            reinterpret_cast<const uint8_t*>(&keyCount) + sizeof(keyCount));
        
        for (const auto& kf : pair.second)
        {
            rawData.insert(rawData.end(), 
                reinterpret_cast<const uint8_t*>(&kf), 
                reinterpret_cast<const uint8_t*>(&kf) + sizeof(VmdScalarKeyframe));
        }
    }
    
    outCompressed.compressedData = std::move(rawData);
    outCompressed.compressedSize = outCompressed.compressedData.size();
    outCompressed.compressionRatio = outCompressed.originalSize > 0 ? 
        static_cast<float>(outCompressed.compressedSize) / static_cast<float>(outCompressed.originalSize) : 0.0f;
    
    // Update statistics
    m_stats.totalOriginalSize += outCompressed.originalSize;
    m_stats.totalCompressedSize += outCompressed.compressedSize;
    if (m_stats.totalOriginalSize > 0)
    {
        m_stats.averageCompressionRatio = 
            static_cast<float>(m_stats.totalCompressedSize) / static_cast<float>(m_stats.totalOriginalSize);
    }
    
    return true;
}

bool AclCompressor::CompressBatch(const std::vector<std::pair<std::string, const VmdAnimation*>>& animations, 
                                 const CompressionSettings& settings, 
                                 std::vector<CompressedAnimation>& outCompressed)
{
    outCompressed.clear();
    outCompressed.reserve(animations.size());
    
    for (const auto& pair : animations)
    {
        if (!pair.second)
        {
            m_lastError = "Null animation pointer in batch: " + pair.first;
            return false;
        }
        
        CompressedAnimation compressed;
        if (!Compress(*pair.second, settings, compressed))
        {
            m_lastError = "Failed to compress animation: " + pair.first;
            return false;
        }
        compressed.name = pair.first;
        outCompressed.push_back(std::move(compressed));
    }
    
    return true;
}

const std::string& AclCompressor::GetLastError() const
{
    return m_lastError;
}

AclCompressor::Statistics AclCompressor::GetStatistics() const
{
    return m_stats;
}

// ============================================================================
// AclDecompressor Implementation (Stub)
// ============================================================================

struct AclDecompressor::Impl
{
    const CompressedAnimation* compressed = nullptr;
    std::vector<std::string> boneNames;
    std::vector<std::string> morphNames;
    uint32_t maxFrame = 0;
    float duration = 0.0f;
    
    // Parsed data (for stub implementation)
    struct BoneTrack
    {
        std::vector<VmdKeyframe> keys;
    };
    struct MorphTrack
    {
        std::vector<VmdScalarKeyframe> keys;
    };
    std::unordered_map<std::string, BoneTrack> boneTracks;
    std::unordered_map<std::string, MorphTrack> morphTracks;
};

AclDecompressor::AclDecompressor() : m_impl(std::make_unique<Impl>())
{
}

AclDecompressor::~AclDecompressor() = default;

bool AclDecompressor::Load(const CompressedAnimation& compressed)
{
    m_impl = std::make_unique<Impl>();
    m_impl->compressed = &compressed;
    m_impl->boneNames = compressed.boneNames;
    m_impl->morphNames = compressed.morphNames;
    m_impl->maxFrame = compressed.maxFrame;
    m_impl->duration = compressed.duration;
    
    m_boneNames = compressed.boneNames;
    m_morphNames = compressed.morphNames;
    m_maxFrame = compressed.maxFrame;
    m_duration = compressed.duration;
    m_compressedSize = compressed.compressedSize;
    m_compressionRatio = compressed.compressionRatio;
    
    // TODO: Parse compressed data and populate bone/morph tracks
    // For now, just return true
    return true;
}

bool AclDecompressor::SampleBoneTransform(const std::string& boneName, float frame, 
                                         DirectX::XMVECTOR& outRotation, DirectX::XMVECTOR& outPosition) const
{
    if (!m_impl || !m_impl->compressed)
    {
        return false;
    }
    
    // TODO: Implement actual ACL sampling
    // For now, return identity
    outRotation = DirectX::XMQuaternionIdentity();
    outPosition = DirectX::XMVectorZero();
    return true;
}

bool AclDecompressor::SampleAllBones(float frame, 
                                    std::vector<DirectX::XMVECTOR>& outRotations,
                                    std::vector<DirectX::XMVECTOR>& outPositions) const
{
    if (!m_impl || !m_impl->compressed)
    {
        return false;
    }
    
    // TODO: Implement actual ACL sampling
    return false;
}

float AclDecompressor::SampleMorph(const std::string& morphName, float frame) const
{
    // TODO: Implement actual ACL sampling
    return 0.0f;
}

} // namespace ACLCompression