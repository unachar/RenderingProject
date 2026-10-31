#pragma once

#include "vmdanimationimpoter.h"
#include <vector>
#include <string>
#include <memory>
#include <cstdint>

namespace ACLCompression
{
    // ACL compression settings
    struct CompressionSettings
    {
        // Quality level (0.0 = lowest, 1.0 = highest)
        float quality = 0.75f;
        
        // Maximum error threshold for rotation (degrees)
        float maxRotationErrorDegrees = 0.5f;
        
        // Maximum error threshold for translation (units)
        float maxTranslationError = 0.01f;
        
        // Maximum error threshold for scale
        float maxScaleError = 0.001f;
        
        // Enable loop detection and optimization
        bool enableLoopOptimization = true;
        
        // Rotation format: 0=quaternion, 1=rotation vector
        int rotationFormat = 0;
    };

    // Compressed animation data
    struct CompressedAnimation
    {
        // Metadata
        std::string name;
        uint32_t maxFrame = 0;
        float duration = 0.0f;
        uint32_t boneCount = 0;
        uint32_t morphCount = 0;
        
        // ACL compressed data (opaque blob)
        std::vector<uint8_t> compressedData;
        
        // Bone name to index mapping
        std::vector<std::string> boneNames;
        
        // Morph names
        std::vector<std::string> morphNames;
        
        // Compression statistics
        size_t originalSize = 0;
        size_t compressedSize = 0;
        float compressionRatio = 0.0f;
        float maxRotationError = 0.0f;
        float maxTranslationError = 0.0f;
    };

    // ACL compression interface
    class AclCompressor
    {
    public:
        AclCompressor();
        ~AclCompressor();
        
        // Compress a VMD animation
        bool Compress(const VmdAnimation& animation, const CompressionSettings& settings, CompressedAnimation& outCompressed);
        
        // Compress multiple animations
        bool CompressBatch(const std::vector<std::pair<std::string, const VmdAnimation*>>& animations, 
                          const CompressionSettings& settings, 
                          std::vector<CompressedAnimation>& outCompressed);
        
        // Get last error message
        const std::string& GetLastError() const { return m_lastError; }
        
        // Get compression statistics
        struct Statistics
        {
            size_t totalOriginalSize = 0;
            size_t totalCompressedSize = 0;
            float averageCompressionRatio = 0.0f;
            float averageRotationError = 0.0f;
            float averageTranslationError = 0.0f;
        };
        Statistics GetStatistics() const;
        
    private:
        std::string m_lastError;
        Statistics m_stats;
        
        bool CompressBoneTracks(const VmdAnimation& animation, const CompressionSettings& settings);
        bool CompressMorphTracks(const VmdAnimation& animation, const CompressionSettings& settings);
        void WriteCompressedData(std::vector<uint8_t>& outData);
    };
    
    // ACL decompressor for runtime
    class AclDecompressor
    {
    public:
        AclDecompressor();
        ~AclDecompressor();
        
        // Load compressed animation
        bool Load(const CompressedAnimation& compressed);
        
        // Sample bone transform at given frame
        bool SampleBoneTransform(const std::string& boneName, float frame, 
                                DirectX::XMVECTOR& outRotation, DirectX::XMVECTOR& outPosition) const;
        
        // Sample all bones at given frame
        bool SampleAllBones(float frame, 
                           std::vector<DirectX::XMVECTOR>& outRotations,
                           std::vector<DirectX::XMVECTOR>& outPositions) const;
        
        // Sample morph weight
        float SampleMorph(const std::string& morphName, float frame) const;
        
        // Get animation info
        uint32_t GetMaxFrame() const { return m_maxFrame; }
        float GetDuration() const { return m_duration; }
        const std::vector<std::string>& GetBoneNames() const { return m_boneNames; }
        const std::vector<std::string>& GetMorphNames() const { return m_morphNames; }
        
        // Get compression info
        size_t GetCompressedSize() const { return m_compressedSize; }
        float GetCompressionRatio() const { return m_compressionRatio; }
        
    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
        uint32_t m_maxFrame = 0;
        float m_duration = 0.0f;
        std::vector<std::string> m_boneNames;
        std::vector<std::string> m_morphNames;
        size_t m_compressedSize = 0;
        float m_compressionRatio = 0.0f;
    };

} // namespace ACLCompression