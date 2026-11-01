#include "fastgltfimport.h"
#include "gltfdraco.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef RENDERINGPROJECT_USE_FASTGLTF
#define RENDERINGPROJECT_USE_FASTGLTF 0
#endif

#ifndef FASTGLTF_SHARED_CRT_SCENE
#define FASTGLTF_SHARED_CRT_SCENE 0
#endif
#if FASTGLTF_SHARED_CRT_SCENE && (!defined(_DLL) || defined(_DEBUG))
#error Direct Assimp scene ownership requires the shared Release CRT.
#endif

#if RENDERINGPROJECT_USE_FASTGLTF
#include "BaseImporter.h"
#include "Importer.hpp"
#include "SceneCombiner.h"
#include "scene.h"
#include "material.h"
#include "GltfMaterial.h"
#include "importerdesc.h"
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

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

    // Static-CRT builds retain a local graph and give Assimp a deep copy.
    // Shared-CRT builds transfer the graph without changing the application DLL boundary.
    void DeleteLocalNode(aiNode* node)
    {
        if (!node)
        {
            return;
        }
        for (unsigned int i = 0; i < node->mNumChildren; ++i)
        {
            DeleteLocalNode(node->mChildren[i]);
        }
        delete[] node->mChildren;
        delete[] node->mMeshes;
        node->mChildren = nullptr;
        node->mMeshes = nullptr;
        node->mNumChildren = node->mNumMeshes = 0;
        delete node;
    }

#if FASTGLTF_SHARED_CRT_SCENE
    using LocalMesh = std::unique_ptr<aiMesh>;
#else
    struct LocalMeshDeleter
    {
        void operator()(aiMesh* mesh) const noexcept
        {
            if (!mesh)
            {
                return;
            }
            // Temporary meshes share one index allocation. The Assimp-owned deep copy
            // uses its usual per-face allocations and never uses this deleter.
            unsigned int* indices = mesh->mFaces && mesh->mNumFaces ? mesh->mFaces[0].mIndices : nullptr;
            for (unsigned int face = 0; face < mesh->mNumFaces; ++face)
            {
                mesh->mFaces[face].mIndices = nullptr;
                mesh->mFaces[face].mNumIndices = 0;
            }
            delete[] indices;
            delete mesh;
        }
    };

    using LocalMesh = std::unique_ptr<aiMesh, LocalMeshDeleter>;
