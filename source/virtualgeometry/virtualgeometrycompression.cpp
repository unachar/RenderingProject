#include "virtualgeometrycompression.h"
#include "virtualgeometryserializationinternal.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace VirtualGeometryCompression
{
namespace
{
using VirtualGeometrySerialization::Writer;
using Vec3 = std::array<float, 3>;
std::string AttributeName(uint32_t semantic)
{
    switch (semantic)
    {
    case Position: return "Position";
    case Normal: return "Normal";
    case Tangent: return "Tangent";
    case Color: return "Color";
    default: return "UV" + std::to_string(semantic - Texcoord0);
    }
}
uint32_t Read(std::span<const uint8_t> bytes, size_t offset, uint32_t width = 4)
{
    if (offset > bytes.size() || width > bytes.size() - offset)
    {
        throw std::runtime_error("Compressed geometry offset out of range");
    }
    uint32_t value = 0;
    for (uint32_t byte = 0; byte < width; ++byte)
    {
        value |= uint32_t(bytes[offset + byte]) << (8 * byte);
    }
    return value;
}
float ReadFloat(std::span<const uint8_t> bytes, size_t offset)
{
    return std::bit_cast<float>(Read(bytes, offset));
}
void Put(std::vector<uint8_t>& bytes, size_t offset, uint32_t value, uint32_t width = 4)
{
    if (offset > bytes.size() || width > bytes.size() - offset)
    {
        throw std::runtime_error("Compressed geometry write out of range");
    }
    for (uint32_t byte = 0; byte < width; ++byte)
    {
        bytes[offset + byte] = uint8_t(value >> (8 * byte));
    }
}
float Sign(float value)
{
    if (value < 0)
    {
        return -1;
    }
    return 1;
}
Vec3 Normalize(Vec3 value)
{
    double length = std::sqrt(double(value[0])*value[0] + double(value[1])*value[1] + double(value[2])*value[2]);
    if (!std::isfinite(length) || length <= 1e-30)
    {
        return {0, 1, 0};
    }
    for (auto& component : value)
    {
        component = float(component / length);
    }
    return value;
}
float Angle(Vec3 first, Vec3 second)
{
    first = Normalize(first);
    second = Normalize(second);
    // Chord length avoids acos losing precision close to parallel vectors.
    double squared = 0;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        double delta = double(first[axis]) - second[axis];
        squared += delta * delta;
    }
    return float(2 * std::asin(std::min(1.0, std::sqrt(squared) * 0.5)));
}
uint32_t PackOct(Vec3 normal)
{
    normal = Normalize(normal);
    float inverse = 1 / (std::abs(normal[0]) + std::abs(normal[1]) + std::abs(normal[2]));
    float x = normal[0] * inverse;
    float y = normal[1] * inverse;
    if (normal[2] < 0)
    {
        float oldX = x;
        x = (1 - std::abs(y)) * Sign(oldX);
        y = (1 - std::abs(oldX)) * Sign(y);
    }
    uint16_t packedX = uint16_t(int16_t(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767)));
    uint16_t packedY = uint16_t(int16_t(std::lround(std::clamp(y, -1.0f, 1.0f) * 32767)));
    return uint32_t(packedX) | (uint32_t(packedY) << 16);
}
int32_t Signed(uint32_t value, uint32_t bits)
{
    uint32_t signBit = 1u << (bits - 1);
    return int32_t(value & (signBit - 1)) - int32_t(value & signBit);
}
Vec3 UnpackOct(uint32_t packed)
{
    float x = std::max(-1.0f, float(Signed(packed & 65535, 16)) / 32767);
    float y = std::max(-1.0f, float(Signed(packed >> 16, 16)) / 32767);
    float z = 1 - std::abs(x) - std::abs(y);
    if (z < 0)
    {
        float oldX = x;
        x = (1 - std::abs(y)) * Sign(oldX);
        y = (1 - std::abs(oldX)) * Sign(y);
    }
    return Normalize({x, y, z});
}
uint32_t PackTangent(const std::array<float, 4>& tangent)
{
    Vec3 direction = Normalize({tangent[0], tangent[1], tangent[2]});
    uint32_t packed = 1u << 30;
    if (tangent[3] < 0)
    {
        packed = 3u << 30;
    }
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        packed |= (uint32_t(int32_t(std::lround(direction[axis] * 511))) & 1023) << (axis * 10);
    }
    return packed;
}
Vec3 UnpackTangent(uint32_t packed)
{
    return Normalize({float(Signed(packed & 1023, 10)) / 511,
        float(Signed((packed >> 10) & 1023, 10)) / 511,
        float(Signed((packed >> 20) & 1023, 10)) / 511});
}
Vec3 Orthogonalize(Vec3 tangent, Vec3 normal)
{
    normal = Normalize(normal);
    float dot = tangent[0]*normal[0] + tangent[1]*normal[1] + tangent[2]*normal[2];
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        tangent[axis] -= dot * normal[axis];
    }
    if (std::abs(tangent[0]) + std::abs(tangent[1]) + std::abs(tangent[2]) < 1e-6f)
    {
        Vec3 helper{0, 0, 1};
        if (std::abs(normal[2]) > 0.9f)
        {
            helper = {0, 1, 0};
        }
        tangent = {helper[1]*normal[2]-helper[2]*normal[1],
            helper[2]*normal[0]-helper[0]*normal[2], helper[0]*normal[1]-helper[1]*normal[0]};
    }
    return Normalize(tangent);
}
// IEEE binary16, round to nearest even. Shared wire representation with HLSL f16tof32.
uint16_t ToHalf(float value)
{
    uint32_t bits = std::bit_cast<uint32_t>(value);
    uint32_t sign = (bits >> 16) & 0x8000;
    int32_t exponent = int32_t((bits >> 23) & 255) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffff;
    if (exponent >= 31)
    {
        return uint16_t(sign | 0x7c00);
    }
    if (exponent <= 0)
    {
        if (exponent < -10)
        {
            return uint16_t(sign);
        }
        mantissa |= 0x800000;
        uint32_t shift = uint32_t(14 - exponent);
        uint32_t rounded = (mantissa + ((1u << (shift - 1)) - 1) + ((mantissa >> shift) & 1)) >> shift;
        return uint16_t(sign | rounded);
    }
    uint32_t rounded = mantissa + 0xfff + ((mantissa >> 13) & 1);
    return uint16_t(sign | ((uint32_t(exponent) << 10) + (rounded >> 13)));
}
float FromHalf(uint16_t value)
{
    uint32_t sign = uint32_t(value & 0x8000) << 16;
    uint32_t exponent = (value >> 10) & 31;
    uint32_t mantissa = value & 1023;
    if (!exponent)
    {
        float result = std::ldexp(float(mantissa), -24);
        if (sign)
        {
            result = -result;
        }
        return result;
    }
    if (exponent == 31)
    {
        return std::bit_cast<float>(sign | 0x7f800000 | (mantissa << 13));
    }
    return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}
