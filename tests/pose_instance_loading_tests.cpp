#include <cassert>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace std;

struct ID3D12Device
{
};

struct Vertex
{
    struct
    {
        float w = 10.0f;
    } Diffuse;
};

// Replace file/GPU IO at the resource boundary; exercise the real pose-instance loading function.
class AnimationModelResource
{
  public:
    string m_SourceModelPath = "model.fbx";
    bool m_SourceConvert = true;
    vector<string> m_BoneNames{"root"};
    vector<int> m_Meshes{0};
    int m_ResourceMap = 42;
    float m_VmdTranslationScale = 2.0f;
    vector<int> m_VmdRetargetTransforms{3};
    map<string, string> m_AnimationSourcePaths;
    vector<vector<Vertex>> m_BaseGpuSkinVertices{{Vertex{}}};
    vector<string> LoadedClips;
    vector<int> PartIds;

    bool Load(const char*, ID3D12Device*, bool)
    {
        return true;
    }

    bool LoadAnimation(const char* path, const char*)
    {
        LoadedClips.push_back(path);
        return string(path) != "missing.vmd";
    }

    bool ApplyMeshShadingOverridePartIds(const vector<int>& ids)
    {
        PartIds = ids;
        return true;
    }

    void Uninit() {}

    bool LoadPoseInstance(const AnimationModelResource& source, ID3D12Device* device, bool copyAnimations = true);
};

#include "pose-instance-loading.inc"

int main()
{
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    AnimationModelResource source;
    source.m_AnimationSourcePaths["old"] = "missing.vmd";
    AnimationModelResource preview;
    assert(preview.LoadPoseInstance(source, nullptr, false));
    assert(preview.LoadedClips.empty());
    assert(preview.m_ResourceMap == 42 && preview.m_VmdTranslationScale == 2.0f);
    assert(preview.m_VmdRetargetTransforms == vector<int>({3}));
    assert(preview.PartIds == vector<int>({10}));
    assert(preview.LoadAnimation("replacement.vmd", "body"));
    assert(source.LoadedClips.empty());
    AnimationModelResource existingCaller;
    assert(!existingCaller.LoadPoseInstance(source, nullptr));
    assert(existingCaller.LoadedClips == vector<string>({"missing.vmd"}));
    cout << "Pose instance replacement preserves bindings and skips removed clips\n";
}
