#include "pch.h"
#include "pmxloader.h"
#include "modelimportutils.h"
#include <filesystem>
#include <fstream>

using namespace std;

namespace
{
	struct Reader
	{
		const vector<uint8_t>& Bytes;
		size_t Pos = 0;
		uint8_t Encoding = 1;
		uint8_t AdditionalUvCount = 0;
		uint8_t VertexIndexSize = 4;
		uint8_t TextureIndexSize = 4;
		uint8_t MaterialIndexSize = 4;
		uint8_t BoneIndexSize = 4;
		uint8_t MorphIndexSize = 4;
		uint8_t RigidBodyIndexSize = 4;

		bool CanRead(size_t byteCount) const
		{
			return Pos <= Bytes.size() && byteCount <= Bytes.size() - Pos;
		}

		bool ReadBytes(void* outValue, size_t byteCount)
		{
			if (!CanRead(byteCount))
			{
				return false;
			}
			memcpy(outValue, Bytes.data() + Pos, byteCount);
			Pos += byteCount;
			return true;
		}

		bool Read(uint8_t& value) { return ReadBytes(&value, sizeof(value)); }
		bool Read(uint16_t& value) { return ReadBytes(&value, sizeof(value)); }
		bool Read(uint32_t& value) { return ReadBytes(&value, sizeof(value)); }
		bool Read(int32_t& value) { return ReadBytes(&value, sizeof(value)); }
		bool Read(float& value) { return ReadBytes(&value, sizeof(value)); }

		bool Skip(size_t byteCount)
		{
			if (!CanRead(byteCount))
			{
				return false;
			}
			Pos += byteCount;
			return true;
		}

		bool ReadIndex(uint8_t indexSize, int32_t& value)
		{
			if (indexSize == 1)
			{
				uint8_t raw = 0;
				if (!Read(raw)) return false;
				value = raw == 0xff ? -1 : static_cast<int32_t>(raw);
				return true;
			}
			if (indexSize == 2)
			{
				uint16_t raw = 0;
				if (!Read(raw)) return false;
				value = raw == 0xffff ? -1 : static_cast<int32_t>(raw);
				return true;
			}
			return Read(value);
		}

		bool ReadUnsignedIndex(uint8_t indexSize, uint32_t& value)
		{
			if (indexSize == 1)
			{
				uint8_t raw = 0;
				if (!Read(raw)) return false;
				value = raw;
				return true;
			}
			if (indexSize == 2)
			{
				uint16_t raw = 0;
				if (!Read(raw)) return false;
				value = raw;
				return true;
			}
			return Read(value);
		}

		bool SkipIndex(uint8_t indexSize)
		{
			return Skip(indexSize);
		}

		bool ReadText(string& text)
		{
			int32_t byteLength = 0;
			if (!Read(byteLength) || byteLength < 0 || !CanRead(static_cast<size_t>(byteLength)))
			{
				return false;
			}
			if (byteLength == 0)
			{
				text.clear();
				return true;
			}

			if (Encoding == 0)
			{
				if ((byteLength % 2) != 0)
				{
					return false;
				}
				const int wideLength = byteLength / 2;
				wstring wideText(static_cast<size_t>(wideLength), L'\0');
				memcpy(wideText.data(), Bytes.data() + Pos, static_cast<size_t>(byteLength));
				Pos += static_cast<size_t>(byteLength);

				const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wideText.data(), wideLength, nullptr, 0, nullptr, nullptr);
				if (utf8Length <= 0)
				{
					text.clear();
					return true;
				}
				text.assign(static_cast<size_t>(utf8Length), '\0');
				WideCharToMultiByte(CP_UTF8, 0, wideText.data(), wideLength, text.data(), utf8Length, nullptr, nullptr);
				return true;
			}

			text.assign(reinterpret_cast<const char*>(Bytes.data() + Pos), static_cast<size_t>(byteLength));
			Pos += static_cast<size_t>(byteLength);
			return true;
		}
	};

	aiMatrix4x4 MakeIdentityMatrix()
	{
		return aiMatrix4x4(
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f);
	}

	aiMatrix4x4 MakeTranslationMatrix(const aiVector3D& translation)
	{
		return aiMatrix4x4(
			aiVector3D(1.0f, 1.0f, 1.0f),
			aiQuaternion(1.0f, 0.0f, 0.0f, 0.0f),
			translation);
	}

	bool ReadFloat3(Reader& reader, aiVector3D& outValue)
	{
		return reader.Read(outValue.x) && reader.Read(outValue.y) && reader.Read(outValue.z);
	}

	bool ReadFloat3(Reader& reader, XMFLOAT3& outValue)
	{
		return reader.Read(outValue.x) && reader.Read(outValue.y) && reader.Read(outValue.z);
	}

	bool ReadFloat4(Reader& reader, XMFLOAT4& outValue)
	{
		return reader.Read(outValue.x) && reader.Read(outValue.y) &&
			reader.Read(outValue.z) && reader.Read(outValue.w);
	}

	aiVector3D ConvertPosition(const aiVector3D& value)
	{
		return value;
	}

	aiQuaternion ConvertRotation(aiQuaternion value)
	{
		value.Normalize();
		return value;
	}

	void AddMaterialProperties(aiMaterial* material, const PmxBinary::Material& source, const vector<string>& textures)
	{
		const string materialName = !source.Name.empty() ? source.Name : source.EnglishName;
		aiString name(materialName.c_str());
		material->AddProperty(&name, AI_MATKEY_NAME);

		aiColor3D diffuse(source.Diffuse.x, source.Diffuse.y, source.Diffuse.z);
		material->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
		float opacity = source.Diffuse.w;
		material->AddProperty(&opacity, 1, AI_MATKEY_OPACITY);
		aiColor3D ambient(source.Ambient.x, source.Ambient.y, source.Ambient.z);
		material->AddProperty(&ambient, 1, AI_MATKEY_COLOR_AMBIENT);
		aiColor3D specular(source.Specular.x, source.Specular.y, source.Specular.z);
		material->AddProperty(&specular, 1, AI_MATKEY_COLOR_SPECULAR);
		material->AddProperty(&source.SpecularPower, 1, AI_MATKEY_SHININESS);

		if (source.TextureIndex >= 0 && source.TextureIndex < static_cast<int32_t>(textures.size()) &&
			!textures[static_cast<size_t>(source.TextureIndex)].empty())
		{
			aiString texPath(textures[static_cast<size_t>(source.TextureIndex)].c_str());
			material->AddProperty(&texPath, AI_MATKEY_TEXTURE(aiTextureType_DIFFUSE, 0));
		}
	}
}