#endif

    struct LocalScene
    {
        aiScene scene;

#if FASTGLTF_SHARED_CRT_SCENE
        void MoveTo(aiScene& destination)
        {
            Require(!destination.mRootNode && !destination.mMeshes && !destination.mMaterials &&
                        !destination.mAnimations && !destination.mTextures,
                    "Import target is not empty");
            // Transfer only fields populated by Converter. Assimp's private state
            // remains attached to its original scene object.
            std::swap(destination.mFlags, scene.mFlags);
            std::swap(destination.mRootNode, scene.mRootNode);
            std::swap(destination.mNumMeshes, scene.mNumMeshes);
            std::swap(destination.mMeshes, scene.mMeshes);
            std::swap(destination.mNumMaterials, scene.mNumMaterials);
            std::swap(destination.mMaterials, scene.mMaterials);
            std::swap(destination.mNumAnimations, scene.mNumAnimations);
            std::swap(destination.mAnimations, scene.mAnimations);
            std::swap(destination.mNumTextures, scene.mNumTextures);
            std::swap(destination.mTextures, scene.mTextures);
        }
#else
        ~LocalScene()
        {
            DeleteLocalNode(scene.mRootNode);
            scene.mRootNode = nullptr;
            for (unsigned int i = 0; i < scene.mNumMeshes; ++i)
            {
                LocalMeshDeleter{}(scene.mMeshes[i]);
            }
            delete[] scene.mMeshes;
            scene.mMeshes = nullptr;
            scene.mNumMeshes = 0;
            for (unsigned int i = 0; i < scene.mNumMaterials; ++i)
            {
                delete scene.mMaterials[i];
            }
            delete[] scene.mMaterials;
            scene.mMaterials = nullptr;
            scene.mNumMaterials = 0;
            for (unsigned int i = 0; i < scene.mNumTextures; ++i)
            {
                delete scene.mTextures[i];
            }
            delete[] scene.mTextures;
            scene.mTextures = nullptr;
            scene.mNumTextures = 0;
            for (unsigned int i = 0; i < scene.mNumAnimations; ++i)
            {
                delete scene.mAnimations[i];
            }
            delete[] scene.mAnimations;
            scene.mAnimations = nullptr;
            scene.mNumAnimations = 0;
        }
#endif
    };

    std::span<const std::byte> Bytes(const fastgltf::DataSource& source)
    {
        return std::visit(
            [](const auto& value) -> std::span<const std::byte> {
                if constexpr (requires {
                                  value.bytes.data();
                                  value.bytes.size();
                              })
                {
                    return {value.bytes.data(), value.bytes.size()};
                }
                else
                {
                    throw std::runtime_error("Buffer data is not resident");
                }
            },
            source);
    }

    void CheckRange(std::size_t offset, std::size_t count, std::size_t stride, std::size_t element, std::size_t size)
    {
        Require(count > 0 && count <= MaxElements && stride >= element && offset <= size && element <= size - offset,
                "Invalid accessor span");
        Require(count - 1 <= (size - offset - element) / stride, "Accessor exceeds buffer view");
    }

    void ValidateBuffers(const fastgltf::Asset& asset)
    {
        std::size_t total = 0;
        for (const auto& buffer : asset.buffers)
        {
            const auto bytes = Bytes(buffer.data);
            Require(buffer.byteLength <= bytes.size() && bytes.size() <= MaxBytes - total, "Buffer size limit");
            total += bytes.size();
        }
        for (const auto& view : asset.bufferViews)
        {
            Require(view.bufferIndex < asset.buffers.size() && !view.meshoptCompression, "Unsupported buffer view");
            const auto size = asset.buffers[view.bufferIndex].byteLength;
            Require(view.byteOffset <= size && view.byteLength <= size - view.byteOffset, "Buffer view exceeds buffer");
        }
        for (const auto& accessor : asset.accessors)
        {
            const auto element = fastgltf::getElementByteSize(accessor.type, accessor.componentType);
            Require(element > 0 && accessor.count > 0 && accessor.count <= MaxElements, "Invalid accessor");
            if (accessor.bufferViewIndex)
            {
                const auto& view = asset.bufferViews.at(*accessor.bufferViewIndex);
                CheckRange(accessor.byteOffset, accessor.count, view.byteStride.value_or(element), element,
                           view.byteLength);
            }
            if (accessor.sparse)
            {
                const auto& sparse = *accessor.sparse;
                Require(sparse.count > 0 && sparse.count <= accessor.count, "Invalid sparse count");
                const auto& indices = asset.bufferViews.at(sparse.indicesBufferView);
                const auto& values = asset.bufferViews.at(sparse.valuesBufferView);
                const auto width = fastgltf::getComponentByteSize(sparse.indexComponentType);
                Require(width == 1 || width == 2 || width == 4, "Invalid sparse index type");
                CheckRange(sparse.indicesByteOffset, sparse.count, width, width, indices.byteLength);
                CheckRange(sparse.valuesByteOffset, sparse.count, element, element, values.byteLength);
                const auto bytes = Bytes(asset.buffers[indices.bufferIndex].data);
                std::uint32_t previous = 0;
                for (std::size_t i = 0; i < sparse.count; ++i)
                {
                    std::uint32_t index = 0;
                    std::memcpy(&index, bytes.data() + indices.byteOffset + sparse.indicesByteOffset + i * width,
                                width);
                    Require(index < accessor.count && (i == 0 || index > previous), "Invalid sparse index");
                    previous = index;
                }
            }
        }
    }

    void LoadBuffers(fastgltf::Asset& asset, const std::filesystem::path& directory)
    {
        std::size_t total = 0;
        for (auto& buffer : asset.buffers)
        {
            Require(buffer.byteLength > 0 && buffer.byteLength <= MaxBytes - total, "Buffer size limit");
            total += buffer.byteLength;
            if (const auto uri = std::get_if<fastgltf::sources::URI>(&buffer.data))
            {
                Require(uri->uri->isLocalPath() && uri->fileByteOffset == 0, "Unsupported buffer URI");
                const auto path = directory / uri->uri->fspath();
                const auto fileSize = std::filesystem::file_size(path);
                Require(fileSize >= buffer.byteLength, "Truncated external buffer");
                fastgltf::sources::Vector resident;
                resident.bytes.resize(buffer.byteLength);
                std::ifstream stream(path, std::ios::binary);
                Require(static_cast<bool>(stream.read(reinterpret_cast<char*>(resident.bytes.data()),
                                                      static_cast<std::streamsize>(resident.bytes.size()))),
                        "Cannot read external buffer");
                buffer.data = std::move(resident);
            }
        }
    }

    aiMatrix4x4 Matrix(const fastgltf::math::fmat4x4& source)
    {
        aiMatrix4x4 result;
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                Require(std::isfinite(source[col][row]), "Nonfinite matrix");
                result[row][col] = source[col][row];
            }
        }
        return result;
    }

    template <typename T, typename Callback>
    void ReadAttribute(const fastgltf::Asset& asset, std::size_t index, fastgltf::AccessorType type, std::size_t count,
                       Callback&& callback)
    {
        const auto& accessor = asset.accessors.at(index);
        Require(accessor.type == type && accessor.count == count, "Attribute shape mismatch");
        std::size_t element = 0;
        fastgltf::iterateAccessor<T>(asset, accessor, [&](T value) {
            callback(element++, value);
        });
    }

    template <typename T>
    std::vector<T> Attribute(const fastgltf::Asset& asset, std::size_t index, fastgltf::AccessorType type,
                             std::size_t count)
    {
        std::vector<T> result;
        result.reserve(count);
        ReadAttribute<T>(asset, index, type, count, [&](std::size_t, T value) {
            result.push_back(value);
        });
        return result;
    }

    aiVector3D Vector(const fastgltf::math::fvec3& value)
    {
        Require(std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]), "Nonfinite vertex data");
        return {value[0], value[1], value[2]};
    }

    LocalMesh MakeMesh(const fastgltf::Asset& asset, const fastgltf::Mesh& source, const fastgltf::Primitive& primitive)
    {
        Require(primitive.type == fastgltf::PrimitiveType::Triangles && !primitive.dracoCompression,
                "Primitive requires a topology/compression adapter");
        LocalMesh mesh(new aiMesh());
        mesh->mName.Set(source.name);
        mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        mesh->mMaterialIndex = static_cast<unsigned int>(primitive.materialIndex.value_or(asset.materials.size()));
        const auto position = primitive.findAttribute("POSITION");
        Require(position != primitive.attributes.end(), "Missing positions");
        const auto count = asset.accessors.at(position->accessorIndex).count;
        mesh->mVertices = new aiVector3D[count];
        mesh->mNumVertices = static_cast<unsigned int>(count);
        ReadAttribute<fastgltf::math::fvec3>(asset, position->accessorIndex, fastgltf::AccessorType::Vec3, count,
                                             [&](std::size_t i, const auto& value) {
                                                 mesh->mVertices[i] = Vector(value);
                                             });
        const auto normal = primitive.findAttribute("NORMAL");
        if (normal != primitive.attributes.end())
        {
            mesh->mNormals = new aiVector3D[count];
            ReadAttribute<fastgltf::math::fvec3>(asset, normal->accessorIndex, fastgltf::AccessorType::Vec3, count,
                                                 [&](std::size_t i, const auto& value) {
                                                     mesh->mNormals[i] = Vector(value);
                                                 });
        }
        const auto tangent = primitive.findAttribute("TANGENT");
        if (tangent != primitive.attributes.end())
        {
            Require(mesh->HasNormals(), "Tangents require normals");
            mesh->mTangents = new aiVector3D[count];
            mesh->mBitangents = new aiVector3D[count];
            ReadAttribute<fastgltf::math::fvec4>(
                asset, tangent->accessorIndex, fastgltf::AccessorType::Vec4, count,
                [&](std::size_t i, const auto& value) {
                    mesh->mTangents[i] = Vector({value[0], value[1], value[2]});
                    Require(value[3] == 1.0f || value[3] == -1.0f, "Invalid tangent handedness");
                    mesh->mBitangents[i] = (mesh->mNormals[i] ^ mesh->mTangents[i]) * value[3];
                });
        }
        for (unsigned int set = 0; set < AI_MAX_NUMBER_OF_COLOR_SETS; ++set)
        {
            const auto color = primitive.findAttribute("COLOR_" + std::to_string(set));
            if (color == primitive.attributes.end())
            {
                continue;
            }
            const auto& accessor = asset.accessors.at(color->accessorIndex);
            Require(accessor.count == count && (accessor.type == fastgltf::AccessorType::Vec3 ||
                                                accessor.type == fastgltf::AccessorType::Vec4),
                    "Invalid vertex colors");
            mesh->mColors[set] = new aiColor4D[count];
            std::size_t i = 0;
            if (accessor.type == fastgltf::AccessorType::Vec3)
            {
                fastgltf::iterateAccessor<fastgltf::math::fvec3>(asset, accessor, [&](auto value) {
                    const auto rgb = Vector(value);
                    mesh->mColors[set][i++] = {rgb.x, rgb.y, rgb.z, 1};
                });
            }
            else
            {
                fastgltf::iterateAccessor<fastgltf::math::fvec4>(asset, accessor, [&](auto value) {
                    const auto rgb = Vector({value[0], value[1], value[2]});
                    Require(std::isfinite(value[3]), "Invalid color alpha");
                    mesh->mColors[set][i++] = {rgb.x, rgb.y, rgb.z, value[3]};
                });
            }
        }
        for (unsigned int set = 0; set < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++set)
        {
            const auto uv = primitive.findAttribute("TEXCOORD_" + std::to_string(set));
            if (uv == primitive.attributes.end())
            {
                continue;
            }
            mesh->mTextureCoords[set] = new aiVector3D[count];
            mesh->mNumUVComponents[set] = 2;
            ReadAttribute<fastgltf::math::fvec2>(
                asset, uv->accessorIndex, fastgltf::AccessorType::Vec2, count, [&](std::size_t i, const auto& value) {
                    Require(std::isfinite(value[0]) && std::isfinite(value[1]), "Nonfinite UV");
                    mesh->mTextureCoords[set][i] = {value[0], 1.0f - value[1], 0};
                });
        }
        for (const auto& attribute : primitive.attributes)
        {
            bool supported = attribute.name == "POSITION" || attribute.name == "NORMAL" || attribute.name == "TANGENT";
            for (unsigned int set = 0; set < 8; ++set)
            {
                supported = supported || attribute.name == "TEXCOORD_" + std::to_string(set) ||
                            attribute.name == "COLOR_" + std::to_string(set) ||
                            attribute.name == "JOINTS_" + std::to_string(set) ||
                            attribute.name == "WEIGHTS_" + std::to_string(set);
            }
            Require(supported, "Vertex attribute adapter is not available");
        }
        std::vector<std::uint32_t> indices;
        if (primitive.indicesAccessor)
        {
            const auto& accessor = asset.accessors.at(*primitive.indicesAccessor);
            Require(accessor.type == fastgltf::AccessorType::Scalar && !accessor.normalized, "Invalid index accessor");
            Require(accessor.componentType == fastgltf::ComponentType::UnsignedByte ||
                        accessor.componentType == fastgltf::ComponentType::UnsignedShort ||
                        accessor.componentType == fastgltf::ComponentType::UnsignedInt,
                    "Invalid index component type");
            fastgltf::iterateAccessor<std::uint32_t>(asset, accessor, [&](std::uint32_t value) {
                Require(value < count, "Index exceeds vertex count");
                indices.push_back(value);
            });
        }
        else
        {
            indices.resize(count);
            std::iota(indices.begin(), indices.end(), 0u);
        }
        Require(!indices.empty() && indices.size() % 3 == 0, "Triangle index count");
        mesh->mFaces = new aiFace[indices.size() / 3];
        mesh->mNumFaces = static_cast<unsigned int>(indices.size() / 3);
#if FASTGLTF_SHARED_CRT_SCENE
        for (unsigned int i = 0; i < mesh->mNumFaces; ++i)
        {
            mesh->mFaces[i].mIndices = new unsigned int[3]{indices[i * 3], indices[i * 3 + 1], indices[i * 3 + 2]};
            mesh->mFaces[i].mNumIndices = 3;
        }
#else
        std::unique_ptr<unsigned int[]> faceIndices(new unsigned int[indices.size()]);
        std::copy(indices.begin(), indices.end(), faceIndices.get());
        for (unsigned int i = 0; i < mesh->mNumFaces; ++i)
        {
            mesh->mFaces[i].mIndices = faceIndices.get() + i * 3;
            mesh->mFaces[i].mNumIndices = 3;
        }
        faceIndices.release();
#endif
        mesh->mAnimMeshes = new aiAnimMesh* [primitive.targets.size()] {
        };
        mesh->mNumAnimMeshes = static_cast<unsigned int>(primitive.targets.size());
        for (std::size_t target = 0; target < primitive.targets.size(); ++target)
        {
            auto morph = std::make_unique<aiAnimMesh>();
            morph->mNumVertices = mesh->mNumVertices;
            morph->mWeight = target < source.weights.size() ? source.weights[target] : 0;
            for (const auto& attribute : primitive.targets[target])
            {
                aiVector3D* base = attribute.name == "POSITION"  ? mesh->mVertices
                                   : attribute.name == "NORMAL"  ? mesh->mNormals
                                   : attribute.name == "TANGENT" ? mesh->mTangents
                                                                 : nullptr;
                Require(base != nullptr, "Unsupported morph attribute");
                aiVector3D** destination = attribute.name == "POSITION" ? &morph->mVertices
                                           : attribute.name == "NORMAL" ? &morph->mNormals
                                                                        : &morph->mTangents;
                *destination = new aiVector3D[count];
                ReadAttribute<fastgltf::math::fvec3>(asset, attribute.accessorIndex, fastgltf::AccessorType::Vec3,
                                                     count, [&](std::size_t i, const auto& value) {
                                                         (*destination)[i] = base[i] + Vector(value);
                                                     });
            }
            mesh->mAnimMeshes[target] = morph.release();
        }
        return mesh;
    }

    std::string NodeName(const fastgltf::Asset& asset, std::size_t index)
    {
        const auto& name = asset.nodes.at(index).name;
        return name.empty() ? "nodes[" + std::to_string(index) + "]" : name;
    }

    void Skin(aiMesh& mesh, const fastgltf::Asset& asset, const fastgltf::Primitive& primitive,
              const fastgltf::Skin& skin)
    {
        Require(!skin.joints.empty() && skin.joints.size() <= 4096, "Skin joint count limit");
        std::vector<std::vector<aiVertexWeight>> weights(skin.joints.size());
        bool foundSet = false;
        for (unsigned int set = 0; set < 8; ++set)
        {
            const auto joint = primitive.findAttribute("JOINTS_" + std::to_string(set));
            const auto weight = primitive.findAttribute("WEIGHTS_" + std::to_string(set));
            Require((joint == primitive.attributes.end()) == (weight == primitive.attributes.end()),
                    "Unpaired skin attributes");
            if (joint == primitive.attributes.end())
            {
                continue;
            }
            foundSet = true;
            const auto joints = Attribute<fastgltf::math::uvec4>(asset, joint->accessorIndex,
                                                                 fastgltf::AccessorType::Vec4, mesh.mNumVertices);
            const auto values = Attribute<fastgltf::math::fvec4>(asset, weight->accessorIndex,
                                                                 fastgltf::AccessorType::Vec4, mesh.mNumVertices);
            for (unsigned int vertex = 0; vertex < mesh.mNumVertices; ++vertex)
            {
                for (int component = 0; component < 4; ++component)
                {
                    const float value = values[vertex][component];
                    Require(joints[vertex][component] < skin.joints.size() && std::isfinite(value) && value >= 0,
                            "Invalid skin weight");
                    if (value > 0)
                    {
                        weights[joints[vertex][component]].emplace_back(vertex, value);
                    }
                }
            }
        }
        Require(foundSet, "Skin has no weights");
        std::vector<fastgltf::math::fmat4x4> inverse;
        if (skin.inverseBindMatrices)
        {
            inverse = Attribute<fastgltf::math::fmat4x4>(asset, *skin.inverseBindMatrices, fastgltf::AccessorType::Mat4,
                                                         skin.joints.size());
        }
        mesh.mBones = new aiBone* [skin.joints.size()] {
        };
        for (std::size_t joint = 0; joint < skin.joints.size(); ++joint)
        {
            if (weights[joint].empty())
            {
                continue;
            }
            auto bone = std::make_unique<aiBone>();
            bone->mName.Set(NodeName(asset, skin.joints[joint]));
            if (!inverse.empty())
            {
                bone->mOffsetMatrix = Matrix(inverse[joint]);
            }
            bone->mWeights = new aiVertexWeight[weights[joint].size()];
            bone->mNumWeights = static_cast<unsigned int>(weights[joint].size());
            std::copy(weights[joint].begin(), weights[joint].end(), bone->mWeights);
            mesh.mBones[mesh.mNumBones++] = bone.release();
        }
    }

    class Converter
    {
        const fastgltf::Asset& asset;
        LocalScene local;
        std::vector<std::vector<unsigned int>> nodeMeshes;
        std::vector<bool> visited;
        std::vector<std::string> imagePaths;

        void Images()
        {
            local.scene.mTextures = new aiTexture* [asset.images.size()] {
            };
            for (const auto& source : asset.images)
            {
                if (const auto uri = std::get_if<fastgltf::sources::URI>(&source.data))
                {
                    Require(uri->uri->isLocalPath() && uri->fileByteOffset == 0, "Unsupported image URI");
                    const auto path = uri->uri->fspath().u8string();
                    imagePaths.emplace_back(reinterpret_cast<const char*>(path.data()), path.size());
                    continue;
                }
                std::span<const std::byte> bytes;
                auto mime = fastgltf::MimeType::None;
                if (const auto view = std::get_if<fastgltf::sources::BufferView>(&source.data))
                {
                    const auto& data = asset.bufferViews.at(view->bufferViewIndex);
                    bytes = Bytes(asset.buffers.at(data.bufferIndex).data).subspan(data.byteOffset, data.byteLength);
                    mime = view->mimeType;
                }
                else
                {
                    bytes = Bytes(source.data);
                    std::visit(
                        [&](const auto& value) {
                            if constexpr (requires { value.mimeType; })
                            {
                                mime = value.mimeType;
                            }
                        },
                        source.data);
                }
                Require(!bytes.empty() && bytes.size() <= MaxBytes, "Embedded image size limit");
                auto image = std::make_unique<aiTexture>();
                image->mWidth = static_cast<unsigned int>(bytes.size());
                image->pcData = new aiTexel[(bytes.size() + sizeof(aiTexel) - 1) / sizeof(aiTexel)];
                std::memcpy(image->pcData, bytes.data(), bytes.size());
                image->mFilename.Set(source.name);
                const char* hint = mime == fastgltf::MimeType::PNG    ? "png"
                                   : mime == fastgltf::MimeType::JPEG ? "jpg"
                                                                      : "";
                std::memcpy(image->achFormatHint, hint, std::strlen(hint) + 1);
                imagePaths.push_back("*" + std::to_string(local.scene.mNumTextures));
                local.scene.mTextures[local.scene.mNumTextures++] = image.release();
            }
        }

        void Texture(aiMaterial& material, const fastgltf::TextureInfo& info, aiTextureType type)
        {
            Require(!info.transform && info.texCoordIndex < AI_MAX_NUMBER_OF_TEXTURECOORDS,
                    "Unsupported texture coordinates");
            const auto& texture = asset.textures.at(info.textureIndex);
            Require(texture.imageIndex.has_value(), "Texture source extension is not available");
            aiString path(imagePaths.at(*texture.imageIndex));
            material.AddProperty(&path, AI_MATKEY_TEXTURE(type, 0));
            const unsigned int uv = static_cast<unsigned int>(info.texCoordIndex);
            material.AddProperty(&uv, 1, AI_MATKEY_UVWSRC(type, 0));
            auto wrap = [](fastgltf::Wrap value) {
                return value == fastgltf::Wrap::ClampToEdge      ? aiTextureMapMode_Clamp
                       : value == fastgltf::Wrap::MirroredRepeat ? aiTextureMapMode_Mirror
                                                                 : aiTextureMapMode_Wrap;
            };
            const fastgltf::Sampler fallback;
            const auto& sampler = texture.samplerIndex ? asset.samplers.at(*texture.samplerIndex) : fallback;
            const int u = wrap(sampler.wrapS), v = wrap(sampler.wrapT);
            material.AddProperty(&u, 1, AI_MATKEY_MAPPINGMODE_U(type, 0));
            material.AddProperty(&v, 1, AI_MATKEY_MAPPINGMODE_V(type, 0));
        }

        void Animations()
        {
            local.scene.mAnimations = new aiAnimation* [asset.animations.size()] {
            };
            for (const auto& source : asset.animations)
            {
                auto animation = std::make_unique<aiAnimation>();
                animation->mName.Set(source.name);
                animation->mDuration = 0;
                animation->mTicksPerSecond = 1000;
                std::map<std::size_t, std::unique_ptr<aiNodeAnim>> channels;
                for (const auto& track : source.channels)
                {
                    Require(track.nodeIndex.has_value(), "Animation channel has no node");
                    const auto nodeIndex = *track.nodeIndex;
                    Require(nodeIndex < asset.nodes.size(), "Animation node index out of bounds");
                    const auto& sampler = source.samplers.at(track.samplerIndex);
                    Require(sampler.interpolation == fastgltf::AnimationInterpolation::Linear,
                            "Animation interpolation requires a dedicated adapter");
                    auto& channel = channels[nodeIndex];
                    if (!channel)
                    {
                        channel = std::make_unique<aiNodeAnim>();
                        channel->mNodeName.Set(NodeName(asset, nodeIndex));
                    }
                    const auto& input = asset.accessors.at(sampler.inputAccessor);
                    Require(input.type == fastgltf::AccessorType::Scalar &&
                                input.componentType == fastgltf::ComponentType::Float,
                            "Invalid animation times");
                    std::vector<double> times;
                    times.reserve(input.count);
                    fastgltf::iterateAccessor<float>(asset, input, [&](float value) {
                        Require(std::isfinite(value) && value >= 0 && (times.empty() || value * 1000.0 > times.back()),
                                "Nonmonotonic animation times");
                        times.push_back(value * 1000.0);
                    });
                    Require(!times.empty(), "Empty animation channel");
                    animation->mDuration = std::max(animation->mDuration, times.back());
                    if (track.path == fastgltf::AnimationPath::Rotation)
                    {
                        Require(!channel->mRotationKeys, "Duplicate animation rotation");
                        channel->mRotationKeys = new aiQuatKey[times.size()];
                        channel->mNumRotationKeys = static_cast<unsigned int>(times.size());
                        ReadAttribute<fastgltf::math::fvec4>(
                            asset, sampler.outputAccessor, fastgltf::AccessorType::Vec4, times.size(),
                            [&](std::size_t i, const auto& value) {
                                Require(std::isfinite(value[3]), "Nonfinite animation rotation");
                                Vector({value[0], value[1], value[2]});
                                aiQuaternion rotation(value[3], value[0], value[1], value[2]);
                                Require(rotation.w * rotation.w + rotation.x * rotation.x + rotation.y * rotation.y +
                                                rotation.z * rotation.z >
                                            0,
                                        "Zero animation rotation");
                                rotation.Normalize();
                                channel->mRotationKeys[i] = aiQuatKey(times[i], rotation);
                            });
                    }
                    else
                    {
                        Require(track.path == fastgltf::AnimationPath::Translation ||
                                    track.path == fastgltf::AnimationPath::Scale,
                                "Morph animation adapter is not available");
                        auto& keys = track.path == fastgltf::AnimationPath::Translation ? channel->mPositionKeys
                                                                                        : channel->mScalingKeys;
                        auto& count = track.path == fastgltf::AnimationPath::Translation ? channel->mNumPositionKeys
                                                                                         : channel->mNumScalingKeys;
                        Require(!keys, "Duplicate animation vector track");
                        keys = new aiVectorKey[times.size()];
                        count = static_cast<unsigned int>(times.size());
                        ReadAttribute<fastgltf::math::fvec3>(asset, sampler.outputAccessor,
                                                             fastgltf::AccessorType::Vec3, times.size(),
                                                             [&](std::size_t i, const auto& value) {
                                                                 keys[i] = aiVectorKey(times[i], Vector(value));
                                                             });
                    }
                }
                animation->mChannels = new aiNodeAnim* [channels.size()] {
                };
                for (auto& [index, channel] : channels)
                {
                    aiVector3D scale, position;
                    aiQuaternion rotation;
                    Matrix(fastgltf::getTransformMatrix(asset.nodes[index])).Decompose(scale, rotation, position);
                    if (!channel->mPositionKeys)
                    {
                        channel->mPositionKeys = new aiVectorKey[1]{aiVectorKey(0, position)};
                        channel->mNumPositionKeys = 1;
                    }
                    if (!channel->mRotationKeys)
                    {
                        channel->mRotationKeys = new aiQuatKey[1]{aiQuatKey(0, rotation)};
                        channel->mNumRotationKeys = 1;
                    }
                    if (!channel->mScalingKeys)
                    {
                        channel->mScalingKeys = new aiVectorKey[1]{aiVectorKey(0, scale)};
                        channel->mNumScalingKeys = 1;
                    }
                    animation->mChannels[animation->mNumChannels++] = channel.release();
                }
                local.scene.mAnimations[local.scene.mNumAnimations++] = animation.release();
            }
        }

        aiNode* Node(std::size_t index, aiNode* parent, unsigned int depth)
        {
            Require(depth < 128 && index < asset.nodes.size() && !visited[index], "Cyclic or shared node hierarchy");
            visited[index] = true;
            const auto& source = asset.nodes[index];
            Require(!source.cameraIndex && !source.lightIndex && source.instancingAttributes.empty(),
                    "Node requires camera/light/instancing adapter");
            auto node = std::unique_ptr<aiNode, decltype(&DeleteLocalNode)>(new aiNode(), DeleteLocalNode);
            node->mName.Set(NodeName(asset, index));
            node->mParent = parent;
            node->mTransformation = Matrix(fastgltf::getTransformMatrix(source));
            if (source.meshIndex)
            {
                const auto& meshes = nodeMeshes.at(index);
                node->mMeshes = new unsigned int[meshes.size()];
                node->mNumMeshes = static_cast<unsigned int>(meshes.size());
                std::copy(meshes.begin(), meshes.end(), node->mMeshes);
            }
            node->mChildren = new aiNode* [source.children.size()] {
            };
            node->mNumChildren = static_cast<unsigned int>(source.children.size());
            for (std::size_t i = 0; i < source.children.size(); ++i)
            {
                node->mChildren[i] = Node(source.children[i], node.get(), depth + 1);
            }
            return node.release();
        }

      public:
        explicit Converter(const fastgltf::Asset& value) : asset(value), visited(value.nodes.size()) {}

        void CopyTo(aiScene* destination)
        {
            Require(asset.nodes.size() <= 65536 && asset.meshes.size() <= 65536, "Scene count limit");
            Require(asset.extensionsUsed.empty(), "Extension adapter is not available");
            std::unordered_set<std::string> names;
            for (std::size_t i = 0; i < asset.nodes.size(); ++i)
            {
                Require(names.insert(NodeName(asset, i)).second, "Ambiguous node names");
            }
            Images();
            Animations();
            std::size_t primitives = 0;
            for (const auto& node : asset.nodes)
            {
                if (node.meshIndex)
                {
                    primitives += asset.meshes.at(*node.meshIndex).primitives.size();
                }
            }
            Require(primitives > 0 && primitives <= 65536, "Mesh count limit");
            local.scene.mMeshes = new aiMesh* [primitives] {
            };
            nodeMeshes.resize(asset.nodes.size());
            std::map<std::tuple<std::size_t, std::size_t, std::size_t>, std::vector<unsigned int>> instances;
            std::size_t convertedBytes = 0;
            for (std::size_t n = 0; n < asset.nodes.size(); ++n)
            {
                const auto& node = asset.nodes[n];
                if (!node.meshIndex)
                {
                    continue;
                }
                const auto key = std::make_tuple(*node.meshIndex, node.skinIndex.value_or(SIZE_MAX),
                                                 node.weights.empty() ? SIZE_MAX : n);
                const auto existing = instances.find(key);
                if (existing != instances.end())
                {
                    nodeMeshes[n] = existing->second;
                    continue;
                }
                const auto& source = asset.meshes.at(*node.meshIndex);
                for (const auto& primitive : source.primitives)
                {
                    const auto position = primitive.findAttribute("POSITION");
                    Require(position != primitive.attributes.end(), "Missing positions");
                    const auto count = asset.accessors.at(position->accessorIndex).count;
                    const auto bytesPerVertex = 512 + 48 * primitive.targets.size();
                    Require(bytesPerVertex <= MaxBytes && count <= (MaxBytes - convertedBytes) / bytesPerVertex,
                            "Converted scene size limit");
                    convertedBytes += count * bytesPerVertex;
                    auto mesh = MakeMesh(asset, source, primitive);
                    if (node.skinIndex)
                    {
                        Skin(*mesh, asset, primitive, asset.skins.at(*node.skinIndex));
                    }
                    if (!node.weights.empty())
                    {
                        Require(node.weights.size() == mesh->mNumAnimMeshes, "Morph weight count mismatch");
                        for (std::size_t i = 0; i < node.weights.size(); ++i)
                        {
                            mesh->mAnimMeshes[i]->mWeight = node.weights[i];
                        }
                    }
                    nodeMeshes[n].push_back(local.scene.mNumMeshes);
                    local.scene.mMeshes[local.scene.mNumMeshes++] = mesh.release();
                }
                instances.emplace(key, nodeMeshes[n]);
            }
            const auto materialCount = asset.materials.size() + 1;
            local.scene.mMaterials = new aiMaterial* [materialCount] {
            };
            for (std::size_t i = 0; i < materialCount; ++i)
            {
                auto material = std::make_unique<aiMaterial>();
                const fastgltf::Material fallback;
                const auto& source = i < asset.materials.size() ? asset.materials[i] : fallback;
                aiString name(source.name.empty() ? "material_" + std::to_string(i) : source.name);
                material->AddProperty(&name, AI_MATKEY_NAME);
                const auto color = source.pbrData.baseColorFactor;
                aiColor4D base(color[0], color[1], color[2], color[3]);
                material->AddProperty(&base, 1, AI_MATKEY_COLOR_DIFFUSE);
                material->AddProperty(&base, 1, AI_MATKEY_BASE_COLOR);
                material->AddProperty(&source.pbrData.metallicFactor, 1, AI_MATKEY_METALLIC_FACTOR);
                material->AddProperty(&source.pbrData.roughnessFactor, 1, AI_MATKEY_ROUGHNESS_FACTOR);
                const int twoSided = source.doubleSided;
                material->AddProperty(&twoSided, 1, AI_MATKEY_TWOSIDED);
                const float opacity = source.alphaMode == fastgltf::AlphaMode::Opaque ? 1.0f : color[3];
                material->AddProperty(&opacity, 1, AI_MATKEY_OPACITY);
                aiColor3D emissive(source.emissiveFactor[0], source.emissiveFactor[1], source.emissiveFactor[2]);
                material->AddProperty(&emissive, 1, AI_MATKEY_COLOR_EMISSIVE);
                aiString alpha(source.alphaMode == fastgltf::AlphaMode::Opaque ? "OPAQUE"
                               : source.alphaMode == fastgltf::AlphaMode::Mask ? "MASK"
                                                                               : "BLEND");
                material->AddProperty(&alpha, AI_MATKEY_GLTF_ALPHAMODE);
                material->AddProperty(&source.alphaCutoff, 1, AI_MATKEY_GLTF_ALPHACUTOFF);
                if (source.pbrData.baseColorTexture)
                {
                    Texture(*material, *source.pbrData.baseColorTexture, aiTextureType_DIFFUSE);
                    Texture(*material, *source.pbrData.baseColorTexture, aiTextureType_BASE_COLOR);
                }
                if (source.pbrData.metallicRoughnessTexture)
                {
                    Texture(*material, *source.pbrData.metallicRoughnessTexture, aiTextureType_UNKNOWN);
                    Texture(*material, *source.pbrData.metallicRoughnessTexture, aiTextureType_METALNESS);
                    Texture(*material, *source.pbrData.metallicRoughnessTexture, aiTextureType_DIFFUSE_ROUGHNESS);
                }
                if (source.normalTexture)
                {
                    Texture(*material, *source.normalTexture, aiTextureType_NORMALS);
                    material->AddProperty(&source.normalTexture->scale, 1,
                                          AI_MATKEY_GLTF_TEXTURE_SCALE(aiTextureType_NORMALS, 0));
                }
                if (source.occlusionTexture)
                {
                    Texture(*material, *source.occlusionTexture, aiTextureType_LIGHTMAP);
                    material->AddProperty(&source.occlusionTexture->strength, 1,
                                          AI_MATKEY_GLTF_TEXTURE_STRENGTH(aiTextureType_LIGHTMAP, 0));
                }
                if (source.emissiveTexture)
                {
                    Texture(*material, *source.emissiveTexture, aiTextureType_EMISSIVE);
                }
                local.scene.mMaterials[local.scene.mNumMaterials++] = material.release();
            }
            Require(!asset.scenes.empty(), "Scene has no roots");
            const auto& roots = asset.scenes.at(asset.defaultScene.value_or(0));
            if (roots.nodeIndices.size() == 1)
            {
                local.scene.mRootNode = Node(roots.nodeIndices[0], nullptr, 0);
            }
            else
            {
                local.scene.mRootNode = new aiNode();
                auto root = local.scene.mRootNode;
                root->mName.Set("ROOT");
                root->mChildren = new aiNode* [roots.nodeIndices.size()] {
                };
                root->mNumChildren = static_cast<unsigned int>(roots.nodeIndices.size());
                for (std::size_t i = 0; i < roots.nodeIndices.size(); ++i)
                {
                    root->mChildren[i] = Node(roots.nodeIndices[i], root, 0);
                }
            }
            // Preserve the DLL-allocated scene and its private importer state.
#if FASTGLTF_SHARED_CRT_SCENE
            local.MoveTo(*destination);
#else
            Assimp::SceneCombiner::CopyScene(&destination, &local.scene, false);
#endif
        }
    };

    class Loader final : public Assimp::BaseImporter
    {
      public:
        bool CanRead(const std::string&, Assimp::IOSystem*, bool) const override
        {
            return true;
        }

        const aiImporterDesc* GetInfo() const override
        {
            static const aiImporterDesc info{
                "Renderer fastgltf adapter", "", "", "glTF2", 0, 2, 0, 2, 0, "gltf glb vrm"};
            return &info;
        }

        void InternReadFile(const std::string& file, aiScene* scene, Assimp::IOSystem*) override
        {
            const auto path = std::filesystem::u8path(file);
            const auto size = std::filesystem::file_size(path);
            Require(size > 0 && size <= MaxBytes, "Input file size limit");
            auto data = fastgltf::GltfDataBuffer::FromPath(path);
            Require(data.error() == fastgltf::Error::None, "Cannot read glTF input");
            fastgltf::Parser parser(fastgltf::Extensions::KHR_draco_mesh_compression);
            auto result = parser.loadGltf(data.get(), path.parent_path());
            Require(result.error() == fastgltf::Error::None, "fastgltf parse failed");
            auto& asset = result.get();
            LoadBuffers(asset, path.parent_path());
            ValidateBuffers(asset);
            if (GltfDraco::Expand(asset))
            {
                ValidateBuffers(asset);
            }
            Require(fastgltf::validate(asset) == fastgltf::Error::None, "glTF validation failed");
            Converter(asset).CopyTo(scene);
        }
    };

    std::mutex ownerMutex;
    std::unordered_map<const aiScene*, std::unique_ptr<Assimp::Importer>> owners;

    bool HandlesGltf(const char* extensions)
    {
        std::string_view remaining = extensions ? extensions : "";
        while (!remaining.empty())
        {
            const auto first = remaining.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
            {
                return false;
            }
            remaining.remove_prefix(first);
            const auto end = remaining.find_first_of(" \t\r\n");
            const auto extension = remaining.substr(0, end);
            if (extension == "gltf" || extension == "glb" || extension == "vrm")
            {
                return true;
            }
            if (end == std::string_view::npos)
            {
                return false;
            }
            remaining.remove_prefix(end);
        }
        return false;
    }

    void RemoveGltfLoaders(Assimp::Importer& importer)
    {
        std::vector<Assimp::BaseImporter*> loaders;
        const auto count = importer.GetImporterCount();
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto info = importer.GetImporterInfo(index);
            Require(info != nullptr, "Missing importer description");
            if (HandlesGltf(info->mFileExtensions))
            {
                loaders.push_back(importer.GetImporter(index));
            }
        }
        for (const auto loader : loaders)
        {
            Require(importer.UnregisterLoader(loader) == AI_SUCCESS, "Cannot replace glTF loader");
            delete loader;
        }
    }
}
#endif

