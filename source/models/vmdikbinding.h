#pragma once
#include <array>
#include <string>
#include <unordered_map>

namespace VmdIkBinding
{
using Names=std::array<const char*,3>;
inline Names FootNames(unsigned side) { return side ? Names{"右足ＩＫ","右足IK","右足ＩK"} : Names{"左足ＩＫ","左足IK","左足ＩK"}; }
inline Names ToeNames(unsigned side) { return side ? Names{"右つま先ＩＫ","右つま先IK","右つま先ＩK"} : Names{"左つま先ＩＫ","左つま先IK","左つま先ＩK"}; }
inline Names ParentNames(unsigned side) { return side ? Names{"右足IK親","右足ＩＫ親","右足ＩK親"} : Names{"左足IK親","左足ＩＫ親","左足ＩK親"}; }
template<class Map> inline const typename Map::mapped_type* Find(const Map& tracks,const Names& names)
{
    for(const char* name:names)
    {
        const auto track=tracks.find(name);
        if(track!=tracks.end()) return &track->second;
    }
    return nullptr;
}
// Reused by single/blend and layer caches. Missing/empty tracks preserve the
// earlier layer; motion and switches have independent layer ownership.
template<class BoneMap,class SwitchMap,class Channel> inline void UpdateChannel(
    const BoneMap& bones,const SwitchMap& switches,size_t layer,unsigned side,Channel& channel)
{
    auto select = [&](const auto* track,auto& destination) {
        if(track && !track->empty())
        {
            destination.Track=track;
            destination.LayerIndex=layer;
            destination.Cursor={};
        }
    };
    select(Find(bones,FootNames(side)),channel.Foot);
    select(Find(bones,ToeNames(side)),channel.Toe);
    select(Find(bones,ParentNames(side)),channel.Parent);
    select(Find(bones,Names{"全ての親","全ての親","全ての親"}),channel.Root);
    select(Find(switches,FootNames(side)),channel.FootSwitch);
    select(Find(switches,ToeNames(side)),channel.ToeSwitch);
}

}
