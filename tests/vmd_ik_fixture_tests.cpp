#include "models/humanoidik.h"
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
    for(const std::string side:{"left","right"})
    {
        Pose pose=bind;
        Leg leg{role(side+"UpperLeg"),role(side+"LowerLeg"),role(side+"Foot"),role(side+"Toes"),{0,0,1}};
        assert(ValidLeg(pose,leg));
        const auto hip=pose.Position(leg.Hip),foot=pose.Position(leg.Foot),toe=pose.Position(leg.Toe);
        const auto up=Unit(hip-foot);
        leg.Pole=Unit(toe-foot-up*((toe-foot)*up));
        Target target;target.FootEnabled=true;target.ToeEnabled=true;
        target.FootPosition=foot+up*0.05f+leg.Pole*0.01f;
        target.FootRotation=Rotation(pose.Nodes[static_cast<size_t>(leg.Foot)].Global);
        target.ToePosition=target.FootPosition+toe-foot+up*0.01f;
        const auto kneeTranslation=Translation(pose.Nodes[static_cast<size_t>(leg.Knee)].Local);
        const auto footTranslation=Translation(pose.Nodes[static_cast<size_t>(leg.Foot)].Local);
        assert(Apply(pose,leg,target));
        assert((pose.Position(leg.Foot)-target.FootPosition).Length()<0.001f);
        assert((Translation(pose.Nodes[static_cast<size_t>(leg.Knee)].Local)-kneeTranslation).Length()<1e-6f);
        assert((Translation(pose.Nodes[static_cast<size_t>(leg.Foot)].Local)-footTranslation).Length()<1e-6f);
        assert(Finite(pose.Position(leg.Toe)));
    }
    std::cout<<"VMD IK Moca VRM fixture: PASS (both legs/toes; original joint transforms)\n";
}
