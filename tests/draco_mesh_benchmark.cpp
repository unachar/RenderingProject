#include <draco/compression/decode.h>
#include <draco/compression/encode.h>
#include <draco/mesh/mesh.h>
#include "External/meshoptimizer/src/meshoptimizer.h"
#include "External/zstd/zstd.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace
{
    constexpr unsigned int Grid = 128;

    struct Vertex
    {
        float position[3], normal[3], uv[2];
    };

    static_assert(sizeof(Vertex) == 32);

    struct Input
    {
        std::vector<Vertex> vertices;
        std::vector<unsigned int> indices;
        std::vector<std::byte> raw;
    };

    struct Encoded
    {
        std::vector<char> first, second;
    };

    struct Errors
    {
        double position = 0, normal = 0, uv = 0;
    };

    void Check(bool condition, const char* error)
    {
        if (!condition)
        {
            throw std::runtime_error(error);
        }
    }

    Input MakeInput()
    {
        Input input;
        for (unsigned int row = 0; row <= Grid; ++row)
        {
            for (unsigned int column = 0; column <= Grid; ++column)
            {
                const float x = static_cast<float>(column) / Grid, y = static_cast<float>(row) / Grid;
                Vertex vertex{{x, y, 0.1f * std::sin(x * 6) * std::cos(y * 5)},
                              {-0.6f * std::cos(x * 6) * std::cos(y * 5), 0.5f * std::sin(x * 6) * std::sin(y * 5), 1},
                              {x, y}};
                const float length =
                    std::sqrt(vertex.normal[0] * vertex.normal[0] + vertex.normal[1] * vertex.normal[1] + 1);
                for (auto& value : vertex.normal)
                {
                    value /= length;
                }
                input.vertices.push_back(vertex);
            }
        }
        for (unsigned int row = 0; row < Grid; ++row)
        {
            for (unsigned int column = 0; column < Grid; ++column)
            {
                const auto a = row * (Grid + 1) + column, b = a + 1, c = a + Grid + 1, d = c + 1;
                input.indices.insert(input.indices.end(), {a, b, c, b, d, c});
            }
        }
        const auto vertices = input.vertices.size() * sizeof(Vertex);
        input.raw.resize(vertices + input.indices.size() * sizeof(unsigned int));
        std::memcpy(input.raw.data(), input.vertices.data(), vertices);
        std::memcpy(input.raw.data() + vertices, input.indices.data(), input.raw.size() - vertices);
        return input;
    }

    std::unique_ptr<draco::Mesh> MakeDraco(const Input& input)
    {
        auto mesh = std::make_unique<draco::Mesh>();
        mesh->set_num_points(static_cast<std::uint32_t>(input.vertices.size()));
        mesh->SetNumFaces(input.indices.size() / 3);
        for (std::uint32_t face = 0; face < mesh->num_faces(); ++face)
        {
            mesh->SetFace(draco::FaceIndex(face),
                          {draco::PointIndex(input.indices[face * 3]), draco::PointIndex(input.indices[face * 3 + 1]),
                           draco::PointIndex(input.indices[face * 3 + 2])});
        }
        for (int component = 0; component < 3; ++component)
        {
            const auto type = component == 0   ? draco::GeometryAttribute::POSITION
                              : component == 1 ? draco::GeometryAttribute::NORMAL
                                               : draco::GeometryAttribute::TEX_COORD;
            draco::GeometryAttribute descriptor;
            descriptor.Init(type, nullptr, component == 2 ? 2 : 3, draco::DT_FLOAT32, false,
                            (component == 2 ? 2 : 3) * sizeof(float), 0);
            const auto id = mesh->AddAttribute(descriptor, true, static_cast<std::uint32_t>(input.vertices.size()));
            Check(id >= 0, "Draco attribute allocation");
            for (std::uint32_t point = 0; point < input.vertices.size(); ++point)
            {
                const auto& vertex = input.vertices[point];
                const float* value = component == 0 ? vertex.position : component == 1 ? vertex.normal : vertex.uv;
                mesh->attribute(id)->SetAttributeValue(draco::AttributeValueIndex(point), value);
            }
        }
        return mesh;
    }

    Encoded Encode(const Input& input, int mode)
    {
        Encoded encoded;
        if (mode == 0)
        {
            encoded.first.resize(ZSTD_compressBound(input.raw.size()));
            const auto size =
                ZSTD_compress(encoded.first.data(), encoded.first.size(), input.raw.data(), input.raw.size(), 3);
            Check(!ZSTD_isError(size), "zstd encode");
            encoded.first.resize(size);
        }
        else if (mode == 1)
        {
            encoded.first.resize(meshopt_encodeVertexBufferBound(input.vertices.size(), sizeof(Vertex)));
            const auto vertices =
                meshopt_encodeVertexBuffer(reinterpret_cast<unsigned char*>(encoded.first.data()), encoded.first.size(),
                                           input.vertices.data(), input.vertices.size(), sizeof(Vertex));
            encoded.second.resize(meshopt_encodeIndexBufferBound(input.indices.size(), input.vertices.size()));
            const auto indices =
                meshopt_encodeIndexBuffer(reinterpret_cast<unsigned char*>(encoded.second.data()),
                                          encoded.second.size(), input.indices.data(), input.indices.size());
            Check(vertices && indices, "meshoptimizer encode");
            encoded.first.resize(vertices);
            encoded.second.resize(indices);
        }
        else
        {
            const auto mesh = MakeDraco(input);
            draco::Encoder encoder;
            encoder.SetSpeedOptions(5, 5);
            encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, mode == 2 ? 0 : 14);
            encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, mode == 2 ? 0 : 10);
            encoder.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, mode == 2 ? 0 : 12);
            draco::EncoderBuffer buffer;
            Check(encoder.EncodeMeshToBuffer(*mesh, &buffer).ok(), "Draco encode");
            encoded.first.assign(buffer.data(), buffer.data() + buffer.size());
        }
        return encoded;
    }

    struct Decoded
    {
        std::vector<std::byte> raw;
        std::unique_ptr<draco::Mesh> mesh;
    };

    Decoded Decode(const Input& input, const Encoded& encoded, int mode)
    {
        Decoded result;
        if (mode <= 1)
        {
            result.raw.resize(input.raw.size());
            if (mode == 0)
            {
                Check(ZSTD_decompress(result.raw.data(), result.raw.size(), encoded.first.data(),
                                      encoded.first.size()) == input.raw.size(),
                      "zstd decode");
            }
            else
            {
                Check(meshopt_decodeVertexBuffer(result.raw.data(), input.vertices.size(), sizeof(Vertex),
                                                 reinterpret_cast<const unsigned char*>(encoded.first.data()),
                                                 encoded.first.size()) == 0,
                      "meshoptimizer vertex decode");
                Check(meshopt_decodeIndexBuffer(result.raw.data() + input.vertices.size() * sizeof(Vertex),
                                                input.indices.size(), sizeof(unsigned int),
                                                reinterpret_cast<const unsigned char*>(encoded.second.data()),
                                                encoded.second.size()) == 0,
                      "meshoptimizer index decode");
            }
        }
        else
        {
            draco::DecoderBuffer buffer;
            buffer.Init(encoded.first.data(), encoded.first.size());
            draco::Decoder decoder;
            auto decoded = decoder.DecodeMeshFromBuffer(&buffer);
            Check(decoded.ok(), "Draco decode");
            result.mesh = std::move(decoded).value();
        }
        return result;
    }

    std::array<unsigned int, 3> Canonical(std::array<unsigned int, 3> face)
    {
        const auto first = std::min_element(face.begin(), face.end()) - face.begin();
        return {face[first], face[(first + 1) % 3], face[(first + 2) % 3]};
    }

    Errors Verify(const Input& input, const Decoded& decoded, int mode)
    {
        Errors errors;
        if (mode == 0)
        {
            Check(decoded.raw == input.raw, "lossless byte parity");
            return errors;
        }
        if (mode == 1)
        {
            const auto vertexBytes = input.vertices.size() * sizeof(Vertex);
            Check(std::memcmp(decoded.raw.data(), input.vertices.data(), vertexBytes) == 0,
                  "meshoptimizer vertex byte parity");
            for (std::size_t face = 0; face < input.indices.size() / 3; ++face)
            {
                std::array<unsigned int, 3> actual;
                std::memcpy(actual.data(), decoded.raw.data() + vertexBytes + face * sizeof(actual), sizeof(actual));
                Check(Canonical(actual) == Canonical({input.indices[face * 3], input.indices[face * 3 + 1],
                                                      input.indices[face * 3 + 2]}),
                      "meshoptimizer oriented triangle parity");
            }
            return errors;
        }
        const auto& mesh = *decoded.mesh;
        Check(mesh.num_faces() * 3 == input.indices.size(), "Draco face count parity");
        const auto position = mesh.GetNamedAttribute(draco::GeometryAttribute::POSITION);
        const auto normal = mesh.GetNamedAttribute(draco::GeometryAttribute::NORMAL);
        const auto uv = mesh.GetNamedAttribute(draco::GeometryAttribute::TEX_COORD);
        Check(position && normal && uv, "Draco decoded surface attributes");
        std::vector<unsigned int> ids(mesh.num_points());
        for (std::uint32_t point = 0; point < mesh.num_points(); ++point)
        {
            float p[3], n[3], t[2];
            const auto index = draco::PointIndex(point);
            Check(position->ConvertValue(position->mapped_index(index), 3, p) &&
                      normal->ConvertValue(normal->mapped_index(index), 3, n) &&
                      uv->ConvertValue(uv->mapped_index(index), 2, t),
                  "Draco point attribute conversion");
            const int column = std::lround(p[0] * Grid), row = std::lround(p[1] * Grid);
            Check(column >= 0 && column <= Grid && row >= 0 && row <= Grid, "Draco grid correspondence");
            ids[point] = row * (Grid + 1) + column;
            const auto& expected = input.vertices[ids[point]];
            for (int component = 0; component < 3; ++component)
            {
                errors.position = std::max(errors.position,
                                           std::abs(static_cast<double>(p[component]) - expected.position[component]));
                errors.normal =
                    std::max(errors.normal, std::abs(static_cast<double>(n[component]) - expected.normal[component]));
            }
            for (int component = 0; component < 2; ++component)
            {
                errors.uv = std::max(errors.uv, std::abs(static_cast<double>(t[component]) - expected.uv[component]));
            }
        }
        std::vector<std::array<unsigned int, 3>> actual, expected;
        for (std::uint32_t face = 0; face < mesh.num_faces(); ++face)
        {
            const auto& f = mesh.face(draco::FaceIndex(face));
            actual.push_back(Canonical({ids.at(f[0].value()), ids.at(f[1].value()), ids.at(f[2].value())}));
            expected.push_back(
                Canonical({input.indices[face * 3], input.indices[face * 3 + 1], input.indices[face * 3 + 2]}));
        }
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        Check(actual == expected, "Draco oriented topology parity");
        if (mode == 2)
        {
            Check(errors.position == 0 && errors.normal == 0 && errors.uv == 0, "Draco lossless float parity");
        }
        else
        {
            Check(errors.position < 0.0001 && errors.normal < 0.02 && errors.uv < 0.001,
                  "Draco quantization error budget");
        }
        return errors;
    }
}

