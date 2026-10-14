#pragma once
#include "virtualgeometryasset.h"
#include <bit>
#include <span>
#include <stdexcept>

namespace VirtualGeometrySerialization
{
    inline constexpr uint32_t Magic = 0x314d4756; // Little-endian "VGM1".
    inline constexpr uint32_t SectionCount = 8;
    inline constexpr uint32_t HeaderBytes = 32 + SectionCount * 16;
    enum Section : uint32_t { Vertices, Triangles, Clusters, Groups, Hierarchy, Roots, Levels, Metadata };
    inline constexpr uint64_t Strides[SectionCount] = {48, 1, 104, 44, 4, 4, 20, 1};

    class Writer
    {
    public:
        vector<uint8_t> Bytes;
        void U32(uint32_t value)
        {
            for (uint32_t byte = 0; byte < 4; ++byte)
            {
                Bytes.push_back(static_cast<uint8_t>(value >> (byte * 8)));
            }
        }
        void U64(uint64_t value)
        {
            for (uint32_t byte = 0; byte < 8; ++byte)
            {
                Bytes.push_back(static_cast<uint8_t>(value >> (byte * 8)));
            }
        }
        void F32(float value) { U32(bit_cast<uint32_t>(value)); }
        void F64(double value) { U64(bit_cast<uint64_t>(value)); }
        void Float3(const array<float, 3>& value)
        {
            for (float component : value)
            {
                F32(component);
            }
        }
        void Sphere(const VirtualGeometrySphere& value)
        {
            Float3(value.Center);
            F32(value.Radius);
        }
    };

    class Reader
    {
        span<const uint8_t> m_Bytes;
        size_t m_Offset = 0;
    public:
        explicit Reader(span<const uint8_t> bytes) : m_Bytes(bytes) {}
        uint32_t U32()
        {
            uint32_t value = 0;
            auto bytes = Take(4);
            for (uint32_t byte = 0; byte < 4; ++byte)
            {
                value |= uint32_t(bytes[byte]) << (byte * 8);
            }
            return value;
        }
        uint64_t U64()
        {
            uint64_t value = 0;
            auto bytes = Take(8);
            for (uint32_t byte = 0; byte < 8; ++byte)
            {
                value |= uint64_t(bytes[byte]) << (byte * 8);
            }
            return value;
        }
        float F32() { return bit_cast<float>(U32()); }
        double F64() { return bit_cast<double>(U64()); }
        span<const uint8_t> Take(size_t count)
        {
            if (count > m_Bytes.size() - m_Offset)
            {
                throw runtime_error("Truncated virtual geometry section");
            }
            auto result = m_Bytes.subspan(m_Offset, count);
            m_Offset += count;
            return result;
        }
        array<float, 3> Float3() { return {F32(), F32(), F32()}; }
        VirtualGeometrySphere Sphere()
        {
            VirtualGeometrySphere sphere;
            sphere.Center = Float3();
            sphere.Radius = F32();
            return sphere;
        }
        void RequireEnd() const
        {
            if (m_Offset != m_Bytes.size())
            {
                throw runtime_error("Unexpected trailing section data");
            }
        }
    };

    void WriteMetadata(Writer& writer, const VirtualGeometryAsset& asset);
    void ReadMetadata(Reader& reader, VirtualGeometryAsset& asset);
}
