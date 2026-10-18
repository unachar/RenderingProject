#include "virtualgeometry/virtualgeometrycompression.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <stdexcept>

namespace
{
using namespace VirtualGeometryCompression;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error("Compression: " + message);
    }
}

std::vector<VirtualGeometryVertex> MakeVertices(float scale, float offset)
{
    std::vector<VirtualGeometryVertex> vertices(64);
    for (uint32_t index = 0; index < vertices.size(); ++index)
    {
        auto& vertex = vertices[index];
        vertex.Position[0] = offset + scale * float(index % 8) / 7;
        vertex.Position[1] = offset + scale * float(index / 8) / 7;
        vertex.Position[2] = offset; // Flat mesh with a degenerate axis.
        vertex.Normal[2] = 1;
        vertex.TexCoord[0] = float(index % 8) / 7;
        vertex.TexCoord[1] = -float(index / 8) / 7;
        std::fill(std::begin(vertex.Diffuse), std::end(vertex.Diffuse), 1.0f);
    }
    return vertices;
}

void PositionCases()
{
    std::string error;
    for (float scale : {1.0f, 1e8f, 1e-12f, 0.0f})
    {
        auto vertices = MakeVertices(scale, -scale * 0.5f);
        Input input;
        input.Vertices = vertices;
        Plan plan;
        VGCompressionSettings settings;
        Require(Select(input, settings, plan, error), error);
        std::vector<uint32_t> indices{0, 1, 8};
        std::vector<uint8_t> bytes, repeated;
        Require(EncodeCluster(input, plan, 0, 64, indices, bytes, error), error);
        Require(EncodeCluster(input, plan, 0, 64, indices, repeated, error), error);
        Require(bytes == repeated, "nondeterministic payload");
        Require(Validate(bytes, 64, 1, error), error);
        for (uint32_t vertex = 0; vertex < 64; ++vertex)
        {
            auto decoded = DecodeVertexCPU(bytes, vertex);
            double squared = 0;
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                double delta = double(decoded.Position[axis]) - vertices[vertex].Position[axis];
                squared += delta * delta;
            }
            Require(std::sqrt(squared) <= settings.MaxPositionError, "position error");
            Require(decoded.Position[2] == vertices[vertex].Position[2], "zero extent axis changed");
        }
        if (scale == 1e8f)
        {
            Require(plan.Streams[0].Encoding == Codec::Float32 && !plan.FallbackReasons.empty(),
                    "large grid did not fall back");
        }
        auto truncated = bytes;
        truncated.pop_back();
        Require(!Validate(truncated, 64, 1, error), "truncated cluster accepted");
        auto damaged = bytes;
        std::fill(damaged.begin() + 64, damaged.begin() + 68, uint8_t(255));
        Require(!Validate(damaged, 64, 1, error), "invalid stream offset accepted");
        damaged = bytes;
        uint32_t indexOffset = uint32_t(bytes[24]) | (uint32_t(bytes[25]) << 8) | (uint32_t(bytes[26]) << 16) |
                               (uint32_t(bytes[27]) << 24);
        damaged[indexOffset] = 255;
        Require(!Validate(damaged, 64, 1, error), "invalid triangle accepted");
    }
}