namespace PmxBinary
{
	bool LoadModel(const char* fileName, Model& outModel)
	{
		const filesystem::path pmxPath = ModelImportUtils::FromUtf8(fileName);
		ifstream stream(pmxPath, ios::binary | ios::ate);
		if (!stream)
		{
			Debug::Log("ERROR: Failed to open PMX model: %s\n", fileName);
			return false;
		}

		const streamsize fileSize = stream.tellg();
		if (fileSize <= 0)
		{
			Debug::Log("ERROR: PMX file is empty: %s\n", fileName);
			return false;
		}

		vector<uint8_t> bytes(static_cast<size_t>(fileSize));
		stream.seekg(0, ios::beg);
		if (!stream.read(reinterpret_cast<char*>(bytes.data()), fileSize))
		{
			Debug::Log("ERROR: Failed to read PMX model: %s\n", fileName);
			return false;
		}

		Reader reader{ bytes };
		array<char, 4> magic{};
		if (!reader.ReadBytes(magic.data(), magic.size()) || string(magic.data(), magic.size()) != "PMX ")
		{
			Debug::Log("ERROR: Invalid PMX header: %s\n", fileName);
			return false;
		}

		float version = 0.0f;
		uint8_t configSize = 0;
		if (!reader.Read(version) || !reader.Read(configSize) || configSize < 8 || !reader.CanRead(configSize))
		{
			Debug::Log("ERROR: PMX header config is truncated: %s\n", fileName);
			return false;
		}

		if (!reader.Read(reader.Encoding) ||
			!reader.Read(reader.AdditionalUvCount) ||
			!reader.Read(reader.VertexIndexSize) ||
			!reader.Read(reader.TextureIndexSize) ||
			!reader.Read(reader.MaterialIndexSize) ||
			!reader.Read(reader.BoneIndexSize) ||
			!reader.Read(reader.MorphIndexSize) ||
			!reader.Read(reader.RigidBodyIndexSize))
		{
			return false;
		}
		if (configSize > 8 && !reader.Skip(configSize - 8))
		{
			return false;
		}

		string ignoredText;
		if (!reader.ReadText(outModel.ModelName) ||
			!reader.ReadText(outModel.EnglishModelName) ||
			!reader.ReadText(ignoredText) ||
			!reader.ReadText(ignoredText))
		{
			Debug::Log("ERROR: PMX model text is truncated: %s\n", fileName);
			return false;
		}

		int32_t vertexCount = 0;
		if (!reader.Read(vertexCount) || vertexCount < 0)
		{
			Debug::Log("ERROR: PMX vertex count is invalid: %s\n", fileName);
			return false;
		}
		outModel.Vertices.resize(static_cast<size_t>(vertexCount));
		size_t sdefVertexCount = 0;
		for (int32_t i = 0; i < vertexCount; ++i)
		{
			Vertex& vertex = outModel.Vertices[static_cast<size_t>(i)];
			if (!ReadFloat3(reader, vertex.Position) ||
				!ReadFloat3(reader, vertex.Normal) ||
				!reader.Read(vertex.TexCoord.x) ||
				!reader.Read(vertex.TexCoord.y) ||
				!reader.Skip(static_cast<size_t>(reader.AdditionalUvCount) * 16))
			{
				Debug::Log("ERROR: PMX vertex data is truncated: %s\n", fileName);
				return false;
			}

			if (!reader.Read(vertex.DeformType))
			{
				return false;
			}

			switch (vertex.DeformType)
			{
			case 0:
				if (!reader.ReadIndex(reader.BoneIndexSize, vertex.BoneIndices[0])) return false;
				vertex.BoneWeights[0] = 1.0f;
				break;
			case 1:
				if (!reader.ReadIndex(reader.BoneIndexSize, vertex.BoneIndices[0]) ||
					!reader.ReadIndex(reader.BoneIndexSize, vertex.BoneIndices[1]) ||
					!reader.Read(vertex.BoneWeights[0]))
				{
					return false;
				}
				vertex.BoneWeights[1] = 1.0f - vertex.BoneWeights[0];
				break;
			case 2:
			case 4:
				for (int n = 0; n < 4; ++n)
				{
					if (!reader.ReadIndex(reader.BoneIndexSize, vertex.BoneIndices[n])) return false;
				}
				for (int n = 0; n < 4; ++n)
				{
					if (!reader.Read(vertex.BoneWeights[n])) return false;
				}
				break;
			case 3:
				if (!reader.ReadIndex(reader.BoneIndexSize, vertex.BoneIndices[0]) ||
					!reader.ReadIndex(reader.BoneIndexSize, vertex.BoneIndices[1]) ||
					!reader.Read(vertex.BoneWeights[0]) ||
					!ReadFloat3(reader, vertex.SdefC) ||
					!ReadFloat3(reader, vertex.SdefR0) ||
					!ReadFloat3(reader, vertex.SdefR1))
				{
					return false;
				}
				vertex.BoneWeights[1] = 1.0f - vertex.BoneWeights[0];
				++sdefVertexCount;
				break;
			default:
				Debug::Log("ERROR: PMX vertex deform type is unsupported: %u (%s)\n", vertex.DeformType, fileName);
				return false;
			}

			if (!reader.Read(vertex.EdgeScale))
			{
				return false;
			}
		}

		int32_t indexCount = 0;
		if (!reader.Read(indexCount) || indexCount < 0)
		{
			Debug::Log("ERROR: PMX index count is invalid: %s\n", fileName);
			return false;
		}
		outModel.Indices.resize(static_cast<size_t>(indexCount));
		for (int32_t i = 0; i < indexCount; ++i)
		{
			if (!reader.ReadUnsignedIndex(reader.VertexIndexSize, outModel.Indices[static_cast<size_t>(i)]))
			{
				Debug::Log("ERROR: PMX index data is truncated: %s\n", fileName);
				return false;
			}
		}

		int32_t textureCount = 0;
		if (!reader.Read(textureCount) || textureCount < 0)
		{
			Debug::Log("ERROR: PMX texture count is invalid: %s\n", fileName);
			return false;
		}
		outModel.Textures.resize(static_cast<size_t>(textureCount));
		for (int32_t i = 0; i < textureCount; ++i)
		{
			if (!reader.ReadText(outModel.Textures[static_cast<size_t>(i)]))
			{
				return false;
			}
		}

		int32_t materialCount = 0;
		if (!reader.Read(materialCount) || materialCount < 0)
		{
			Debug::Log("ERROR: PMX material count is invalid: %s\n", fileName);
			return false;
		}
		outModel.Materials.resize(static_cast<size_t>(materialCount));
		for (int32_t i = 0; i < materialCount; ++i)
		{
			Material& material = outModel.Materials[static_cast<size_t>(i)];
			int32_t surfaceCount = 0;
			if (!reader.ReadText(material.Name) ||
				!reader.ReadText(material.EnglishName) ||
				!ReadFloat4(reader, material.Diffuse) ||
				!reader.Read(material.Specular.x) ||
				!reader.Read(material.Specular.y) ||
				!reader.Read(material.Specular.z) ||
				!reader.Read(material.SpecularPower) ||
				!reader.Read(material.Ambient.x) ||
				!reader.Read(material.Ambient.y) ||
				!reader.Read(material.Ambient.z) ||
				!reader.Read(material.DrawFlags) ||
				!ReadFloat4(reader, material.EdgeColor) ||
				!reader.Read(material.EdgeSize) ||
				!reader.ReadIndex(reader.TextureIndexSize, material.TextureIndex) ||
				!reader.ReadIndex(reader.TextureIndexSize, material.SphereTextureIndex) ||
				!reader.Read(material.SphereMode) ||
				!reader.Read(material.ToonMode))
			{
				Debug::Log("ERROR: PMX material data is truncated: %s\n", fileName);
				return false;
			}

			if (material.ToonMode == 0)
			{
				if (!reader.ReadIndex(reader.TextureIndexSize, material.ToonTextureIndex)) return false;
			}
			else
			{
				uint8_t sharedToon = 0;
				if (!reader.Read(sharedToon)) return false;
				material.ToonTextureIndex = static_cast<int32_t>(sharedToon);
			}

			if (!reader.ReadText(ignoredText) || !reader.Read(surfaceCount) || surfaceCount < 0)
			{
				return false;
			}
			material.IndexCount = static_cast<uint32_t>(surfaceCount);
		}

		int32_t boneCount = 0;
		if (!reader.Read(boneCount) || boneCount < 0)
		{
			Debug::Log("ERROR: PMX bone count is invalid: %s\n", fileName);
			return false;
		}
		outModel.Bones.resize(static_cast<size_t>(boneCount));
		for (int32_t i = 0; i < boneCount; ++i)
		{
			Bone& bone = outModel.Bones[static_cast<size_t>(i)];
			int32_t ignoredIndex = -1;
			if (!reader.ReadText(bone.Name) ||
				!reader.ReadText(bone.EnglishName) ||
				!ReadFloat3(reader, bone.Position) ||
				!reader.ReadIndex(reader.BoneIndexSize, bone.ParentIndex) ||
				!reader.Read(bone.DeformDepth) ||
				!reader.Read(bone.Flags))
			{
				Debug::Log("ERROR: PMX bone data is truncated: %s\n", fileName);
				return false;
			}

			if ((bone.Flags & 0x0001) != 0)
			{
				if (!reader.ReadIndex(reader.BoneIndexSize, ignoredIndex)) return false;
			}
			else if (!reader.Skip(12))
			{
				return false;
			}

			if ((bone.Flags & 0x0100) != 0 || (bone.Flags & 0x0200) != 0)
			{
				if (!reader.ReadIndex(reader.BoneIndexSize, bone.AppendBoneIndex) ||
					!reader.Read(bone.AppendWeight))
				{
					return false;
				}
			}
			if ((bone.Flags & 0x0400) != 0 && !reader.Skip(12)) return false;
			if ((bone.Flags & 0x0800) != 0 && !reader.Skip(24)) return false;
			if ((bone.Flags & 0x2000) != 0 && !reader.Skip(4)) return false;

			if ((bone.Flags & 0x0020) != 0)
			{
				int32_t linkCount = 0;
				if (!reader.ReadIndex(reader.BoneIndexSize, bone.IkTargetIndex) ||
					!reader.Read(bone.IkIterationCount) ||
					!reader.Read(bone.IkLimitAngle) ||
					!reader.Read(linkCount) ||
					linkCount < 0)
				{
					return false;
				}

				bone.IkLinks.resize(static_cast<size_t>(linkCount));
				for (int32_t link = 0; link < linkCount; ++link)
				{
					IkLink& rawLink = bone.IkLinks[static_cast<size_t>(link)];
					uint8_t hasLimit = 0;
					if (!reader.ReadIndex(reader.BoneIndexSize, rawLink.BoneIndex) ||
						!reader.Read(hasLimit))
					{
						return false;
					}
					rawLink.HasLimit = hasLimit != 0;
					if (rawLink.HasLimit &&
						(!ReadFloat3(reader, rawLink.LimitMin) || !ReadFloat3(reader, rawLink.LimitMax)))
					{
						return false;
					}
				}
			}
		}

		int32_t morphCount = 0;
		if (!reader.Read(morphCount) || morphCount < 0)
		{
			Debug::Log("ERROR: PMX morph count is invalid: %s\n", fileName);
			return false;
		}
		outModel.Morphs.resize(static_cast<size_t>(morphCount));
		for (int32_t i = 0; i < morphCount; ++i)
		{
			PmxMorph& morph = outModel.Morphs[static_cast<size_t>(i)];
			string englishName;
			uint8_t panel = 0;
			int32_t offsetCount = 0;
			if (!reader.ReadText(morph.Name) ||
				!reader.ReadText(englishName) ||
				!reader.Read(panel) ||
				!reader.Read(morph.Type) ||
				!reader.Read(offsetCount) ||
				offsetCount < 0)
			{
				Debug::Log("ERROR: PMX morph data is truncated: %s\n", fileName);
				return false;
			}

			for (int32_t offset = 0; offset < offsetCount; ++offset)
			{
				switch (morph.Type)
				{
				case 0:
				{
					int32_t morphIndex = -1;
					float weight = 0.0f;
					if (!reader.ReadIndex(reader.MorphIndexSize, morphIndex) || !reader.Read(weight)) return false;
					if (morphIndex >= 0)
					{
						morph.GroupOffsets.push_back({ static_cast<uint32_t>(morphIndex), weight });
					}
					break;
				}
				case 1:
				{
					uint32_t vertexIndex = 0;
					aiVector3D position{};
					if (!reader.ReadUnsignedIndex(reader.VertexIndexSize, vertexIndex) ||
						!ReadFloat3(reader, position))
					{
						return false;
					}
					morph.PositionOffsets.push_back({ vertexIndex, ConvertPosition(position) });
					break;
				}
				case 2:
				{
					int32_t boneIndex = -1;
					aiVector3D position{};
					float qx = 0.0f;
					float qy = 0.0f;
					float qz = 0.0f;
					float qw = 1.0f;
					if (!reader.ReadIndex(reader.BoneIndexSize, boneIndex) ||
						!ReadFloat3(reader, position) ||
						!reader.Read(qx) ||
						!reader.Read(qy) ||
						!reader.Read(qz) ||
						!reader.Read(qw))
					{
						return false;
					}
					if (boneIndex >= 0 && boneIndex < static_cast<int32_t>(outModel.Bones.size()))
					{
						PmxBoneMorphOffset boneOffset{};
						boneOffset.BoneName = outModel.Bones[static_cast<size_t>(boneIndex)].Name;
						boneOffset.Position = ConvertPosition(position);
						boneOffset.Rotation = ConvertRotation(aiQuaternion(qw, qx, qy, qz));
						morph.BoneOffsets.push_back(boneOffset);
					}
					break;
				}
				case 3:
				{
					uint32_t vertexIndex = 0;
					XMFLOAT4 uv{};
					if (!reader.ReadUnsignedIndex(reader.VertexIndexSize, vertexIndex) ||
						!ReadFloat4(reader, uv))
					{
						return false;
					}
					morph.UvOffsets.push_back({ vertexIndex, uv });
					break;
				}
				case 4:
				case 5:
				case 6:
				case 7:
					if (!reader.SkipIndex(reader.VertexIndexSize) || !reader.Skip(16)) return false;
					break;
				case 8:
				{
					PmxMaterialMorphOffset materialOffset{};
					XMFLOAT4 ignored{};
					float ignoredFloat = 0.0f;
					if (!reader.ReadIndex(reader.MaterialIndexSize, materialOffset.MaterialIndex) ||
						!reader.Read(materialOffset.Operation) ||
						!ReadFloat4(reader, materialOffset.Diffuse) ||
						!reader.Skip(12) ||
						!reader.Read(ignoredFloat) ||
						!reader.Skip(12) ||
						!ReadFloat4(reader, ignored) ||
						!reader.Read(ignoredFloat) ||
						!ReadFloat4(reader, ignored) ||
						!ReadFloat4(reader, ignored) ||
						!ReadFloat4(reader, ignored))
					{
						return false;
					}
					morph.MaterialOffsets.push_back(materialOffset);
					break;
				}
				case 9:
					if (!reader.SkipIndex(reader.MorphIndexSize) || !reader.Skip(4)) return false;
					break;
				case 10:
					if (!reader.SkipIndex(reader.RigidBodyIndexSize) || !reader.Skip(25)) return false;
					break;
				default:
					Debug::Log("ERROR: PMX morph type is unsupported: %u (%s)\n", morph.Type, fileName);
					return false;
				}
			}
		}

		// Display frames sit between morphs and rigid bodies in PMX 2.x. They
		// are editor metadata, but must be consumed to reach the physics block.
		int32_t displayFrameCount = 0;
		if (!reader.Read(displayFrameCount) || displayFrameCount < 0)
		{
			Debug::Log("ERROR: PMX display frame count is invalid: %s\n", fileName);
			return false;
		}
		for (int32_t frame = 0; frame < displayFrameCount; ++frame)
		{
			string frameName;
			string frameEnglishName;
			uint8_t specialFrame = 0;
			int32_t elementCount = 0;
			if (!reader.ReadText(frameName) ||
				!reader.ReadText(frameEnglishName) ||
				!reader.Read(specialFrame) ||
				!reader.Read(elementCount) ||
				elementCount < 0)
			{
				Debug::Log("ERROR: PMX display frame data is truncated: %s\n", fileName);
				return false;
			}
			for (int32_t element = 0; element < elementCount; ++element)
			{
				uint8_t elementType = 0;
				if (!reader.Read(elementType))
				{
					return false;
				}
				const uint8_t indexSize = elementType == 0
					? reader.BoneIndexSize : reader.MorphIndexSize;
				if (elementType > 1 || !reader.SkipIndex(indexSize))
				{
					return false;
				}
			}
		}

		int32_t rigidBodyCount = 0;
		if (!reader.Read(rigidBodyCount) || rigidBodyCount < 0)
		{
			Debug::Log("ERROR: PMX rigid body count is invalid: %s\n", fileName);
			return false;
		}
		outModel.RigidBodies.resize(static_cast<size_t>(rigidBodyCount));
		for (int32_t i = 0; i < rigidBodyCount; ++i)
		{
			PmxRigidBodyData& body = outModel.RigidBodies[static_cast<size_t>(i)];
			if (!reader.ReadText(body.Name) ||
				!reader.ReadText(body.EnglishName) ||
				!reader.ReadIndex(reader.BoneIndexSize, body.BoneIndex) ||
				!reader.Read(body.CollisionGroup) ||
				!reader.Read(body.CollisionMask) ||
				!reader.Read(body.Shape) ||
				!ReadFloat3(reader, body.Size) ||
				!ReadFloat3(reader, body.Position) ||
				!ReadFloat3(reader, body.Rotation) ||
				!reader.Read(body.Mass) ||
				!reader.Read(body.LinearDamping) ||
				!reader.Read(body.AngularDamping) ||
				!reader.Read(body.Restitution) ||
				!reader.Read(body.Friction) ||
				!reader.Read(body.Operation))
			{
				Debug::Log("ERROR: PMX rigid body data is truncated: %s\n", fileName);
				return false;
			}
			if (body.BoneIndex >= 0 &&
				body.BoneIndex < static_cast<int32_t>(outModel.Bones.size()))
			{
				body.BoneName = outModel.Bones[static_cast<size_t>(body.BoneIndex)].Name;
			}
		}

		int32_t jointCount = 0;
		if (!reader.Read(jointCount) || jointCount < 0)
		{
			Debug::Log("ERROR: PMX joint count is invalid: %s\n", fileName);
			return false;
		}
		outModel.Joints.resize(static_cast<size_t>(jointCount));
		for (int32_t i = 0; i < jointCount; ++i)
		{
			PmxJointData& joint = outModel.Joints[static_cast<size_t>(i)];
			if (!reader.ReadText(joint.Name) ||
				!reader.ReadText(joint.EnglishName) ||
				!reader.Read(joint.Type) ||
				!reader.ReadIndex(reader.RigidBodyIndexSize, joint.RigidBodyA) ||
				!reader.ReadIndex(reader.RigidBodyIndexSize, joint.RigidBodyB) ||
				!ReadFloat3(reader, joint.Position) ||
				!ReadFloat3(reader, joint.Rotation) ||
				!ReadFloat3(reader, joint.LinearLimitMin) ||
				!ReadFloat3(reader, joint.LinearLimitMax) ||
				!ReadFloat3(reader, joint.AngularLimitMin) ||
				!ReadFloat3(reader, joint.AngularLimitMax) ||
				!ReadFloat3(reader, joint.LinearSpring) ||
				!ReadFloat3(reader, joint.AngularSpring))
			{
				Debug::Log("ERROR: PMX joint data is truncated: %s\n", fileName);
				return false;
			}
		}

		Debug::Log("PMX binary model loaded: %s (version=%.1f, vertices=%zu, indices=%zu, materials=%zu, textures=%zu, bones=%zu, morphs=%zu, rigidBodies=%zu, joints=%zu, sdefVertices=%zu)\n",
			fileName, version, outModel.Vertices.size(), outModel.Indices.size(), outModel.Materials.size(),
			outModel.Textures.size(), outModel.Bones.size(), outModel.Morphs.size(),
			outModel.RigidBodies.size(), outModel.Joints.size(), sdefVertexCount);
		return true;
	}

