#pragma once
#include "types.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace HumanoidIk
{
inline bool Finite(const aiVector3D& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline aiVector3D Translation(const aiMatrix4x4& m) { return {m.a4,m.b4,m.c4}; }
inline aiQuaternion Rotation(const aiMatrix4x4& m)
{
    aiVector3D scale,position;
    aiQuaternion rotation;
    m.Decompose(scale,rotation,position);
    return rotation.Normalize();
}
inline aiVector3D Unit(aiVector3D value, const aiVector3D& fallback={0,0,1})
{
    const float length=value.Length();
    return std::isfinite(length) && length>1e-8f ? value/length : fallback;
}
inline aiVector3D Perpendicular(const aiVector3D& axis)
{
    const aiVector3D reference=std::abs(axis.z)<0.9f ? aiVector3D(0,0,1) : aiVector3D(1,0,0);
    return Unit(reference-axis*(reference*axis));
}
inline aiQuaternion FromTo(aiVector3D from, aiVector3D to)
{
    from=Unit(from); to=Unit(to);
    const float dot=std::clamp(from*to,-1.0f,1.0f);
    if (dot>1-1e-7f) return {};
    if (dot<-1+1e-6f) return aiQuaternion(Perpendicular(from),3.14159265358979323846f);
    const auto axis=from^to;
    return aiQuaternion(1+dot,axis.x,axis.y,axis.z).Normalize();
}
inline bool RigidScale(const aiMatrix4x4& m)
{
    for (unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) if(!std::isfinite(m[r][c])) return false;
    const aiVector3D x(m.a1,m.b1,m.c1),y(m.a2,m.b2,m.c2),z(m.a3,m.b3,m.c3);
    const float scale2=x.SquareLength();
    if(scale2<1e-12f || !std::isfinite(scale2)) return false;
    const float tolerance=scale2*0.0001f;
    return std::abs(y.SquareLength()-scale2)<tolerance && std::abs(z.SquareLength()-scale2)<tolerance &&
           std::abs(x*y)<tolerance && std::abs(x*z)<tolerance && std::abs(y*z)<tolerance && (x^(y))*z>0;
}
struct Node
{
    int Parent=-1;
    aiMatrix4x4 Local{}, Global{}, BindGlobal{};
};
struct Pose
{
    std::vector<Node> Nodes;
    bool UpdateGlobals()
    {
        for(size_t i=0;i<Nodes.size();++i)
        {
            auto& node=Nodes[i];
            if(node.Parent < -1 || node.Parent>=static_cast<int>(i)) return false;
            node.Global=node.Parent<0 ? node.Local : Nodes[static_cast<size_t>(node.Parent)].Global*node.Local;
        }
        return true;
    }
    bool CaptureBindPose()
    {
        if(!UpdateGlobals()) return false;
        for(auto& node:Nodes) node.BindGlobal=node.Global;
        return true;
    }
    bool Contains(int index) const { return index>=0 && static_cast<size_t>(index)<Nodes.size(); }
    aiVector3D Position(int index) const { return Translation(Nodes[static_cast<size_t>(index)].Global); }
    bool DescendsFrom(int child,int ancestor) const
    {
        for(size_t step=0;Contains(child) && step<Nodes.size();++step)
        {
            if(child==ancestor) return true;
            child=Nodes[static_cast<size_t>(child)].Parent;
        }
        return false;
    }
    void RotateWorld(int index, const aiQuaternion& delta)
    {
        auto& node=Nodes[static_cast<size_t>(index)];
        aiQuaternion world=delta*Rotation(node.Global);
        aiQuaternion parent=node.Parent<0 ? aiQuaternion() : Rotation(Nodes[static_cast<size_t>(node.Parent)].Global);
        parent.Conjugate();
        aiQuaternion local=parent*world;
        local.Normalize();
        aiVector3D scale,position; aiQuaternion old;
        node.Local.Decompose(scale,old,position);
        node.Local=aiMatrix4x4(scale,local,position);
        UpdateGlobals();
    }
};
struct Leg
{
    int Hip=-1,Knee=-1,Foot=-1,Toe=-1;
    aiVector3D Pole{0,0,1}; // Preferred model-space knee bend, refreshed from FK/bind by the caller.
};
struct Target
{
    bool FootEnabled=false,ToeEnabled=false;
    aiVector3D FootPosition{},ToePosition{};
    aiQuaternion FootRotation{};
};
inline bool ValidLeg(const Pose& pose,const Leg& leg)
{
    if(!pose.Contains(leg.Hip) || !pose.Contains(leg.Knee) || !pose.Contains(leg.Foot) ||
       leg.Hip==leg.Knee || leg.Knee==leg.Foot || leg.Foot==leg.Hip ||
       !pose.DescendsFrom(leg.Knee,leg.Hip) || !pose.DescendsFrom(leg.Foot,leg.Knee)) return false;
    // Global rigidity alone can hide a nonuniform parent compensated by a
    // sheared local matrix. RotateWorld decomposes both; reject before mutation.
    int index=leg.Foot;
    for(size_t step=0;pose.Contains(index) && step<pose.Nodes.size();++step)
    {
        const auto& node=pose.Nodes[static_cast<size_t>(index)];
        if(!RigidScale(node.Local) || !RigidScale(node.Global)) return false;
        index=node.Parent;
    }
    return index==-1;
}
inline bool Apply(Pose& pose,const Leg& leg,const Target& target)
{
    if(!target.FootEnabled && !target.ToeEnabled) return false;
    if(!pose.UpdateGlobals() || !ValidLeg(pose,leg)) return false;
    const auto hip=pose.Position(leg.Hip),knee=pose.Position(leg.Knee),foot=pose.Position(leg.Foot);
    const float upper=(knee-hip).Length(),lower=(foot-knee).Length();
    if(!std::isfinite(upper+lower) || upper<1e-6f || lower<1e-6f) return false;
    bool solved=false;
    if(target.FootEnabled)
    {
        if(!Finite(target.FootPosition) || !std::isfinite(target.FootRotation.w) ||
           !std::isfinite(target.FootRotation.x) || !std::isfinite(target.FootRotation.y) ||
           !std::isfinite(target.FootRotation.z) ||
           target.FootRotation.w*target.FootRotation.w + target.FootRotation.x*target.FootRotation.x +
           target.FootRotation.y*target.FootRotation.y + target.FootRotation.z*target.FootRotation.z < 1e-12f) return false;
        const auto direction=Unit(target.FootPosition-hip,Unit(foot-hip,{0,-1,0}));
        const float minimum=std::max(std::abs(upper-lower),(upper+lower)*1e-6f);
        const float distance=std::clamp((target.FootPosition-hip).Length(),minimum,upper+lower);
        aiVector3D bend=leg.Pole-direction*(leg.Pole*direction);
        bend=Unit(bend,Perpendicular(direction));
        const float along=(upper*upper-lower*lower+distance*distance)/(2*distance);
        const float height=std::sqrt(std::max(0.0f,upper*upper-along*along));
        const auto desiredKnee=hip+direction*along+bend*height;
        const auto desiredFoot=hip+direction*distance;
        pose.RotateWorld(leg.Hip,FromTo(knee-hip,desiredKnee-hip));
        pose.RotateWorld(leg.Knee,FromTo(pose.Position(leg.Foot)-pose.Position(leg.Knee),desiredFoot-pose.Position(leg.Knee)));
        auto inverse=Rotation(pose.Nodes[static_cast<size_t>(leg.Foot)].Global);
        inverse.Conjugate();
        pose.RotateWorld(leg.Foot,target.FootRotation*inverse);
        solved=true;
    }
    if(target.ToeEnabled && pose.Contains(leg.Toe) && leg.Toe!=leg.Foot && pose.DescendsFrom(leg.Toe,leg.Foot) &&
       Finite(target.ToePosition))
    {
        const auto current=pose.Position(leg.Toe)-pose.Position(leg.Foot);
        const auto desired=target.ToePosition-pose.Position(leg.Foot);
        if(current.SquareLength()>1e-12f && desired.SquareLength()>1e-12f)
        {
            pose.RotateWorld(leg.Foot,FromTo(current,desired));
            solved=true;
        }
    }
    return solved;
}
inline aiQuaternion Slerp(const aiQuaternion& a,const aiQuaternion& b,float weight)
{
    aiQuaternion result;
    aiQuaternion::Interpolate(result,a,b,weight);
    return result.Normalize();
}
inline Target Blend(const Target& a,const Target& b,float blend,const Target& fk)
{
    blend=std::clamp(blend,0.0f,1.0f);
    const float wa=a.FootEnabled ? 1-blend : 0,wb=b.FootEnabled ? blend : 0;
    Target result;
    result.FootEnabled=wa+wb>1e-6f;
    result.FootPosition=a.FootPosition*wa+b.FootPosition*wb+fk.FootPosition*(1-wa-wb);
    if(wa>0 && wb>0) result.FootRotation=Slerp(a.FootRotation,b.FootRotation,wb/(wa+wb));
    else if(result.FootEnabled) result.FootRotation=Slerp(fk.FootRotation,wa>0 ? a.FootRotation : b.FootRotation,wa+wb);
    else result.FootRotation=fk.FootRotation;
    const float ta=a.ToeEnabled ? 1-blend : 0,tb=b.ToeEnabled ? blend : 0;
    result.ToeEnabled=ta+tb>1e-6f;
    result.ToePosition=a.ToePosition*ta+b.ToePosition*tb+fk.ToePosition*(1-ta-tb);
    return result;
}
struct Offsets
{
    bool FootEnabled=false,ToeEnabled=false;
    aiVector3D FootPosition{},ToePosition{},RootPosition{},ParentPosition{};
    aiQuaternion FootRotation{},RootRotation{},ParentRotation{};
};
inline Target FromOffsets(const aiVector3D& bindFoot,const aiQuaternion& bindFootRotation,
                          const aiVector3D& bindToe,const Offsets& offset,float scale)
{
    Target target;
    target.FootEnabled=offset.FootEnabled;
    target.ToeEnabled=offset.ToeEnabled;
    const auto localFoot=bindFoot+(offset.ParentPosition+offset.ParentRotation.Rotate(offset.FootPosition))*scale;
    target.FootPosition=offset.RootRotation.Rotate(localFoot)+offset.RootPosition*scale;
    target.FootRotation=offset.RootRotation*offset.ParentRotation*offset.FootRotation*bindFootRotation;
    target.FootRotation.Normalize();
    const auto toeOffset=offset.ParentRotation.Rotate(offset.FootRotation.Rotate(bindToe-bindFoot+offset.ToePosition*scale));
    target.ToePosition=offset.RootRotation.Rotate(localFoot+toeOffset)+offset.RootPosition*scale;
    return target;
}
}