void BoundaryAndAttributes()
{
    auto vertices = MakeVertices(1, -0.5f);
    // Duplicate source positions at different local indices in two clusters.
    auto duplicate = vertices;
    vertices.insert(vertices.end(), duplicate.begin(), duplicate.begin() + 32);
    Input input;
    input.Vertices = vertices;
    std::vector<std::array<float, 4>> tangents(vertices.size(), {1, 0, 0, -1});
    std::vector<std::array<float, 2>> uv1(vertices.size(), {2.25f, -3.5f});
    std::vector<std::array<float, 2>> uv2(vertices.size(), {100000, -100000});
    input.Tangents = tangents;
    input.Texcoords = {uv1, uv2};
    Plan plan;
    std::string error;
    Require(Select(input, {}, plan, error), error);
    std::vector<uint32_t> indices{0, 1, 2};
    std::vector<uint8_t> first, second;
    Require(EncodeCluster(input, plan, 0, 32, indices, first, error), error);
    Require(EncodeCluster(input, plan, 64, 32, indices, second, error), error);
    for (uint32_t vertex = 0; vertex < 32; ++vertex)
    {
        auto a = DecodeVertexCPU(first, vertex);
        auto b = DecodeVertexCPU(second, vertex);
        Require(a.Position == b.Position, "cluster boundary has a seam");
        Require(a.Normal[2] > 0.999f, "oct normal mismatch");
        Require(a.Tangent[3] == -1 && a.Tangent[0] > 0.999f, "tangent handedness");
        Require(a.Texcoords[0] == uv1[vertex], "out of unit range UV");
        Require(a.Texcoords[1] == uv2[vertex], "half range fallback");
        Require(a.Color == std::array<float, 4>{1, 1, 1, 1}, "vertex color mismatch");
    }
    Require(!plan.FallbackReasons.empty(), "UV fallback missing");
    input.HasColor = false;
    Require(Select(input, {}, plan, error), error);
    Require(std::none_of(plan.Streams.begin(), plan.Streams.end(),
                         [](const auto& stream) {
                             return stream.Attribute == Color;
                         }),
            "absent color allocated a stream");
    vertices[0].TexCoord[0] = std::numeric_limits<float>::infinity();
    input.Texcoords.clear();
    Require(!Select(input, {}, plan, error), "nonfinite input accepted");
}

void IndexCases()
{
    std::string error;
    for (uint32_t count : {255u, 256u, 257u})
    {
        std::vector<VirtualGeometryVertex> vertices(count);
        Input input;
        input.Vertices = vertices;
        Plan plan;
        Require(Select(input, {}, plan, error), error);
        uint32_t expected = 2;
        if (count <= 256)
        {
            expected = 1;
        }
        Require(uint32_t(SelectIndexCodec(count, true)) == expected, "index width threshold");
        std::vector<uint32_t> indices{0, count - 1, 1};
        std::vector<uint8_t> bytes;
        Require(EncodeCluster(input, plan, 0, count, indices, bytes, error), error);
        Require(LoadTriangleCPU(bytes, 0) == std::array<uint32_t, 3>{0, count - 1, 1}, "index roundtrip");
    }
}

void NormalCases()
{
    auto vertices = MakeVertices(1, 0);
    for (uint32_t index = 0; index < vertices.size(); ++index)
    {
        vertices[index].Normal[0] = std::cos(float(index) * 0.7f);
        vertices[index].Normal[1] = std::sin(float(index) * 0.7f);
        vertices[index].Normal[2] = std::cos(float(index) * 0.31f);
    }
    Input input;
    input.Vertices = vertices;
    Plan plan;
    std::string error;
    Require(Select(input, {}, plan, error), error);
    std::vector<uint32_t> indices{0, 1, 2};
    std::vector<uint8_t> bytes;
    Require(EncodeCluster(input, plan, 0, 64, indices, bytes, error), error);
    for (uint32_t index = 0; index < vertices.size(); ++index)
    {
        auto decoded = DecodeVertexCPU(bytes, index);
        const auto& normal = vertices[index].Normal;
        double length =
            std::sqrt(double(normal[0]) * normal[0] + double(normal[1]) * normal[1] + double(normal[2]) * normal[2]);
        double squared = 0;
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            double delta = normal[axis] / length - decoded.Normal[axis];
            squared += delta * delta;
        }
        Require(2 * std::asin(std::min(1.0, std::sqrt(squared) * 0.5)) <= VGCompressionSettings{}.MaxNormalAngularError,
                "oct normal angular error in both hemispheres");
    }
    VGCompressionSettings exact;
    exact.MaxNormalAngularError = 0;
    Require(Select(input, exact, plan, error), error);
    Require(plan.Streams[1].Encoding == Codec::Float32, "normal precision fallback");
}
} // namespace

void TestVirtualGeometryCompression()
{
    PositionCases();
    BoundaryAndAttributes();
    IndexCases();
    NormalCases();
}