int main()
{
    try
    {
        using Clock = std::chrono::steady_clock;
        const auto input = MakeInput();
        const char* names[]{"zstd-3", "meshoptimizer", "draco-lossless", "draco-14-10-12"};
        std::cout << "{\"vertices\":" << input.vertices.size() << ",\"triangles\":" << input.indices.size() / 3
                  << ",\"rawBytes\":" << input.raw.size() << ",\"passes\":[\n";
        bool first = true;
        for (int pass = 0; pass < 5; ++pass)
        {
            for (int step = 0; step < 4; ++step)
            {
                const int mode = (pass + step) % 4;
                auto start = Clock::now();
                const auto encoded = Encode(input, mode);
                const auto encodeMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                start = Clock::now();
                const auto decoded = Decode(input, encoded, mode);
                const auto decodeMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                const auto errors = Verify(input, decoded, mode);
                if (!first)
                {
                    std::cout << ",\n";
                }
                first = false;
                std::cout << "{\"codec\":\"" << names[mode]
                          << "\",\"bytes\":" << encoded.first.size() + encoded.second.size()
                          << ",\"encodeMs\":" << encodeMs << ",\"decodeMs\":" << decodeMs
                          << ",\"maxPositionComponentError\":" << errors.position
                          << ",\"maxNormalComponentError\":" << errors.normal
                          << ",\"maxUvComponentError\":" << errors.uv << '}';
            }
        }
        std::cout << "\n]}\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
