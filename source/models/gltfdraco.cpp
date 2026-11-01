#include "gltfdraco.h"
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

#ifndef RENDERINGPROJECT_USE_DRACO
#define RENDERINGPROJECT_USE_DRACO 0
#endif

#if RENDERINGPROJECT_USE_DRACO
#include <draco/compression/decode.h>
#include <draco/mesh/mesh.h>
#endif

namespace
{
    constexpr std::size_t MaxBytes = 512u * 1024u * 1024u;
    constexpr std::size_t MaxElements = 8u * 1024u * 1024u;

    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

#if RENDERINGPROJECT_USE_DRACO
    std::size_t Append(fastgltf::Asset& asset, fastgltf::Accessor accessor, std::vector<std::byte> bytes,
                       std::size_t& residentBytes)
    {
        Require(bytes.size() <= MaxBytes - residentBytes, "Draco expanded buffer limit");
        residentBytes += bytes.size();
        fastgltf::Buffer buffer;
        buffer.byteLength = bytes.size();
        buffer.data = fastgltf::sources::Vector{std::move(bytes)};
        fastgltf::BufferView view;
        view.bufferIndex = asset.buffers.size();
        view.byteLength = buffer.byteLength;
        accessor.bufferViewIndex = asset.bufferViews.size();
        accessor.byteOffset = 0;
        accessor.sparse.reset();
        asset.buffers.push_back(std::move(buffer));
        asset.bufferViews.push_back(std::move(view));
        const auto index = asset.accessors.size();
        asset.accessors.push_back(std::move(accessor));
        return index;
    }

    template <typename T>
    std::vector<std::byte> DecodeAttribute(const draco::Mesh& mesh, const draco::PointAttribute& attribute,
                                           std::size_t components)
    {
        const auto stride = components * sizeof(T);
        Require(mesh.num_points() <= MaxBytes / stride, "Draco attribute size limit");
        std::vector<std::byte> bytes(mesh.num_points() * stride);
        for (std::uint32_t point = 0; point < mesh.num_points(); ++point)
        {
            const auto index = attribute.mapped_index(draco::PointIndex(point));
            Require(index.value() < attribute.size(), "Draco attribute mapping out of range");
            std::array<T, 4> value{};
            Require(attribute.ConvertValue(index, static_cast<std::int8_t>(components), value.data()),
                    "Draco attribute conversion failed");
            std::memcpy(bytes.data() + point * stride, value.data(), stride);
        }
        return bytes;
    }

    std::vector<std::byte> DecodeAttribute(const draco::Mesh& mesh, const draco::PointAttribute& attribute,
                                           const fastgltf::Accessor& accessor)
    {
        const auto components = fastgltf::getNumComponents(accessor.type);
        Require(components > 0 && components <= 4 && attribute.num_components() == components,
                "Draco attribute component count mismatch");
        // Preserve the glTF component type and apply accessor.normalized only once, in fastgltf.
        switch (accessor.componentType)
        {
            case fastgltf::ComponentType::Float:
                Require(attribute.data_type() == draco::DT_FLOAT32, "Draco float attribute type mismatch");
                return DecodeAttribute<float>(mesh, attribute, components);
            case fastgltf::ComponentType::UnsignedByte:
                Require(attribute.data_type() == draco::DT_UINT8, "Draco uint8 attribute type mismatch");
                return DecodeAttribute<std::uint8_t>(mesh, attribute, components);
            case fastgltf::ComponentType::UnsignedShort:
                Require(attribute.data_type() == draco::DT_UINT16, "Draco uint16 attribute type mismatch");
                return DecodeAttribute<std::uint16_t>(mesh, attribute, components);
            case fastgltf::ComponentType::UnsignedInt:
                Require(attribute.data_type() == draco::DT_UINT32, "Draco uint32 attribute type mismatch");
                return DecodeAttribute<std::uint32_t>(mesh, attribute, components);
            case fastgltf::ComponentType::Byte:
                Require(attribute.data_type() == draco::DT_INT8, "Draco int8 attribute type mismatch");
                return DecodeAttribute<std::int8_t>(mesh, attribute, components);
            case fastgltf::ComponentType::Short:
                Require(attribute.data_type() == draco::DT_INT16, "Draco int16 attribute type mismatch");
                return DecodeAttribute<std::int16_t>(mesh, attribute, components);
            default:
                throw std::runtime_error("Unsupported Draco component type");
        }
    }