	aiScene* CreateGeneratedScene(const Model& model, vector<vector<uint32_t>>& outMeshVertexPmxIndices)
	{
		unique_ptr<aiScene> scene(new aiScene());
		scene->mRootNode = new aiNode();
		scene->mRootNode->mName = aiString("PMXRoot");
		scene->mRootNode->mTransformation = MakeIdentityMatrix();

		vector<aiNode*> boneNodes(model.Bones.size(), nullptr);
		vector<vector<aiNode*>> childLists(model.Bones.size() + 1);
		const size_t rootChildSlot = model.Bones.size();
		for (size_t i = 0; i < model.Bones.size(); ++i)
		{
			const Bone& bone = model.Bones[i];
			aiNode* node = new aiNode();
			node->mName = aiString(bone.Name.c_str());
			const bool hasParent = bone.ParentIndex >= 0 &&
				bone.ParentIndex < static_cast<int32_t>(model.Bones.size()) &&
				bone.ParentIndex != static_cast<int32_t>(i);
			const aiVector3D parentPosition = hasParent ? model.Bones[static_cast<size_t>(bone.ParentIndex)].Position : aiVector3D(0.0f, 0.0f, 0.0f);
			node->mTransformation = MakeTranslationMatrix(bone.Position - parentPosition);
			boneNodes[i] = node;
		}

		for (size_t i = 0; i < model.Bones.size(); ++i)
		{
			const Bone& bone = model.Bones[i];
			const bool hasParent = bone.ParentIndex >= 0 &&
				bone.ParentIndex < static_cast<int32_t>(model.Bones.size()) &&
				bone.ParentIndex != static_cast<int32_t>(i);
			if (hasParent)
			{
				aiNode* parentNode = boneNodes[static_cast<size_t>(bone.ParentIndex)];
				boneNodes[i]->mParent = parentNode;
				childLists[static_cast<size_t>(bone.ParentIndex)].push_back(boneNodes[i]);
			}
			else
			{
				boneNodes[i]->mParent = scene->mRootNode;
				childLists[rootChildSlot].push_back(boneNodes[i]);
			}
		}

		auto assignChildren = [](aiNode* node, const vector<aiNode*>& children)
			{
				node->mNumChildren = static_cast<unsigned int>(children.size());
				if (children.empty())
				{
					return;
				}
				node->mChildren = new aiNode * [children.size()];
				for (size_t i = 0; i < children.size(); ++i)
				{
					node->mChildren[i] = children[i];
				}
			};

		assignChildren(scene->mRootNode, childLists[rootChildSlot]);
		for (size_t i = 0; i < boneNodes.size(); ++i)
		{
			assignChildren(boneNodes[i], childLists[i]);
		}

		const size_t materialCount = max<size_t>(1, model.Materials.size());
		scene->mNumMaterials = static_cast<unsigned int>(materialCount);
		scene->mMaterials = new aiMaterial * [materialCount];
		for (size_t i = 0; i < materialCount; ++i)
		{
			scene->mMaterials[i] = new aiMaterial();
			if (i < model.Materials.size())
			{
				AddMaterialProperties(scene->mMaterials[i], model.Materials[i], model.Textures);
			}
			else
			{
				Material fallback{};
				fallback.Name = "Default";
				AddMaterialProperties(scene->mMaterials[i], fallback, model.Textures);
			}
		}

		struct MeshBuild
		{
			uint32_t MaterialIndex = 0;
			vector<uint32_t> PmxVertexIndices{};
			vector<unsigned int> Indices{};
		};
		vector<MeshBuild> meshBuilds;

		auto addMaterialMesh = [&](uint32_t materialIndex, size_t indexStart, size_t indexCount)
			{
				if (indexStart >= model.Indices.size())
				{
					return;
				}
				const size_t clampedEnd = min(model.Indices.size(), indexStart + indexCount);
				if (clampedEnd <= indexStart + 2)
				{
					return;
				}

				MeshBuild build{};
				build.MaterialIndex = materialIndex;
				unordered_map<uint32_t, unsigned int> localIndexByPmxVertex;
				for (size_t i = indexStart; i + 2 < clampedEnd; i += 3)
				{
					uint32_t pmxIndices[3] =
					{
						model.Indices[i + 0],
						model.Indices[i + 1],
						model.Indices[i + 2],
					};
					if (pmxIndices[0] >= model.Vertices.size() ||
						pmxIndices[1] >= model.Vertices.size() ||
						pmxIndices[2] >= model.Vertices.size())
					{
						continue;
					}

					for (uint32_t pmxIndex : pmxIndices)
					{
						auto it = localIndexByPmxVertex.find(pmxIndex);
						if (it == localIndexByPmxVertex.end())
						{
							const unsigned int localIndex = static_cast<unsigned int>(build.PmxVertexIndices.size());
							localIndexByPmxVertex[pmxIndex] = localIndex;
							build.PmxVertexIndices.push_back(pmxIndex);
							build.Indices.push_back(localIndex);
						}
						else
						{
							build.Indices.push_back(it->second);
						}
					}
				}

				if (!build.PmxVertexIndices.empty() && build.Indices.size() >= 3)
				{
					meshBuilds.push_back(std::move(build));
				}
			};

		if (!model.Materials.empty())
		{
			size_t indexCursor = 0;
			for (uint32_t materialIndex = 0; materialIndex < static_cast<uint32_t>(model.Materials.size()); ++materialIndex)
			{
				const size_t indexCount = model.Materials[materialIndex].IndexCount;
				addMaterialMesh(materialIndex, indexCursor, indexCount);
				indexCursor += indexCount;
			}
		}
		else
		{
			addMaterialMesh(0, 0, model.Indices.size());
		}

		if (meshBuilds.empty())
		{
			return nullptr;
		}

		scene->mNumMeshes = static_cast<unsigned int>(meshBuilds.size());
		scene->mMeshes = new aiMesh * [meshBuilds.size()];
		outMeshVertexPmxIndices.clear();
		outMeshVertexPmxIndices.reserve(meshBuilds.size());
		for (size_t meshIndex = 0; meshIndex < meshBuilds.size(); ++meshIndex)
		{
			const MeshBuild& build = meshBuilds[meshIndex];
			aiMesh* mesh = new aiMesh();
			scene->mMeshes[meshIndex] = mesh;
			mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
			mesh->mMaterialIndex = build.MaterialIndex;
			const string meshName =
				build.MaterialIndex < model.Materials.size()
				? (!model.Materials[build.MaterialIndex].Name.empty() ? model.Materials[build.MaterialIndex].Name : model.Materials[build.MaterialIndex].EnglishName)
				: "PMXMesh";
			mesh->mName = aiString(meshName.c_str());

			mesh->mNumVertices = static_cast<unsigned int>(build.PmxVertexIndices.size());
			mesh->mVertices = new aiVector3D[mesh->mNumVertices];
			mesh->mNormals = new aiVector3D[mesh->mNumVertices];
			mesh->mTextureCoords[0] = new aiVector3D[mesh->mNumVertices];
			mesh->mNumUVComponents[0] = 2;
			for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
			{
				const Vertex& sourceVertex = model.Vertices[build.PmxVertexIndices[v]];
				mesh->mVertices[v] = ConvertPosition(sourceVertex.Position);
				mesh->mNormals[v] = ConvertPosition(sourceVertex.Normal);
				mesh->mTextureCoords[0][v] = aiVector3D(sourceVertex.TexCoord.x, sourceVertex.TexCoord.y, 0.0f);
			}

			mesh->mNumFaces = static_cast<unsigned int>(build.Indices.size() / 3);
			mesh->mFaces = new aiFace[mesh->mNumFaces];
			for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
			{
				aiFace& face = mesh->mFaces[f];
				face.mNumIndices = 3;
				face.mIndices = new unsigned int[3];
				face.mIndices[0] = build.Indices[f * 3 + 0];
				face.mIndices[1] = build.Indices[f * 3 + 1];
				face.mIndices[2] = build.Indices[f * 3 + 2];
			}

			vector<vector<aiVertexWeight>> weightsByBone(model.Bones.size());
			for (unsigned int localVertex = 0; localVertex < mesh->mNumVertices; ++localVertex)
			{
				const Vertex& sourceVertex = model.Vertices[build.PmxVertexIndices[localVertex]];
				for (int influence = 0; influence < 4; ++influence)
				{
					const int32_t boneIndex = sourceVertex.BoneIndices[influence];
					const float weight = sourceVertex.BoneWeights[influence];
					if (boneIndex < 0 || boneIndex >= static_cast<int32_t>(model.Bones.size()) || weight <= 0.0f)
					{
						continue;
					}
					weightsByBone[static_cast<size_t>(boneIndex)].push_back(aiVertexWeight(localVertex, weight));
				}
			}

			vector<aiBone*> meshBones;
			for (size_t boneIndex = 0; boneIndex < weightsByBone.size(); ++boneIndex)
			{
				const vector<aiVertexWeight>& weights = weightsByBone[boneIndex];
				if (weights.empty())
				{
					continue;
				}

				aiBone* bone = new aiBone();
				bone->mName = aiString(model.Bones[boneIndex].Name.c_str());
				bone->mOffsetMatrix = MakeTranslationMatrix(aiVector3D(
					-model.Bones[boneIndex].Position.x,
					-model.Bones[boneIndex].Position.y,
					-model.Bones[boneIndex].Position.z));
				bone->mNumWeights = static_cast<unsigned int>(weights.size());
				bone->mWeights = new aiVertexWeight[weights.size()];
				for (size_t weightIndex = 0; weightIndex < weights.size(); ++weightIndex)
				{
					bone->mWeights[weightIndex] = weights[weightIndex];
				}
				meshBones.push_back(bone);
			}

			mesh->mNumBones = static_cast<unsigned int>(meshBones.size());
			if (!meshBones.empty())
			{
				mesh->mBones = new aiBone * [meshBones.size()];
				for (size_t i = 0; i < meshBones.size(); ++i)
				{
					mesh->mBones[i] = meshBones[i];
				}
			}

			outMeshVertexPmxIndices.push_back(build.PmxVertexIndices);
		}

		scene->mRootNode->mNumMeshes = scene->mNumMeshes;
		scene->mRootNode->mMeshes = new unsigned int[scene->mNumMeshes];
		for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
		{
			scene->mRootNode->mMeshes[i] = i;
		}

		return scene.release();
	}