uint32_t Components(uint32_t semantic)
{
    if (semantic >= Texcoord0)
    {
        return 2;
    }
    if (semantic == Tangent || semantic == Color)
    {
        return 4;
    }
    return 3;
}
uint32_t Stride(const Stream& stream)
{
    if (stream.Encoding == Codec::Float32)
    {
        return Components(stream.Attribute) * 4;
    }
    if (stream.Encoding == Codec::UNorm16x3)
    {
        return 6;
    }
    return 4;
}
std::array<float, 4> Attribute(const Input& input, uint32_t index, uint32_t semantic)
{
    if (index >= input.Vertices.size())
    {
        throw std::runtime_error("Compression vertex stream index out of range");
    }
    const auto& vertex = input.Vertices[index];
    if (semantic == Position)
    {
        return {vertex.Position[0], vertex.Position[1], vertex.Position[2], 0};
    }
    if (semantic == Normal)
    {
        return {vertex.Normal[0], vertex.Normal[1], vertex.Normal[2], 0};
    }
    if (semantic == Tangent)
    {
        if (index >= input.Tangents.size())
        {
            throw std::runtime_error("Compression tangent stream index out of range");
        }
        return input.Tangents[index];
    }
    if (semantic == Color)
    {
        return {vertex.Diffuse[0], vertex.Diffuse[1], vertex.Diffuse[2], vertex.Diffuse[3]};
    }
    if (input.Texcoords.empty())
    {
        return {vertex.TexCoord[0], vertex.TexCoord[1], 0, 0};
    }
    auto stream = input.Texcoords.at(semantic - Texcoord0);
    if (index >= stream.size())
    {
        throw std::runtime_error("Compression UV stream index out of range");
    }
    const auto& uv = stream[index];
    return {uv[0], uv[1], 0, 0};
}
uint16_t Quantize(float value, float origin, float step)
{
    if (step == 0)
    {
        return 0;
    }
    return uint16_t(std::round(std::clamp((double(value) - origin) / step, 0.0, 65535.0)));
}
float DecodePosition(float origin, float step, uint32_t quantized)
{
    // GPU uses precise multiply/add, preventing an FMA-specific CPU/GPU boundary difference.
    volatile float delta = float(quantized) * step;
    return origin + delta;
}
void EncodeAttribute(std::vector<uint8_t>& bytes, uint32_t address, const Stream& stream,
    const std::array<float, 4>& value, const Plan& plan)
{
    switch (stream.Encoding)
    {
    case Codec::Float32:
        for (uint32_t component = 0; component < Components(stream.Attribute); ++component)
        {
            Put(bytes, address + component*4, std::bit_cast<uint32_t>(value[component]));
        }
        break;
    case Codec::UNorm16x3:
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            Put(bytes, address + axis*2, Quantize(value[axis], plan.Origin[axis], plan.Step[axis]), 2);
        }
        break;
    case Codec::OctSnorm16x2:
        Put(bytes, address, PackOct({value[0], value[1], value[2]}));
        break;
    case Codec::Snorm1010102:
        Put(bytes, address, PackTangent(value));
        break;
    case Codec::Float16x2:
        Put(bytes, address, uint32_t(ToHalf(value[0])) | (uint32_t(ToHalf(value[1])) << 16));
        break;
    case Codec::UNorm8x4:
        for (uint32_t component = 0; component < 4; ++component)
        {
            Put(bytes, address + component, uint32_t(std::lround(std::clamp(value[component], 0.0f, 1.0f)*255)), 1);
        }
        break;
    }
}
}

