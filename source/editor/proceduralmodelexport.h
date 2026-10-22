#pragma once

#include "proceduralstudio.h"

namespace ProceduralIO
{
    // 拡張子で OBJ / FBX (ASCII) を選ぶ。テクスチャは同じフォルダのファイル名で参照する。
    bool SaveModelFile(const std::filesystem::path& path, const ProcStudio::Mesh& mesh, bool vertexColors,
                       const std::string& diffuseTexture, std::string& error);
}