	void DestroyGeneratedScene(aiScene* scene)
	{
		if (!scene)
		{
			return;
		}

		auto destroyNode = [](auto&& self, aiNode* node) -> void
			{
				if (!node)
				{
					return;
				}

				aiNode** children = node->mChildren;
				const unsigned int childCount = node->mNumChildren;
				node->mChildren = nullptr;
				node->mNumChildren = 0;
				for (unsigned int i = 0; i < childCount; ++i)
				{
					self(self, children[i]);
				}
				delete[] children;

				delete[] node->mMeshes;
				node->mMeshes = nullptr;
				node->mNumMeshes = 0;
				delete node;
			};

		destroyNode(destroyNode, scene->mRootNode);
		scene->mRootNode = nullptr;

		for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
		{
			aiMesh* mesh = scene->mMeshes ? scene->mMeshes[meshIndex] : nullptr;
			if (!mesh)
			{
				continue;
			}

			delete[] mesh->mVertices;
			mesh->mVertices = nullptr;
			delete[] mesh->mNormals;
			mesh->mNormals = nullptr;
			for (unsigned int i = 0; i < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++i)
			{
				delete[] mesh->mTextureCoords[i];
				mesh->mTextureCoords[i] = nullptr;
			}
			for (unsigned int i = 0; i < AI_MAX_NUMBER_OF_COLOR_SETS; ++i)
			{
				delete[] mesh->mColors[i];
				mesh->mColors[i] = nullptr;
			}
			for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
			{
				delete[] mesh->mFaces[faceIndex].mIndices;
				mesh->mFaces[faceIndex].mIndices = nullptr;
				mesh->mFaces[faceIndex].mNumIndices = 0;
			}
			delete[] mesh->mFaces;
			mesh->mFaces = nullptr;
			mesh->mNumFaces = 0;

			for (unsigned int boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex)
			{
				aiBone* bone = mesh->mBones ? mesh->mBones[boneIndex] : nullptr;
				if (!bone)
				{
					continue;
				}
				delete[] bone->mWeights;
				bone->mWeights = nullptr;
				bone->mNumWeights = 0;
				delete bone;
			}
			delete[] mesh->mBones;
			mesh->mBones = nullptr;
			mesh->mNumBones = 0;
			mesh->mNumVertices = 0;
			delete mesh;
		}
		delete[] scene->mMeshes;
		scene->mMeshes = nullptr;
		scene->mNumMeshes = 0;

		for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
		{
			delete scene->mMaterials[materialIndex];
		}
		delete[] scene->mMaterials;
		scene->mMaterials = nullptr;
		scene->mNumMaterials = 0;

		delete scene;
	}

