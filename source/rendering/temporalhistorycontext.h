#pragma once
#include "temporalhistory.h"

namespace TemporalHistory
{
    // 描画スレッドのコンテキスト。世代は永続的で、エフェクトによって消費されないため、
    // TAA、反射、GI が同じ変更をそれぞれ独立して認識できる。
    class Context final
    {
      public:
        static void BeginFrame(uint32_t outputWidth, uint32_t outputHeight)
        {
            ++s_Frame.Serial;
            s_Frame.OutputWidth = outputWidth;
            s_Frame.OutputHeight = outputHeight;
        }

        static uint64_t GetFrameSerial()
        {
            return s_Frame.Serial;
        }

        static State Capture(uint64_t cameraId, uint32_t inputWidth, uint32_t inputHeight, uint64_t configurationKey,
                             double exposure = 1.0)
        {
            State state = s_Frame;
            state.CameraId = cameraId;
            state.InputWidth = inputWidth;
            state.InputHeight = inputHeight;
            state.ConfigurationKey = configurationKey;
            state.Exposure = exposure;
            return state;
        }

        static void NotifySceneChanged()
        {
            ++s_Frame.SceneGeneration;
        }

        static void NotifyLightingChanged()
        {
            ++s_Frame.LightingGeneration;
        }

        static void NotifyLodChanged()
        {
            ++s_Frame.LodGeneration;
        }

        static void NotifyCameraCut()
        {
            ++s_Frame.CameraCutGeneration;
        }

      private:
        static inline State s_Frame;
    };
} // 名前空間 TemporalHistory
