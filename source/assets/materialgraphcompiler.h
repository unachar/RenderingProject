#pragma once
#include "materialgraph.h"
#include <unordered_map>

struct MaterialGraphSourceLocation
{
    uint64_t Node = 0;
    std::string File;
    size_t Line = 0;
};

struct MaterialGraphProgram
{
    bool Success = false;
    bool Animated = false;
    bool MaterialOutput = false;
    std::string Hlsl;
    std::string Error;
    uint64_t ShaderHash = 0;
    std::vector<std::array<float, 4>> Parameters;
    std::vector<uint64_t> ParameterNodes;
    std::vector<AssetHandle> Textures;
    std::vector<MaterialGraphSourceLocation> Sources;
    std::vector<uint64_t> DependencyOrder;
};

class MaterialGraphCompiler
{
  public:
    // targetNode==0 compiles Material Output; preview compilation visits only the chosen node's inputs.
    static MaterialGraphProgram Generate(const MaterialGraph& graph, uint64_t targetNode = 0);
};