	void PopulateAnimationMetadata(
		const Model& model,
		vector<PmxAppendConstraint>& appendConstraints,
		vector<PmxIkConstraint>& ikConstraints,
		vector<aiVector3D>& baseVertices,
		vector<aiVector3D>& baseNormals,
		vector<XMFLOAT2>& baseTexCoords,
		vector<PmxMorph>& morphs,
		unordered_map<string, uint32_t>& morphIndexMap)
	{
		baseVertices.clear();
		baseNormals.clear();
		baseTexCoords.clear();
		baseVertices.reserve(model.Vertices.size());
		baseNormals.reserve(model.Vertices.size());
		baseTexCoords.reserve(model.Vertices.size());
		for (const Vertex& vertex : model.Vertices)
		{
			baseVertices.push_back(ConvertPosition(vertex.Position));
			baseNormals.push_back(ConvertPosition(vertex.Normal));
			baseTexCoords.push_back(vertex.TexCoord);
		}

		morphs = model.Morphs;
		morphIndexMap.clear();
		for (uint32_t i = 0; i < static_cast<uint32_t>(morphs.size()); ++i)
		{
			if (!morphs[i].Name.empty())
			{
				morphIndexMap[morphs[i].Name] = i;
			}
		}

		appendConstraints.clear();
		for (uint32_t i = 0; i < static_cast<uint32_t>(model.Bones.size()); ++i)
		{
			const Bone& bone = model.Bones[i];
			if (((bone.Flags & 0x0100) == 0 && (bone.Flags & 0x0200) == 0) ||
				bone.AppendBoneIndex < 0 ||
				bone.AppendBoneIndex >= static_cast<int32_t>(model.Bones.size()))
			{
				continue;
			}

			PmxAppendConstraint constraint{};
			constraint.BoneName = bone.Name;
			constraint.AppendBoneName = model.Bones[static_cast<size_t>(bone.AppendBoneIndex)].Name;
			constraint.Weight = bone.AppendWeight;
			constraint.InheritRotation = (bone.Flags & 0x0100) != 0;
			constraint.InheritTranslation = (bone.Flags & 0x0200) != 0;
			constraint.Local = (bone.Flags & 0x0080) != 0;
			constraint.DeformDepth = bone.DeformDepth;
			constraint.BoneOrder = i;
			if (!constraint.BoneName.empty() && !constraint.AppendBoneName.empty())
			{
				appendConstraints.push_back(std::move(constraint));
			}
		}

		ikConstraints.clear();
		for (uint32_t i = 0; i < static_cast<uint32_t>(model.Bones.size()); ++i)
		{
			const Bone& bone = model.Bones[i];
			if ((bone.Flags & 0x0020) == 0 ||
				bone.IkTargetIndex < 0 ||
				bone.IkTargetIndex >= static_cast<int32_t>(model.Bones.size()))
			{
				continue;
			}

			PmxIkConstraint constraint{};
			constraint.BoneName = bone.Name;
			constraint.TargetBoneName = model.Bones[static_cast<size_t>(bone.IkTargetIndex)].Name;
			constraint.IterationCount = bone.IkIterationCount;
			constraint.LimitAngle = bone.IkLimitAngle;
			constraint.DeformDepth = bone.DeformDepth;
			constraint.BoneOrder = i;
			for (const IkLink& rawLink : bone.IkLinks)
			{
				if (rawLink.BoneIndex < 0 || rawLink.BoneIndex >= static_cast<int32_t>(model.Bones.size()))
				{
					continue;
				}

				PmxIkLink link{};
				link.BoneName = model.Bones[static_cast<size_t>(rawLink.BoneIndex)].Name;
				link.HasLimit = rawLink.HasLimit;
				link.LimitMin = rawLink.LimitMin;
				link.LimitMax = rawLink.LimitMax;
				constraint.Links.push_back(link);
			}

			if (!constraint.BoneName.empty() && !constraint.TargetBoneName.empty() && !constraint.Links.empty())
			{
				ikConstraints.push_back(std::move(constraint));
			}
		}

		auto compareTransformOrder = [](const auto& lhs, const auto& rhs)
			{
				if (lhs.DeformDepth != rhs.DeformDepth)
				{
					return lhs.DeformDepth < rhs.DeformDepth;
				}
				return lhs.BoneOrder < rhs.BoneOrder;
			};
		sort(appendConstraints.begin(), appendConstraints.end(), compareTransformOrder);
		sort(ikConstraints.begin(), ikConstraints.end(), compareTransformOrder);

		size_t positionMorphCount = 0;
		size_t uvMorphCount = 0;
		size_t boneMorphCount = 0;
		size_t materialMorphCount = 0;
		size_t groupMorphCount = 0;
		for (const PmxMorph& morph : morphs)
		{
			if (!morph.PositionOffsets.empty()) ++positionMorphCount;
			if (!morph.UvOffsets.empty()) ++uvMorphCount;
			if (!morph.BoneOffsets.empty()) ++boneMorphCount;
			if (!morph.MaterialOffsets.empty()) ++materialMorphCount;
			if (!morph.GroupOffsets.empty()) ++groupMorphCount;
		}

		Debug::Log("PMX animation metadata loaded from binary model (vertices=%zu, bones=%zu, appendConstraints=%zu, ikConstraints=%zu, morphs=%zu, positionMorphs=%zu, uvMorphs=%zu, boneMorphs=%zu, materialMorphs=%zu, groupMorphs=%zu)\n",
			baseVertices.size(), model.Bones.size(), appendConstraints.size(), ikConstraints.size(), morphs.size(),
			positionMorphCount, uvMorphCount, boneMorphCount, materialMorphCount, groupMorphCount);
	}

