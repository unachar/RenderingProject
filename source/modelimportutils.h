#pragma once

#include "cimport.h"
#include "scene.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>


	inline std::filesystem::path ModelPathFromUtf8(const char* value)
	{
		return std::filesystem::u8path(value ? value : "");
	}

	inline std::string ModelPathToUtf8(const std::filesystem::path& path)
	{
		const auto value = path.u8string();
		return std::string(reinterpret_cast<const char*>(value.data()), value.size());
	}

	inline std::string ModelPathLowerExtension(const std::filesystem::path& path)
	{
		std::string extension = ModelPathToUtf8(path.extension());
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c)
			{
				return static_cast<char>(std::tolower(c));
			});
		return extension;
	}

	inline const aiScene* ImportModelScene(const char* fileName, unsigned int flags)
	{
		const std::filesystem::path path = ModelPathFromUtf8(fileName);
		const std::string extension = ModelPathLowerExtension(path);
		const char* formatHint = nullptr;
		if (extension == ".vrm")
		{
			formatHint = "glb";
		}
		else if (extension == ".pmx")
		{
			formatHint = "pmx";
		}
		else if (extension == ".vmd")
		{
			formatHint = "vmd";
		}

		if (!formatHint)
		{
			return aiImportFile(fileName, flags);
		}

		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream)
		{
			return nullptr;
		}

		const std::streamsize size = stream.tellg();
		if (size <= 0)
		{
			return nullptr;
		}

		std::vector<char> bytes(static_cast<size_t>(size));
		stream.seekg(0, std::ios::beg);
		if (!stream.read(bytes.data(), size))
		{
			return nullptr;
		}

		return aiImportFileFromMemory(
			bytes.data(), static_cast<unsigned int>(bytes.size()), flags, formatHint);
	}
