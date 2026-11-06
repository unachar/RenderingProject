#pragma once
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// シーケンサーとエフェクト編集で共有する時間軸。座標とスナップの意味を揃える。
namespace TimelineWidgets
{
    struct Axis
    {
        float Left = 0.0f;
        float Start = 0.0f;
        float PixelsPerSecond = 80.0f;

        float ToX(float time) const
        {
            return Left + (time - Start) * PixelsPerSecond;
        }

        float ToTime(float x) const
        {
            return Start + (x - Left) / (std::max)(1.0f, PixelsPerSecond);
        }
    };

    inline float SnapFrame(float time, float frameRate)
    {
        return std::round(time * frameRate) / (std::max)(1.0f, frameRate);
    }

    template <typename Format>
    void DrawTicks(ImDrawList* draw, const Axis& axis, ImVec2 min, ImVec2 max, float major, float minor, float origin,
                   ImU32 textColor, ImU32 dimColor, Format format)
    {
        const float end = axis.ToTime(max.x);
        const auto ticks = [&](float step, float height, ImU32 color, bool labels) {
            if (!std::isfinite(step) || step <= 0.0f)
            {
                return;
            }
            const double first = std::floor((axis.Start - origin) / step);
            for (int index = 0; index < 4000; ++index)
            {
                const float time = origin + static_cast<float>(first + index) * step;
                if (time > end + step)
                {
                    break;
                }
                const float x = axis.ToX(time);
                draw->AddLine({x, max.y - height}, {x, max.y}, color);
                if (labels)
                {
                    char label[48]{};
                    format(time, major, label, sizeof(label));
                    draw->AddText({x + 3.0f, min.y + 2.0f}, time < 0.0f ? dimColor : textColor, label);
                }
            }
        };
        ticks(minor, 5.0f, IM_COL32(255, 255, 255, 60), false);
        ticks(major, 11.0f, IM_COL32(255, 255, 255, 130), true);
    }

    inline void DrawPlayhead(ImDrawList* draw, float x, ImVec2 min, ImVec2 max, ImU32 color)
    {
        draw->AddTriangleFilled({x - 6.0f, min.y + 10.0f}, {x + 6.0f, min.y + 10.0f}, {x, max.y - 2.0f}, color);
        draw->AddRectFilled({x - 6.0f, min.y}, {x + 6.0f, min.y + 10.0f}, color, 2.0f);
    }

    inline float ScrubTime(const Axis& axis, float mouseX, float duration, bool snap, float fps)
    {
        const float time = axis.ToTime(mouseX);
        return (std::clamp)(snap ? SnapFrame(time, fps) : time, 0.0f, duration);
    }
}