    void ExpandPrimitive(fastgltf::Asset& asset, fastgltf::Primitive& primitive, std::size_t& residentBytes)
    {
        const auto& compression = *primitive.dracoCompression;
        Require(primitive.type == fastgltf::PrimitiveType::Triangles, "Draco mesh is not triangles");
        const auto position = primitive.findAttribute("POSITION");
        Require(position != primitive.attributes.end(), "Draco primitive has no positions");
        const auto declaredPoints = asset.accessors.at(position->accessorIndex).count;
        Require(declaredPoints > 0 && declaredPoints <= MaxElements, "Draco declared vertex limit");
        Require(compression.bufferView < asset.bufferViews.size(), "Draco compressed buffer view out of range");
        const auto bytes = fastgltf::DefaultBufferDataAdapter{}(asset, compression.bufferView);
        Require(!bytes.empty() && bytes.size() <= MaxBytes, "Draco compressed buffer limit");
        draco::DecoderBuffer input;
        input.Init(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        draco::Decoder decoder;
        auto result = decoder.DecodeMeshFromBuffer(&input);
        Require(result.ok(), "Draco mesh decode failed");
        auto mesh = std::move(result).value();
        Require(mesh && mesh->num_points() == declaredPoints, "Draco decoded vertex count mismatch");
        Require(mesh->num_faces() > 0 && mesh->num_faces() <= MaxElements / 3, "Draco decoded triangle limit");
        std::unordered_set<std::uint32_t> ids;
        for (const auto& mapping : compression.attributes)
        {
            const auto output = primitive.findAttribute(mapping.name);
            Require(output != primitive.attributes.end(), "Draco attribute is not declared in primitive");
            Require(mapping.accessorIndex <= UINT32_MAX &&
                        ids.insert(static_cast<std::uint32_t>(mapping.accessorIndex)).second,
                    "Duplicate or oversized Draco attribute ID");
            const auto attribute = mesh->GetAttributeByUniqueId(static_cast<std::uint32_t>(mapping.accessorIndex));
            Require(attribute != nullptr, "Draco unique attribute ID not found");
            if (mapping.name == "POSITION")
            {
                Require(attribute->attribute_type() == draco::GeometryAttribute::POSITION,
                        "Draco position semantic mismatch");
            }
            if (mapping.name == "NORMAL")
            {
                Require(attribute->attribute_type() == draco::GeometryAttribute::NORMAL,
                        "Draco normal semantic mismatch");
            }
            const auto& declared = asset.accessors.at(output->accessorIndex);
            fastgltf::Accessor accessor;
            accessor.type = declared.type;
            accessor.componentType = declared.componentType;
            accessor.normalized = declared.normalized;
            accessor.count = declared.count;
            accessor.name = declared.name;
            auto copyBounds = [](const auto& source, auto& destination) {
                if (!source)
                {
                    return;
                }
                if (source->template isType<double>())
                {
                    auto bounds = fastgltf::AccessorBoundsArray::ForType<double>(source->size());
                    for (std::size_t i = 0; i < source->size(); ++i)
                    {
                        bounds.template set<double>(i, source->template get<double>(i));
                    }
                    destination = std::move(bounds);
                }
                else
                {
                    auto bounds = fastgltf::AccessorBoundsArray::ForType<std::int64_t>(source->size());
                    for (std::size_t i = 0; i < source->size(); ++i)
                    {
                        bounds.template set<std::int64_t>(i, source->template get<std::int64_t>(i));
                    }
                    destination = std::move(bounds);
                }
            };
            copyBounds(declared.min, accessor.min);
            copyBounds(declared.max, accessor.max);
            Require(accessor.count == declaredPoints, "Draco attribute vertex count mismatch");
            const auto element = fastgltf::getElementByteSize(accessor.type, accessor.componentType);
            Require(element > 0 && declaredPoints <= (MaxBytes - residentBytes) / element,
                    "Draco expanded attribute budget");
            auto decoded = DecodeAttribute(*mesh, *attribute, accessor);
            output->accessorIndex = Append(asset, std::move(accessor), std::move(decoded), residentBytes);
        }
        Require(compression.findAttribute("POSITION") != compression.attributes.end(),
                "Draco positions are not mapped");
        fastgltf::Accessor indices;
        if (primitive.indicesAccessor)
        {
            const auto& declared = asset.accessors.at(*primitive.indicesAccessor);
            Require(declared.type == fastgltf::AccessorType::Scalar && declared.count == mesh->num_faces() * 3,
                    "Draco index count mismatch");
        }
        indices.type = fastgltf::AccessorType::Scalar;
        indices.componentType = fastgltf::ComponentType::UnsignedInt;
        indices.normalized = false;
        indices.count = mesh->num_faces() * 3;
        indices.min.reset();
        indices.max.reset();
        Require(indices.count <= (MaxBytes - residentBytes) / sizeof(std::uint32_t), "Draco expanded index budget");
        std::vector<std::byte> faces(indices.count * sizeof(std::uint32_t));
        for (std::uint32_t face = 0; face < mesh->num_faces(); ++face)
        {
            for (std::uint32_t corner = 0; corner < 3; ++corner)
            {
                const auto point = mesh->face(draco::FaceIndex(face))[corner].value();
                Require(point < declaredPoints, "Draco index exceeds vertex count");
                std::memcpy(faces.data() + (face * 3 + corner) * sizeof(point), &point, sizeof(point));
            }
        }
        primitive.indicesAccessor = Append(asset, std::move(indices), std::move(faces), residentBytes);
        primitive.dracoCompression.reset();
    }
#endif
}

namespace GltfDraco
{
    bool Expand(fastgltf::Asset& asset)
    {
        bool expanded = false;
#if RENDERINGPROJECT_USE_DRACO
        std::size_t residentBytes = 0;
        for (const auto& buffer : asset.buffers)
        {
            const auto bytes = std::visit(
                [](const auto& value) -> std::size_t {
                    if constexpr (requires { value.bytes.size(); })
                    {
                        return value.bytes.size();
                    }
                    else
                    {
                        throw std::runtime_error("Draco source buffer is not resident");
                    }
                },
                buffer.data);
            Require(bytes <= MaxBytes - residentBytes, "Draco resident buffer limit");
            residentBytes += bytes;
        }
#endif
        for (auto& mesh : asset.meshes)
        {
            for (auto& primitive : mesh.primitives)
            {
                if (primitive.dracoCompression)
                {
#if RENDERINGPROJECT_USE_DRACO
                    ExpandPrimitive(asset, primitive, residentBytes);
                    expanded = true;
#else
                    throw std::runtime_error("Draco SDK is disabled");
#endif
                }
            }
        }
        auto removeDraco = [](auto& extensions) {
            std::erase(extensions, "KHR_draco_mesh_compression");
        };
        removeDraco(asset.extensionsUsed);
        removeDraco(asset.extensionsRequired);
        return expanded;
    }
}
