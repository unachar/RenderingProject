#pragma once
#include "virtualgeometrytypes.h"
#include <span>
#include <string>
#include <vector>

// All angles are radians and all position errors are in object-space units.
struct VGCompressionSettings
{
    bool Enabled = true; // False selects the Phase 9 raw Page layout for controlled A/B captures.
    float MaxPositionError = 0.001f;
    float MaxNormalAngularError = 0.001f;
    float MaxTangentAngularError = 0.005f;
    float MaxTexcoordError = 0.0005f;
    bool AllowPositionQuantization = true;
    bool AllowNormalCompression = true;
    bool AllowTangentCompression = true;
    bool AllowHalfTexcoord = true;
    bool AllowIndex8 = true;
};

namespace VirtualGeometryCompression
{
inline constexpr uint32_t CompressionVersion = 1;
inline constexpr uint32_t ClusterFlag = 0x10000;
inline constexpr uint32_t PayloadMagic = 0x31434756; // VGC1
inline constexpr uint32_t HeaderBytes = 56;
enum class Codec : uint32_t { Float32, UNorm16x3, OctSnorm16x2, Snorm1010102, Float16x2, UNorm8x4 };
enum Semantic : uint32_t { Position = 0, Normal = 1, Tangent = 2, Color = 3, Texcoord0 = 8 };
enum class IndexCodec : uint32_t { UInt8 = 1, UInt16 = 2 };

struct Input
{
    std::span<const VirtualGeometryVertex> Vertices;
    std::span<const std::array<float, 4>> Tangents;
    // Empty means use the legacy vertex's UV0; otherwise one span per UV channel.
    std::vector<std::span<const std::array<float, 2>>> Texcoords;
    bool HasNormal = true;
    bool HasTexcoord = true;
    bool HasColor = true;
};
struct Stream
{
    uint32_t Attribute = Position;
    Codec Encoding = Codec::Float32;
};
struct Plan
{
    std::array<float, 3> Origin{};
    std::array<float, 3> Step{};
    std::vector<Stream> Streams;
    std::vector<std::string> FallbackReasons;
    VGCompressionSettings Settings;
    float PositionError = 0;
};
struct DecodedVertex
{
    std::array<float, 3> Position{};
    std::array<float, 3> Normal{0, 1, 0};
    std::array<float, 4> Tangent{1, 0, 0, 1};
    std::array<float, 4> Color{1, 1, 1, 1};
    std::vector<std::array<float, 2>> Texcoords;
};
struct Metrics
{
    uint64_t EncodedBytes = 0;
    uint64_t RawBytes = 0;
    uint64_t FallbackCount = 0;
    uint64_t PositionQuantized = 0;
    uint64_t HalfUV = 0;
    uint64_t Index8Clusters = 0;
    uint64_t Index16Clusters = 0;
};
IndexCodec SelectIndexCodec(uint32_t vertexCount, bool allow8);
uint64_t SettingsKey(const VGCompressionSettings& settings);
bool Select(const Input& input, const VGCompressionSettings& settings, Plan& plan, std::string& error);
bool EncodeCluster(const Input& input, const Plan& plan, uint32_t firstVertex, uint32_t vertexCount,
    std::span<const uint32_t> indices, std::vector<uint8_t>& payload, std::string& error);
// Structural validation never expands geometry; used before an encoded Page reaches the GPU.
bool Validate(std::span<const uint8_t> payload, uint32_t vertices, uint32_t triangles, std::string& error);
DecodedVertex DecodeVertexCPU(std::span<const uint8_t> payload, uint32_t vertexIndex);
std::array<uint32_t, 3> LoadTriangleCPU(std::span<const uint8_t> payload, uint32_t triangleIndex);
}