	void ApplyVertexDeformData(
		const Model& model,
		const vector<vector<uint32_t>>& meshVertexPmxIndices,
		uint32_t meshIndex,
		uint32_t vertexIndex,
		GpuSkinVertex& gpuVertex)
	{
		if (meshIndex >= meshVertexPmxIndices.size() ||
			vertexIndex >= meshVertexPmxIndices[meshIndex].size())
		{
			return;
		}

		const uint32_t pmxVertexIndex = meshVertexPmxIndices[meshIndex][vertexIndex];
		if (pmxVertexIndex >= model.Vertices.size())
		{
			return;
		}

		const Vertex& sourceVertex = model.Vertices[pmxVertexIndex];
		gpuVertex.DeformType = sourceVertex.DeformType;
		gpuVertex.SdefC = XMFLOAT4(sourceVertex.SdefC.x, sourceVertex.SdefC.y, sourceVertex.SdefC.z, 0.0f);
		gpuVertex.SdefR0 = XMFLOAT4(sourceVertex.SdefR0.x, sourceVertex.SdefR0.y, sourceVertex.SdefR0.z, 0.0f);
		gpuVertex.SdefR1 = XMFLOAT4(sourceVertex.SdefR1.x, sourceVertex.SdefR1.y, sourceVertex.SdefR1.z, 0.0f);
	}
}