IndexCodec SelectIndexCodec(uint32_t vertexCount, bool allow8)
{
    if (vertexCount == 0 || vertexCount > 65536)
    {
        throw std::runtime_error("Index codec vertex count out of range");
    }
    if (allow8 && vertexCount <= 256)
    {
        return IndexCodec::UInt8;
    }
    return IndexCodec::UInt16;
}

uint64_t SettingsKey(const VGCompressionSettings& settings)
{
    Writer writer;
    writer.U32(CompressionVersion); // Bump when rounding or stream packing changes.
    writer.F32(settings.MaxPositionError);
    writer.F32(settings.MaxNormalAngularError);
    writer.F32(settings.MaxTangentAngularError);
    writer.F32(settings.MaxTexcoordError);
    writer.U32(uint32_t(settings.Enabled));
    writer.U32(uint32_t(settings.AllowPositionQuantization));
    writer.U32(uint32_t(settings.AllowNormalCompression));
    writer.U32(uint32_t(settings.AllowTangentCompression));
    writer.U32(uint32_t(settings.AllowHalfTexcoord));
    writer.U32(uint32_t(settings.AllowIndex8));
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : writer.Bytes)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool Select(const Input& input, const VGCompressionSettings& settings, Plan& output, std::string& error)
{
    try
    {
        if (input.Vertices.empty() || input.Vertices.size() > UINT32_MAX || input.Texcoords.size() > 24 ||
            (!input.Tangents.empty() && input.Tangents.size() != input.Vertices.size()))
        {
            throw std::runtime_error("Invalid compression input streams");
        }
        for (auto stream : input.Texcoords)
        {
            if (stream.size() != input.Vertices.size())
            {
                throw std::runtime_error("UV stream vertex count mismatch");
            }
        }
        for (float threshold : {settings.MaxPositionError, settings.MaxNormalAngularError,
            settings.MaxTangentAngularError, settings.MaxTexcoordError})
        {
            if (!std::isfinite(threshold) || threshold < 0)
            {
                throw std::runtime_error("Invalid compression error threshold");
            }
        }
        Plan plan;
        plan.Settings = settings;
        plan.Streams.push_back({Position, Codec::Float32});
        if (input.HasNormal)
        {
            plan.Streams.push_back({Normal, Codec::Float32});
        }
        if (!input.Tangents.empty())
        {
            plan.Streams.push_back({Tangent, Codec::Float32});
        }
        if (input.HasColor)
        {
            plan.Streams.push_back({Color, Codec::UNorm8x4});
        }
        uint32_t uvCount = 0;
        if (input.HasTexcoord)
        {
            uvCount = std::max(1u, uint32_t(input.Texcoords.size()));
        }
        for (uint32_t channel = 0; channel < uvCount; ++channel)
        {
            plan.Streams.push_back({Texcoord0 + channel, Codec::Float32});
        }
        Vec3 maximum{};
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            plan.Origin[axis] = input.Vertices.front().Position[axis];
            maximum[axis] = plan.Origin[axis];
        }
        for (uint32_t index = 0; index < input.Vertices.size(); ++index)
        {
            for (const auto& stream : plan.Streams)
            {
                auto value = Attribute(input, index, stream.Attribute);
                for (uint32_t component = 0; component < Components(stream.Attribute); ++component)
                {
                    if (!std::isfinite(value[component]))
                    {
                        throw std::runtime_error("Nonfinite " + AttributeName(stream.Attribute) + " at vertex " + std::to_string(index));
                    }
                }
            }
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                plan.Origin[axis] = std::min(plan.Origin[axis], input.Vertices[index].Position[axis]);
                maximum[axis] = std::max(maximum[axis], input.Vertices[index].Position[axis]);
            }
        }
        bool validGrid = true;
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            plan.Step[axis] = float((double(maximum[axis]) - plan.Origin[axis]) / 65535.0);
            double endpoint = double(plan.Origin[axis]) + double(plan.Step[axis])*65535;
            validGrid &= std::isfinite(plan.Step[axis]) && std::abs(endpoint) <= std::numeric_limits<float>::max() &&
                double(plan.Step[axis])*65535 <= std::numeric_limits<float>::max();
        }
        for (auto& stream : plan.Streams)
        {
            bool candidate = false;
            if (stream.Attribute == Position)
            {
                candidate = settings.AllowPositionQuantization;
                stream.Encoding = Codec::UNorm16x3;
            }
            else if (stream.Attribute == Normal)
            {
                candidate = settings.AllowNormalCompression;
                stream.Encoding = Codec::OctSnorm16x2;
            }
            else if (stream.Attribute == Tangent)
            {
                candidate = settings.AllowTangentCompression;
                stream.Encoding = Codec::Snorm1010102;
            }
            else if (stream.Attribute >= Texcoord0)
            {
                candidate = settings.AllowHalfTexcoord;
                stream.Encoding = Codec::Float16x2;
            }
            else
            {
                // Preserve HDR/out-of-range colors through a full precision fallback.
                candidate = true;
            }
            if (!candidate)
            {
                stream.Encoding = Codec::Float32;
                continue;
            }
            bool accurate = true;
            for (uint32_t index = 0; index < input.Vertices.size(); ++index)
            {
                auto value = Attribute(input, index, stream.Attribute);
                if (stream.Attribute == Position)
                {
                    double squared = 0;
                    for (uint32_t axis = 0; axis < 3; ++axis)
                    {
                        float decoded = DecodePosition(plan.Origin[axis], plan.Step[axis],
                            Quantize(value[axis], plan.Origin[axis], plan.Step[axis]));
                        double delta = double(value[axis]) - decoded;
                        squared += delta*delta;
                    }
                    float distance = float(std::sqrt(squared));
                    plan.PositionError = std::max(plan.PositionError, distance);
                    accurate &= validGrid && std::isfinite(distance) && distance <= settings.MaxPositionError;
                }
                else if (stream.Attribute == Normal)
                {
                    accurate &= Angle({value[0], value[1], value[2]}, UnpackOct(PackOct({value[0], value[1], value[2]}))) <= settings.MaxNormalAngularError;
                }
                else if (stream.Attribute == Tangent)
                {
                    auto sourceNormal = Attribute(input, index, Normal);
                    Vec3 normal{sourceNormal[0], sourceNormal[1], sourceNormal[2]};
                    if (!input.HasNormal)
                    {
                        normal = {0, 1, 0};
                    }
                    Vec3 decodedNormal = normal;
                    for (const auto& normalStream : plan.Streams)
                    {
                        if (normalStream.Attribute == Normal && normalStream.Encoding == Codec::OctSnorm16x2)
                        {
                            decodedNormal = UnpackOct(PackOct(normal));
                        }
                    }
                    accurate &= Angle(Orthogonalize({value[0], value[1], value[2]}, normal),
                        Orthogonalize(UnpackTangent(PackTangent(value)), decodedNormal)) <= settings.MaxTangentAngularError;
                }
                else if (stream.Attribute >= Texcoord0)
                {
                    double squared = 0;
                    for (uint32_t component = 0; component < 2; ++component)
                    {
                        float decoded = FromHalf(ToHalf(value[component]));
                        double delta = double(value[component]) - decoded;
                        squared += delta*delta;
                        accurate &= std::abs(value[component]) <= 65504 && std::isfinite(decoded);
                    }
                    accurate &= std::sqrt(squared) <= settings.MaxTexcoordError;
                }
                else
                {
                    for (float component : value)
                    {
                        accurate &= component >= 0 && component <= 1;
                    }
                }
            }
            if (!accurate)
            {
                stream.Encoding = Codec::Float32;
                plan.FallbackReasons.push_back(AttributeName(stream.Attribute) + ": codec error/range exceeded; Float32 fallback");
            }
        }
        Stream* normalStream = nullptr;
        Stream* tangentStream = nullptr;
        for (auto& stream : plan.Streams)
        {
            if (stream.Attribute == Normal)
            {
                normalStream = &stream;
            }
            if (stream.Attribute == Tangent)
            {
                tangentStream = &stream;
            }
        }
        // A full precision tangent can still lose angular accuracy when projected onto a packed normal.
        if (normalStream && tangentStream && normalStream->Encoding == Codec::OctSnorm16x2 && tangentStream->Encoding == Codec::Float32)
        {
            bool accurate = true;
            for (uint32_t index = 0; index < input.Vertices.size(); ++index)
            {
                auto sourceNormal = Attribute(input, index, Normal);
                auto sourceTangent = Attribute(input, index, Tangent);
                Vec3 normal{sourceNormal[0], sourceNormal[1], sourceNormal[2]};
                Vec3 tangent{sourceTangent[0], sourceTangent[1], sourceTangent[2]};
                accurate &= Angle(Orthogonalize(tangent, normal), Orthogonalize(tangent, UnpackOct(PackOct(normal)))) <= settings.MaxTangentAngularError;
            }
            if (!accurate)
            {
                normalStream->Encoding = Codec::Float32;
                plan.FallbackReasons.push_back("Normal: Float32 fallback to preserve tangent angular accuracy");
            }
        }
        if (plan.Streams.front().Encoding == Codec::Float32)
        {
            plan.PositionError = 0;
            plan.Step = {};
        }
        output = std::move(plan);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool EncodeCluster(const Input& input, const Plan& plan, uint32_t firstVertex, uint32_t vertexCount,
    std::span<const uint32_t> indices, std::vector<uint8_t>& output, std::string& error)
{
    try
    {
        if (firstVertex > input.Vertices.size() || vertexCount > input.Vertices.size() - firstVertex ||
            indices.empty() || indices.size() % 3 || plan.Streams.empty() || plan.Streams.size() > 28)
        {
            throw std::runtime_error("Invalid compressed cluster input range");
        }
        uint32_t width = uint32_t(SelectIndexCodec(vertexCount, plan.Settings.AllowIndex8));
        Writer header;
        header.U32(PayloadMagic);
        header.U32(0); // Total payload bytes, patched after packing.
        header.U32(vertexCount);
        header.U32(uint32_t(indices.size() / 3));
        uint32_t mask = 0;
        for (const auto& stream : plan.Streams)
        {
            mask |= 1u << stream.Attribute;
        }
        header.U32(mask);
        header.U32(width);
        header.U32(0); // Index offset.
        header.U32(uint32_t(plan.Streams.size()));
        header.Float3(plan.Origin);
        header.Float3(plan.Step);
        uint32_t offset = HeaderBytes + uint32_t(plan.Streams.size())*16;
        for (const auto& stream : plan.Streams)
        {
            header.U32(stream.Attribute);
            header.U32(uint32_t(stream.Encoding));
            header.U32(offset);
            header.U32(Stride(stream));
            offset = (offset + Stride(stream)*vertexCount + 3) & ~3u;
        }
        uint32_t indexOffset = offset;
        uint64_t total = (uint64_t(offset) + uint64_t(indices.size())*width + 3) & ~uint64_t(3);
        if (total > VirtualGeometryConfig::GeometryPageBytes)
        {
            throw std::runtime_error("Compressed cluster exceeds existing Streaming Page size");
        }
        auto bytes = std::move(header.Bytes);
        bytes.resize(size_t(total), 0);
        Put(bytes, 4, uint32_t(total));
        Put(bytes, 24, indexOffset);
        for (uint32_t streamIndex = 0; streamIndex < plan.Streams.size(); ++streamIndex)
        {
            const auto& stream = plan.Streams[streamIndex];
            uint32_t start = Read(bytes, HeaderBytes + streamIndex*16 + 8);
            for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
            {
                EncodeAttribute(bytes, start + vertex*Stride(stream), stream, Attribute(input, firstVertex + vertex, stream.Attribute), plan);
            }
        }
        for (size_t index = 0; index < indices.size(); ++index)
        {
            if (indices[index] >= vertexCount)
            {
                throw std::runtime_error("Compressed cluster index exceeds VertexCount");
            }
            Put(bytes, indexOffset + index*width, indices[index], width);
        }
        if (!Validate(bytes, vertexCount, uint32_t(indices.size()/3), error))
        {
            return false;
        }
        // Asset-build-only reference validation. Runtime uploads the bytes unchanged.
        for (uint32_t index = 0; index < vertexCount; ++index)
        {
            auto decoded = DecodeVertexCPU(bytes, index);
            const auto& source = input.Vertices[firstVertex + index];
            double squared = 0;
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                double delta = double(decoded.Position[axis]) - source.Position[axis];
                squared += delta*delta;
            }
            if (!std::isfinite(squared) || std::sqrt(squared) > plan.Settings.MaxPositionError)
            {
                throw std::runtime_error("Position reference decode exceeds error threshold");
            }
        }
        output = std::move(bytes);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool Validate(std::span<const uint8_t> bytes, uint32_t vertices, uint32_t triangles, std::string& error)
{
    try
    {
        if (bytes.size() < HeaderBytes || bytes.size() > VirtualGeometryConfig::GeometryPageBytes || bytes.size() % 4 ||
            Read(bytes, 0) != PayloadMagic || Read(bytes, 4) != bytes.size() || Read(bytes, 8) != vertices ||
            !vertices || vertices > 65536 || !triangles || Read(bytes, 12) != triangles)
        {
            throw std::runtime_error("Invalid compressed cluster header/count/size");
        }
        uint32_t width = Read(bytes, 20);
        uint32_t count = Read(bytes, 28);
        uint32_t indexOffset = Read(bytes, 24);
        if ((width != 1 && width != 2) || (width == 1 && vertices > 256) || !count || count > 28 ||
            HeaderBytes + uint64_t(count)*16 > bytes.size())
        {
            throw std::runtime_error("Invalid compressed stream/index directory");
        }
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            float origin = ReadFloat(bytes, 32 + axis*4);
            float step = ReadFloat(bytes, 44 + axis*4);
            double end = double(origin) + double(step)*65535;
            if (!std::isfinite(origin) || !std::isfinite(step) || step < 0 ||
                end > std::numeric_limits<float>::max() || end < -std::numeric_limits<float>::max() ||
                double(step)*65535 > std::numeric_limits<float>::max())
            {
                throw std::runtime_error("Invalid global quantization grid");
            }
        }
        uint64_t end = HeaderBytes + count*16;
        uint32_t mask = 0;
        uint32_t previous = 0;
        for (uint32_t index = 0; index < count; ++index)
        {
            uint32_t descriptor = HeaderBytes + index*16;
            uint32_t semantic = Read(bytes, descriptor);
            Codec codec = Codec(Read(bytes, descriptor + 4));
            uint32_t offset = Read(bytes, descriptor + 8);
            uint32_t stride = Read(bytes, descriptor + 12);
            bool supported = codec == Codec::Float32 || (semantic == Position && codec == Codec::UNorm16x3) ||
                (semantic == Normal && codec == Codec::OctSnorm16x2) || (semantic == Tangent && codec == Codec::Snorm1010102) ||
                (semantic == Color && codec == Codec::UNorm8x4) || (semantic >= Texcoord0 && codec == Codec::Float16x2);
            if (semantic >= 32 || (semantic > Color && semantic < Texcoord0) ||
                (!index && semantic != Position) || (index && semantic <= previous) || !supported ||
                stride != Stride({semantic, codec}) || offset != end || offset % 4)
            {
                throw std::runtime_error("Invalid compressed attribute descriptor");
            }
            previous = semantic;
            mask |= 1u << semantic;
            end = (uint64_t(offset) + uint64_t(vertices)*stride + 3) & ~uint64_t(3);
            if (end > bytes.size())
            {
                throw std::runtime_error("Compressed attribute outside Page payload");
            }
            if (codec == Codec::Float32)
            {
                for (uint32_t vertex = 0; vertex < vertices; ++vertex)
                {
                    for (uint32_t component = 0; component < Components(semantic); ++component)
                    {
                        if (!std::isfinite(ReadFloat(bytes, offset + vertex*stride + component*4)))
                        {
                            throw std::runtime_error("Nonfinite compressed Float32 attribute");
                        }
                    }
                }
            }
            if (codec == Codec::Float16x2)
            {
                for (uint32_t vertex = 0; vertex < vertices; ++vertex)
                {
                    uint32_t packed = Read(bytes, offset + vertex*stride);
                    if ((packed & 0x7c00) == 0x7c00 || (packed & 0x7c000000) == 0x7c000000)
                    {
                        throw std::runtime_error("Nonfinite compressed half UV");
                    }
                }
            }
        }
        uint64_t expectedEnd = (end + uint64_t(triangles)*3*width + 3) & ~uint64_t(3);
        if (mask != Read(bytes, 16) || indexOffset != end || expectedEnd != bytes.size())
        {
            throw std::runtime_error("Invalid compressed index range/attribute mask");
        }
        for (uint64_t index = 0; index < uint64_t(triangles)*3; ++index)
        {
            if (Read(bytes, indexOffset + size_t(index)*width, width) >= vertices)
            {
                throw std::runtime_error("Compressed triangle index out of range");
            }
        }
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

DecodedVertex DecodeVertexCPU(std::span<const uint8_t> bytes, uint32_t vertexIndex)
{
    if (vertexIndex >= Read(bytes, 8))
    {
        throw std::runtime_error("Compressed vertex index out of range");
    }
    DecodedVertex result;
    uint32_t count = Read(bytes, 28);
    if (count > 28)
    {
        throw std::runtime_error("Compressed stream count out of range");
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        uint32_t descriptor = HeaderBytes + index*16;
        uint32_t semantic = Read(bytes, descriptor);
        Codec codec = Codec(Read(bytes, descriptor + 4));
        size_t address = Read(bytes, descriptor + 8) + size_t(vertexIndex)*Read(bytes, descriptor + 12);
        std::array<float, 4> value{};
        if (codec == Codec::Float32)
        {
            for (uint32_t component = 0; component < Components(semantic); ++component)
            {
                value[component] = ReadFloat(bytes, address + component*4);
            }
        }
        else if (codec == Codec::UNorm16x3)
        {
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                value[axis] = DecodePosition(ReadFloat(bytes, 32 + axis*4), ReadFloat(bytes, 44 + axis*4), Read(bytes, address + axis*2, 2));
            }
        }
        else if (codec == Codec::OctSnorm16x2)
        {
            auto normal = UnpackOct(Read(bytes, address));
            std::copy(normal.begin(), normal.end(), value.begin());
        }
        else if (codec == Codec::Snorm1010102)
        {
            uint32_t packed = Read(bytes, address);
            auto tangent = UnpackTangent(packed);
            std::copy(tangent.begin(), tangent.end(), value.begin());
            value[3] = 1;
            if ((packed >> 30) == 3)
            {
                value[3] = -1;
            }
        }
        else if (codec == Codec::Float16x2)
        {
            uint32_t packed = Read(bytes, address);
            value[0] = FromHalf(uint16_t(packed));
            value[1] = FromHalf(uint16_t(packed >> 16));
        }
        else if (codec == Codec::UNorm8x4)
        {
            for (uint32_t component = 0; component < 4; ++component)
            {
                value[component] = float(Read(bytes, address + component, 1)) / 255;
            }
        }
        else
        {
            throw std::runtime_error("Unsupported geometry codec");
        }
        if (semantic == Position)
        {
            result.Position = {value[0], value[1], value[2]};
        }
        else if (semantic == Normal)
        {
            result.Normal = Normalize({value[0], value[1], value[2]});
        }
        else if (semantic == Tangent)
        {
            result.Tangent = value;
        }
        else if (semantic == Color)
        {
            result.Color = value;
        }
        else if (semantic >= Texcoord0 && semantic < 32)
        {
            result.Texcoords.resize(std::max(result.Texcoords.size(), size_t(semantic - Texcoord0 + 1)));
            result.Texcoords[semantic - Texcoord0] = {value[0], value[1]};
        }
    }
    auto tangent = Orthogonalize({result.Tangent[0], result.Tangent[1], result.Tangent[2]}, result.Normal);
    std::copy(tangent.begin(), tangent.end(), result.Tangent.begin());
    return result;
}

std::array<uint32_t, 3> LoadTriangleCPU(std::span<const uint8_t> bytes, uint32_t triangleIndex)
{
    if (triangleIndex >= Read(bytes, 12))
    {
        throw std::runtime_error("Compressed triangle index out of range");
    }
    uint32_t width = Read(bytes, 20);
    if (width != 1 && width != 2)
    {
        throw std::runtime_error("Unsupported index codec");
    }
    size_t address = Read(bytes, 24) + size_t(triangleIndex)*3*width;
    std::array<uint32_t, 3> result{Read(bytes, address, width), Read(bytes, address + width, width), Read(bytes, address + width*2, width)};
    for (uint32_t index : result)
    {
        if (index >= Read(bytes, 8))
        {
            throw std::runtime_error("Decoded triangle index exceeds VertexCount");
        }
    }
    return result;
}
}