namespace FastGltfImport
{
    bool Available() noexcept
    {
        return RENDERINGPROJECT_USE_FASTGLTF != 0;
    }

    const aiScene* Load(const char* path, unsigned int flags, char* error, std::size_t capacity) noexcept
    {
        if (error && capacity)
        {
            error[0] = 0;
        }
        auto failure = [&](const char* message) {
            if (error && capacity)
            {
                const auto count = std::min(capacity - 1, std::strlen(message));
                std::memcpy(error, message, count);
                error[count] = 0;
            }
        };
#if RENDERINGPROJECT_USE_FASTGLTF
        try
        {
            Require(path && *path, "Missing path");
            auto extension = std::filesystem::u8path(path).extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            Require(extension == ".gltf" || extension == ".glb" || extension == ".vrm", "Unsupported file type");
            auto importer = std::make_unique<Assimp::Importer>();
            RemoveGltfLoaders(*importer);
            auto loader = std::make_unique<Loader>();
            Require(importer->RegisterLoader(loader.get()) == AI_SUCCESS, "Cannot register fastgltf loader");
            loader.release();
            const auto scene = importer->ReadFile(path, flags);
            if (!scene)
            {
                failure(importer->GetErrorString());
                return nullptr;
            }
            std::lock_guard lock(ownerMutex);
            owners.emplace(scene, std::move(importer));
            return scene;
        }
        catch (const std::exception& exception)
        {
            failure(exception.what());
        }
        catch (...)
        {
            failure("Unknown fastgltf import failure");
        }
#else
        (void)path;
        (void)flags;
        failure("fastgltf SDK is disabled");
#endif
        return nullptr;
    }

    bool Release(const aiScene* scene) noexcept
    {
#if RENDERINGPROJECT_USE_FASTGLTF
        std::unique_ptr<Assimp::Importer> owner;
        {
            std::lock_guard lock(ownerMutex);
            const auto found = owners.find(scene);
            if (found == owners.end())
            {
                return false;
            }
            owner = std::move(found->second);
            owners.erase(found);
        }
        return true;
#else
        (void)scene;
        return false;
#endif
    }
}
