#pragma once

#include "systembase.h"
#include "timeline.h"
#include "timelinecomponent.h"

class TimeLineSystem : public SystemBase
{
  private:
    static vector<unique_ptr<TimeLineDirector>> m_Directors;

  public:
    void Update() override;
    void Uninit() override;

    static TimeLineDirector& CreateDirector();
    static void Clear();
    static void SuspendAudio(bool close = false);
    static void EvaluateComponent(TimelineComponent& timeline);
    static bool PrepareTrackAnimations(TimelineTrackData& track);
    static XMFLOAT4 ReadProperty(EntityID target, TimelineProperty property, bool* valid = nullptr, int fogVolume = 0);
};
