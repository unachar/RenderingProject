#include "models/lookatsolver.h"
#include <json/json.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
using namespace HumanoidIk;
int main()
{
    std::ifstream file(std::filesystem::path("asset/model/moca.vrm"),std::ios::binary);
    assert(file);
    auto u32=[&]() {
        unsigned char b[4]{};file.read(reinterpret_cast<char*>(b),4);
        return uint32_t(b[0])|uint32_t(b[1])<<8|uint32_t(b[2])<<16|uint32_t(b[3])<<24;
    };
    assert(u32()==0x46546c67); assert(u32()==2); u32();
    const uint32_t size=u32(); assert(u32()==0x4e4f534a && size<32*1024*1024);
    std::string text(size,'\0'); file.read(text.data(),size); assert(file);
    Json::CharReaderBuilder reader;
    Json::Value root; std::string errors;
    std::unique_ptr<Json::CharReader> parser(reader.newCharReader());
    assert(parser->parse(text.data(),text.data()+text.size(),&root,&errors));
    const auto& nodes=root["nodes"];
    std::vector<int> parents(nodes.size(),-1),runtimeIndex(nodes.size(),-1);
    for(Json::ArrayIndex i=0;i<nodes.size();++i)
        for(const auto& child:nodes[i]["children"]) parents[child.asUInt()]=static_cast<int>(i);
    Pose bind;
    std::function<void(unsigned,int)> visit=[&](unsigned source,int parent) {
        const auto& input=nodes[source];
        Node node; node.Parent=parent;
        aiVector3D position,scale(1,1,1);aiQuaternion rotation;
        if(input["translation"].isArray()) position={input["translation"][0].asFloat(),input["translation"][1].asFloat(),input["translation"][2].asFloat()};
        if(input["scale"].isArray()) scale={input["scale"][0].asFloat(),input["scale"][1].asFloat(),input["scale"][2].asFloat()};
        if(input["rotation"].isArray()) rotation={input["rotation"][3].asFloat(),input["rotation"][0].asFloat(),input["rotation"][1].asFloat(),input["rotation"][2].asFloat()};
        node.Local=aiMatrix4x4(scale,rotation,position);
        if(input["matrix"].size()==16)
            for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)node.Local[r][c]=input["matrix"][c*4+r].asFloat();
        const int index=static_cast<int>(bind.Nodes.size());runtimeIndex[source]=index;bind.Nodes.push_back(node);
        for(const auto& child:input["children"]) visit(child.asUInt(),index);
    };
    for(unsigned i=0;i<nodes.size();++i) if(parents[i]<0)visit(i,-1);
    assert(bind.CaptureBindPose());
    const auto& human=root["extensions"]["VRMC_vrm"]["humanoid"]["humanBones"];
    auto role=[&](const std::string& name){return runtimeIndex[human[name]["node"].asUInt()];};
    Gaze::Rig rig;
    rig.Bones={role("neck"),role("head"),role("leftEye"),role("rightEye")};
    for(int bone:rig.Bones) assert(bind.Contains(bone));
    LookAtSettings settings;
    const auto forward=Unit(bind.Position(role("leftToes"))-bind.Position(role("leftFoot")));
    settings.ModelForward=Unit(forward-aiVector3D(0,1,0)*forward.y);
    const auto right=Unit(aiVector3D(0,1,0)^settings.ModelForward);
    const auto center=(bind.Position(rig.Bones[2])+bind.Position(rig.Bones[3]))*0.5f;
    const float scale=(bind.Position(role("head"))-bind.Position(role("neck"))).Length();
    const auto target=center+settings.ModelForward*(scale*30)+right*(scale*18)+aiVector3D(0,scale*4,0);
    Gaze::State state; Pose result;
    for(int i=0;i<300;++i)
    {
        result=bind;
        assert(Gaze::Solve(result,rig,state,target,settings,true,1.0f/60));
    }
    assert(state.Angles[0].Yaw>0.01f && state.Angles[1].Yaw>0.01f);
    for(int eye : {rig.Bones[2],rig.Bones[3]})
    {
        const auto actual=Gaze::AnimationFrame(result,eye).Rotate(settings.ModelForward);
        assert(std::acos(std::clamp(Unit(actual)*Unit(target-result.Position(eye)),-1.0f,1.0f))<0.02f);
        const auto i=static_cast<size_t>(eye);
        assert((Translation(result.Nodes[i].Local)-Translation(bind.Nodes[i].Local)).Length()<1e-6f);
    }
    std::cout << "LookAt Moca VRM fixture: PASS (neck/head/eyes, bind axes and translations)\n";
}
