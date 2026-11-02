// ============================================================================
// SequencerWindow : ライブ演出用タイムライン UI（Dear ImGui + ImDrawList）。
//  データ操作は TimelineCurve、エンジン連携は ISequencerBackend を経由する。
// ============================================================================
#include "pch.h" // プロジェクトのプリコンパイル済みヘッダー（テストでは空のスタブ）
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include "sequencerwindow.h"
#include "sequencerwaveform.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

using TimelineCurve::KeyOrigin;

namespace
{
    constexpr float kRulerHeight = 26.0f;
    constexpr float kSplitter = 5.0f;
    constexpr float kScrollbarHeight = 12.0f;
    constexpr float kKeyRadius = 5.5f;
    constexpr float kHandleWidth = 6.0f;
    constexpr float kDragThreshold = 3.0f;
    constexpr float kMagnetPixels = 8.0f;
    constexpr float kIndent = 14.0f;
    constexpr float kRadToDeg = 57.2957795f;

    // ダークテーマ共通色
    constexpr ImU32 kBackground = IM_COL32(24, 25, 29, 255);
    constexpr ImU32 kRowEven = IM_COL32(30, 31, 36, 255);
    constexpr ImU32 kRowOdd = IM_COL32(34, 35, 41, 255);
    constexpr ImU32 kRowGroup = IM_COL32(40, 42, 50, 255);
    constexpr ImU32 kRowHover = IM_COL32(255, 255, 255, 14);
    constexpr ImU32 kRowSelected = IM_COL32(90, 140, 255, 40);
    constexpr ImU32 kGridMajor = IM_COL32(255, 255, 255, 22);
    constexpr ImU32 kGridMinor = IM_COL32(255, 255, 255, 9);
    constexpr ImU32 kText = IM_COL32(220, 222, 228, 255);
    constexpr ImU32 kTextDim = IM_COL32(150, 154, 165, 255);
    constexpr ImU32 kAccent = IM_COL32(255, 196, 64, 255);
    constexpr ImU32 kPlayhead = IM_COL32(255, 82, 70, 255);
    constexpr ImU32 kSelection = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kOutside = IM_COL32(0, 0, 0, 90);

    ImU32 WithAlpha(ImU32 color, float alpha)
    {
        const ImU32 a = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * ((color >> IM_COL32_A_SHIFT) & 0xFF));
        return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }

    ImU32 Scale(ImU32 color, float factor)
    {
        ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
        value.x = std::min(1.0f, value.x * factor);
        value.y = std::min(1.0f, value.y * factor);
        value.z = std::min(1.0f, value.z * factor);
        return ImGui::ColorConvertFloat4ToU32(value);
    }

    ImU32 ChannelColor(int channel, bool color)
    {
        static const ImU32 kAxes[4] = {IM_COL32(240, 90, 90, 255), IM_COL32(110, 220, 100, 255),
                                       IM_COL32(90, 140, 255, 255), IM_COL32(200, 200, 200, 255)};
        (void)color;
        const int index = channel < 0 ? 0 : (channel > 3 ? 3 : channel);
        return kAxes[index];
    }

    const char* ChannelName(int channel, bool color)
    {
        static const char* const kXyz[4] = {"X", "Y", "Z", "W"};
        static const char* const kRgb[4] = {"R", "G", "B", "A"};
        const int index = channel < 0 ? 0 : (channel > 3 ? 3 : channel);
        return color ? kRgb[index] : kXyz[index];
    }

    const char* PropertyLabel(TimelineProperty property)
    {
        switch (property)
        {
            case TimelineProperty::TransformPosition:
                return "Position";
            case TimelineProperty::TransformRotation:
                return "Rotation";
            case TimelineProperty::TransformScale:
                return "Scale";
            case TimelineProperty::CameraTarget:
                return "Target";
            case TimelineProperty::CameraFov:
                return "FOV";
            case TimelineProperty::CameraNearClip:
                return "Near Clip";
            case TimelineProperty::CameraFarClip:
                return "Far Clip";
            case TimelineProperty::CameraLockOnOffset:
                return "Lock-on Offset";
            case TimelineProperty::PostProcessIntensity:
                return "PostProcess";
            case TimelineProperty::LightIntensity:
                return "Intensity";
            case TimelineProperty::LightColor:
                return "Color";
            case TimelineProperty::LightDirection:
                return "Direction";
            case TimelineProperty::LightRange:
                return "Range";
            case TimelineProperty::LightBeamDensity:
                return "Beam Density";
            case TimelineProperty::BloomThreshold:
                return "Bloom Threshold";
            case TimelineProperty::BloomSoftKnee:
                return "Bloom Soft Knee";
            case TimelineProperty::BloomRadius:
                return "Bloom Radius";
            case TimelineProperty::FogDensity:
                return "Fog Density";
            case TimelineProperty::FogColor:
                return "Fog Color";
            case TimelineProperty::AnimationTime:
                return "Animation";
            case TimelineProperty::EmissiveIntensity:
                return "Emissive";
            case TimelineProperty::EmissiveColor:
                return "Emissive Color";
            case TimelineProperty::LightInnerAngle:
                return "Spot Inner";
            case TimelineProperty::LightOuterAngle:
                return "Spot Outer";
            case TimelineProperty::CameraCut:
                return "Camera Cut";
        }
        return "Property";
    }

    constexpr TimelineProperty kAllProperties[] = {
        TimelineProperty::TransformPosition,   TimelineProperty::TransformRotation,
        TimelineProperty::TransformScale,      TimelineProperty::AnimationTime,
        TimelineProperty::CameraFov,           TimelineProperty::CameraTarget,
        TimelineProperty::CameraCut,           TimelineProperty::CameraNearClip,
        TimelineProperty::CameraFarClip,       TimelineProperty::CameraLockOnOffset,
        TimelineProperty::LightIntensity,      TimelineProperty::LightColor,
        TimelineProperty::LightDirection,      TimelineProperty::LightRange,
        TimelineProperty::LightInnerAngle,     TimelineProperty::LightOuterAngle,
        TimelineProperty::LightBeamDensity,    TimelineProperty::EmissiveIntensity,
        TimelineProperty::EmissiveColor,       TimelineProperty::PostProcessIntensity,
        TimelineProperty::BloomThreshold,      TimelineProperty::BloomSoftKnee,
        TimelineProperty::BloomRadius,
    };

    int InterpolationIndex(TimelineInterpolation interpolation)
    {
        const int value = static_cast<int>(interpolation);
        return value < 0 ? 0 : (value > 5 ? 5 : value);
    }

    const char* kInterpolationNames[] = {"Constant / Step", "Linear", "Ease In", "Ease Out", "Ease In Out",
                                         "Cubic Bezier"};

    bool IsTransformProperty(TimelineProperty property)
    {
        return property == TimelineProperty::TransformPosition || property == TimelineProperty::TransformRotation ||
               property == TimelineProperty::TransformScale;
    }

    bool HasChannelRows(TimelineProperty property)
    {
        return TimelineCurve::ValueDimension(property) == 3 && !TimelineCurve::IsColorProperty(property);
    }

    float DisplayScale(TimelineProperty property)
    {
        return TimelineCurve::IsRadianProperty(property) ? kRadToDeg : 1.0f;
    }

    // ---------------------------------------------------------------- ベクターアイコン
    enum Icon : int
    {
        IconPlay,
        IconPause,
        IconStop,
        IconToStart,
        IconToEnd,
        IconPrevKey,
        IconNextKey,
        IconLoop,
        IconRange,
        IconMagnet,
        IconPreview,
        IconInspector,
        IconPlus,
        IconCurve,
        IconPosition,
        IconRotation,
        IconScale,
        IconLight,
        IconCamera,
        IconFov,
        IconAnimation,
        IconVideo,
        IconAudio,
        IconCut,
        IconCharacter,
        IconTransform,
        IconDot,
        IconKey,
    };

    void DrawDiamond(ImDrawList* drawList, ImVec2 center, float radius, ImU32 fill, ImU32 outline, float thickness)
    {
        const ImVec2 points[4] = {{center.x, center.y - radius},
                                  {center.x + radius, center.y},
                                  {center.x, center.y + radius},
                                  {center.x - radius, center.y}};
        drawList->AddConvexPolyFilled(points, 4, fill);
        if (outline)
        {
            drawList->AddPolyline(points, 4, outline, ImDrawFlags_Closed, thickness);
        }
    }

    void DrawGlyph(ImDrawList* drawList, ImVec2 c, float r, int icon, ImU32 color)
    {
        const float t = std::max(1.0f, r * 0.18f);
        switch (icon)
        {
            case IconPlay:
                drawList->AddTriangleFilled({c.x - r * 0.55f, c.y - r * 0.7f}, {c.x + r * 0.7f, c.y},
                                            {c.x - r * 0.55f, c.y + r * 0.7f}, color);
                break;
            case IconPause:
                drawList->AddRectFilled({c.x - r * 0.6f, c.y - r * 0.65f}, {c.x - r * 0.15f, c.y + r * 0.65f}, color);
                drawList->AddRectFilled({c.x + r * 0.15f, c.y - r * 0.65f}, {c.x + r * 0.6f, c.y + r * 0.65f}, color);
                break;
            case IconStop:
                drawList->AddRectFilled({c.x - r * 0.55f, c.y - r * 0.55f}, {c.x + r * 0.55f, c.y + r * 0.55f}, color,
                                        1.5f);
                break;
            case IconToStart:
                drawList->AddRectFilled({c.x - r * 0.7f, c.y - r * 0.6f}, {c.x - r * 0.45f, c.y + r * 0.6f}, color);
                drawList->AddTriangleFilled({c.x + r * 0.6f, c.y - r * 0.6f}, {c.x - r * 0.4f, c.y},
                                            {c.x + r * 0.6f, c.y + r * 0.6f}, color);
                break;
            case IconToEnd:
                drawList->AddRectFilled({c.x + r * 0.45f, c.y - r * 0.6f}, {c.x + r * 0.7f, c.y + r * 0.6f}, color);
                drawList->AddTriangleFilled({c.x - r * 0.6f, c.y - r * 0.6f}, {c.x + r * 0.4f, c.y},
                                            {c.x - r * 0.6f, c.y + r * 0.6f}, color);
                break;
            case IconPrevKey:
                drawList->AddTriangleFilled({c.x - r * 0.15f, c.y - r * 0.55f}, {c.x - r * 0.8f, c.y},
                                            {c.x - r * 0.15f, c.y + r * 0.55f}, color);
                DrawDiamond(drawList, {c.x + r * 0.35f, c.y}, r * 0.45f, color, 0, 1.0f);
                break;
            case IconNextKey:
                drawList->AddTriangleFilled({c.x + r * 0.15f, c.y - r * 0.55f}, {c.x + r * 0.8f, c.y},
                                            {c.x + r * 0.15f, c.y + r * 0.55f}, color);
                DrawDiamond(drawList, {c.x - r * 0.35f, c.y}, r * 0.45f, color, 0, 1.0f);
                break;
            case IconLoop:
                drawList->PathArcTo(c, r * 0.6f, 0.6f, 5.6f, 16);
                drawList->PathStroke(color, 0, t);
                drawList->AddTriangleFilled({c.x + r * 0.55f, c.y - r * 0.05f}, {c.x + r * 0.95f, c.y - r * 0.45f},
                                            {c.x + r * 0.2f, c.y - r * 0.5f}, color);
                break;
            case IconRange:
                drawList->AddLine({c.x - r * 0.7f, c.y - r * 0.6f}, {c.x - r * 0.7f, c.y + r * 0.6f}, color, t);
                drawList->AddLine({c.x - r * 0.7f, c.y - r * 0.6f}, {c.x - r * 0.35f, c.y - r * 0.6f}, color, t);
                drawList->AddLine({c.x - r * 0.7f, c.y + r * 0.6f}, {c.x - r * 0.35f, c.y + r * 0.6f}, color, t);
                drawList->AddLine({c.x + r * 0.7f, c.y - r * 0.6f}, {c.x + r * 0.7f, c.y + r * 0.6f}, color, t);
                drawList->AddLine({c.x + r * 0.7f, c.y - r * 0.6f}, {c.x + r * 0.35f, c.y - r * 0.6f}, color, t);
                drawList->AddLine({c.x + r * 0.7f, c.y + r * 0.6f}, {c.x + r * 0.35f, c.y + r * 0.6f}, color, t);
                drawList->AddRectFilled({c.x - r * 0.25f, c.y - r * 0.2f}, {c.x + r * 0.25f, c.y + r * 0.2f}, color);
                break;
            case IconMagnet:
                drawList->PathArcTo({c.x, c.y}, r * 0.55f, 0.0f, 3.1416f, 12);
                drawList->PathStroke(color, 0, t * 1.6f);
                drawList->AddLine({c.x - r * 0.55f, c.y}, {c.x - r * 0.55f, c.y - r * 0.6f}, color, t * 1.6f);
                drawList->AddLine({c.x + r * 0.55f, c.y}, {c.x + r * 0.55f, c.y - r * 0.6f}, color, t * 1.6f);
                break;
            case IconPreview:
            case IconCamera:
                drawList->AddRectFilled({c.x - r * 0.75f, c.y - r * 0.45f}, {c.x + r * 0.3f, c.y + r * 0.45f}, color,
                                        2.0f);
                drawList->AddTriangleFilled({c.x + r * 0.3f, c.y}, {c.x + r * 0.8f, c.y - r * 0.4f},
                                            {c.x + r * 0.8f, c.y + r * 0.4f}, color);
                break;
            case IconInspector:
                drawList->AddRect({c.x - r * 0.7f, c.y - r * 0.6f}, {c.x + r * 0.7f, c.y + r * 0.6f}, color, 1.5f, 0,
                                  t);
                drawList->AddRectFilled({c.x + r * 0.1f, c.y - r * 0.6f}, {c.x + r * 0.7f, c.y + r * 0.6f}, color);
                break;
            case IconPlus:
                drawList->AddLine({c.x - r * 0.6f, c.y}, {c.x + r * 0.6f, c.y}, color, t * 1.4f);
                drawList->AddLine({c.x, c.y - r * 0.6f}, {c.x, c.y + r * 0.6f}, color, t * 1.4f);
                break;
            case IconCurve:
                drawList->AddBezierCubic({c.x - r * 0.75f, c.y + r * 0.55f}, {c.x - r * 0.1f, c.y + r * 0.55f},
                                         {c.x + r * 0.1f, c.y - r * 0.55f}, {c.x + r * 0.75f, c.y - r * 0.55f}, color,
                                         t * 1.3f, 12);
                break;
            case IconPosition:
                drawList->AddLine({c.x - r * 0.7f, c.y}, {c.x + r * 0.7f, c.y}, color, t);
                drawList->AddLine({c.x, c.y - r * 0.7f}, {c.x, c.y + r * 0.7f}, color, t);
                drawList->AddTriangleFilled({c.x + r * 0.8f, c.y}, {c.x + r * 0.45f, c.y - r * 0.25f},
                                            {c.x + r * 0.45f, c.y + r * 0.25f}, color);
                drawList->AddTriangleFilled({c.x - r * 0.8f, c.y}, {c.x - r * 0.45f, c.y + r * 0.25f},
                                            {c.x - r * 0.45f, c.y - r * 0.25f}, color);
                drawList->AddTriangleFilled({c.x, c.y - r * 0.8f}, {c.x + r * 0.25f, c.y - r * 0.45f},
                                            {c.x - r * 0.25f, c.y - r * 0.45f}, color);
                drawList->AddTriangleFilled({c.x, c.y + r * 0.8f}, {c.x - r * 0.25f, c.y + r * 0.45f},
                                            {c.x + r * 0.25f, c.y + r * 0.45f}, color);
                break;
            case IconRotation:
                drawList->PathArcTo(c, r * 0.6f, -2.6f, 2.0f, 16);
                drawList->PathStroke(color, 0, t * 1.3f);
                drawList->AddTriangleFilled({c.x - r * 0.15f, c.y + r * 0.35f}, {c.x - r * 0.75f, c.y + r * 0.6f},
                                            {c.x - r * 0.25f, c.y + r * 0.95f}, color);
                break;
            case IconScale:
                drawList->AddRect({c.x - r * 0.7f, c.y - r * 0.1f}, {c.x + r * 0.1f, c.y + r * 0.7f}, color, 0.0f, 0, t);
                drawList->AddLine({c.x - r * 0.1f, c.y + r * 0.1f}, {c.x + r * 0.7f, c.y - r * 0.7f}, color, t);
                drawList->AddTriangleFilled({c.x + r * 0.75f, c.y - r * 0.75f}, {c.x + r * 0.2f, c.y - r * 0.65f},
                                            {c.x + r * 0.65f, c.y - r * 0.2f}, color);
                break;
            case IconLight:
                drawList->AddCircleFilled({c.x, c.y - r * 0.1f}, r * 0.42f, color, 16);
                drawList->AddRectFilled({c.x - r * 0.22f, c.y + r * 0.35f}, {c.x + r * 0.22f, c.y + r * 0.7f}, color);
                for (int ray = 0; ray < 5; ++ray)
                {
                    const float angle = -3.1416f + ray * 0.785f;
                    drawList->AddLine({c.x + std::cos(angle) * r * 0.6f, c.y - r * 0.1f + std::sin(angle) * r * 0.6f},
                                      {c.x + std::cos(angle) * r * 0.9f, c.y - r * 0.1f + std::sin(angle) * r * 0.9f},
                                      color, t);
                }
                break;
            case IconFov:
                drawList->AddLine({c.x - r * 0.7f, c.y}, {c.x + r * 0.7f, c.y - r * 0.6f}, color, t);
                drawList->AddLine({c.x - r * 0.7f, c.y}, {c.x + r * 0.7f, c.y + r * 0.6f}, color, t);
                drawList->PathArcTo({c.x - r * 0.7f, c.y}, r * 0.9f, -0.5f, 0.5f, 8);
                drawList->PathStroke(color, 0, t);
                break;
            case IconAnimation:
            case IconCharacter:
                drawList->AddCircleFilled({c.x, c.y - r * 0.5f}, r * 0.28f, color, 12);
                drawList->AddLine({c.x, c.y - r * 0.2f}, {c.x, c.y + r * 0.3f}, color, t * 1.3f);
                drawList->AddLine({c.x - r * 0.55f, c.y - r * 0.05f}, {c.x + r * 0.55f, c.y - r * 0.05f}, color, t);
                drawList->AddLine({c.x, c.y + r * 0.3f}, {c.x - r * 0.4f, c.y + r * 0.85f}, color, t);
                drawList->AddLine({c.x, c.y + r * 0.3f}, {c.x + r * 0.4f, c.y + r * 0.85f}, color, t);
                break;
            case IconVideo:
                drawList->AddRect({c.x - r * 0.8f, c.y - r * 0.55f}, {c.x + r * 0.8f, c.y + r * 0.55f}, color, 1.5f, 0,
                                  t);
                drawList->AddTriangleFilled({c.x - r * 0.2f, c.y - r * 0.3f}, {c.x + r * 0.35f, c.y},
                                            {c.x - r * 0.2f, c.y + r * 0.3f}, color);
                break;
            case IconAudio:
                for (int bar = 0; bar < 5; ++bar)
                {
                    static const float heights[5] = {0.35f, 0.7f, 0.95f, 0.55f, 0.3f};
                    const float x = c.x - r * 0.7f + bar * r * 0.35f;
                    drawList->AddLine({x, c.y - r * heights[bar] * 0.8f}, {x, c.y + r * heights[bar] * 0.8f}, color,
                                      t * 1.3f);
                }
                break;
            case IconCut:
                drawList->AddLine({c.x - r * 0.6f, c.y - r * 0.7f}, {c.x + r * 0.4f, c.y + r * 0.4f}, color, t);
                drawList->AddLine({c.x + r * 0.6f, c.y - r * 0.7f}, {c.x - r * 0.4f, c.y + r * 0.4f}, color, t);
                drawList->AddCircle({c.x - r * 0.5f, c.y + r * 0.55f}, r * 0.22f, color, 10, t);
                drawList->AddCircle({c.x + r * 0.5f, c.y + r * 0.55f}, r * 0.22f, color, 10, t);
                break;
            case IconTransform:
                drawList->AddRect({c.x - r * 0.6f, c.y - r * 0.6f}, {c.x + r * 0.6f, c.y + r * 0.6f}, color, 2.0f, 0, t);
                drawList->AddLine({c.x - r * 0.25f, c.y}, {c.x + r * 0.25f, c.y}, color, t);
                drawList->AddLine({c.x, c.y - r * 0.25f}, {c.x, c.y + r * 0.25f}, color, t);
                break;
            case IconKey:
                DrawDiamond(drawList, c, r * 0.6f, color, 0, 1.0f);
                break;
            case IconDot:
            default:
                drawList->AddCircleFilled(c, r * 0.3f, color, 10);
                break;
        }
    }

    int CategoryIcon(SequencerWindow::Category category)
    {
        using C = SequencerWindow::Category;
        switch (category)
        {
            case C::Position:
                return IconPosition;
            case C::Rotation:
                return IconRotation;
            case C::Scale:
                return IconScale;
            case C::Light:
                return IconLight;
            case C::CameraFov:
                return IconFov;
            case C::Camera:
                return IconCamera;
            case C::Animation:
                return IconAnimation;
            case C::Video:
                return IconVideo;
            case C::Audio:
                return IconAudio;
            case C::CameraCut:
                return IconCut;
            case C::Character:
                return IconCharacter;
            case C::Transform:
                return IconTransform;
            default:
                return IconDot;
        }
    }

    // ---------------------------------------------------------------- 表示設定の保存（ImGui .cfg）
    SequencerWindow::ViewSettings g_PersistedSettings;
    bool g_PersistedLoaded = false;

    void* SettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
    {
        return std::strcmp(name, "Main") == 0 ? static_cast<void*>(&g_PersistedSettings) : nullptr;
    }

    void SettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
    {
        auto& s = *static_cast<SequencerWindow::ViewSettings*>(entry);
        float f = 0.0f;
        int i = 0;
        int a = 0, b = 0, c = 0, d = 0;
        g_PersistedLoaded = true;
        if (std::sscanf(line, "PixelsPerSecond=%f", &f) == 1)
        {
            s.PixelsPerSecond = std::clamp(f, 1.0f, 5000.0f);
        }
        else if (std::sscanf(line, "ViewStart=%f", &f) == 1)
        {
            s.ViewStart = f;
        }
        else if (std::sscanf(line, "TreeWidth=%f", &f) == 1)
        {
            s.TreeWidth = std::clamp(f, 120.0f, 900.0f);
        }
        else if (std::sscanf(line, "InspectorWidth=%f", &f) == 1)
        {
            s.InspectorWidth = std::clamp(f, 180.0f, 900.0f);
        }
        else if (std::sscanf(line, "RowHeight=%f", &f) == 1)
        {
            s.RowHeight = std::clamp(f, 16.0f, 64.0f);
        }
        else if (std::sscanf(line, "PreviewHeight=%f", &f) == 1)
        {
            s.PreviewHeight = std::clamp(f, 60.0f, 600.0f);
        }
        else if (std::sscanf(line, "Mode=%d", &i) == 1)
        {
            s.ModeValue = std::clamp(i, 0, 2);
        }
        else if (std::sscanf(line, "TimeFormat=%d", &i) == 1)
        {
            s.TimeFormatValue = std::clamp(i, 0, 3);
        }
        else if (std::sscanf(line, "Snap=%d", &i) == 1)
        {
            s.SnapValue = std::clamp(i, 0, 4);
        }
        else if (std::sscanf(line, "ShowPreview=%d", &i) == 1)
        {
            s.ShowPreview = i != 0;
        }
        else if (std::sscanf(line, "ShowInspector=%d", &i) == 1)
        {
            s.ShowInspector = i != 0;
        }
        else if (std::sscanf(line, "Magnet=%d", &i) == 1)
        {
            s.Magnet = i != 0;
        }
        else if (std::sscanf(line, "InlineCurves=%d", &i) == 1)
        {
            s.InlineCurves = i != 0;
        }
        else if (std::sscanf(line, "Range=%d,%f", &i, &f) == 2)
        {
            s.RangeEnabled = i != 0;
            s.RangeIn = f;
        }
        else if (std::sscanf(line, "RangeOut=%f", &f) == 1)
        {
            s.RangeOut = f;
        }
        else if (std::sscanf(line, "CurveChannels=%d,%d,%d,%d", &a, &b, &c, &d) == 4)
        {
            s.CurveChannels[0] = a != 0;
            s.CurveChannels[1] = b != 0;
            s.CurveChannels[2] = c != 0;
            s.CurveChannels[3] = d != 0;
        }
    }

    void SettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buffer)
    {
        const auto& s = g_PersistedSettings;
        buffer->appendf("[%s][Main]\n", handler->TypeName);
        buffer->appendf("PixelsPerSecond=%.3f\nViewStart=%.4f\nTreeWidth=%.1f\nInspectorWidth=%.1f\n", s.PixelsPerSecond,
                        s.ViewStart, s.TreeWidth, s.InspectorWidth);
        buffer->appendf("RowHeight=%.1f\nPreviewHeight=%.1f\nMode=%d\nTimeFormat=%d\nSnap=%d\n", s.RowHeight,
                        s.PreviewHeight, s.ModeValue, s.TimeFormatValue, s.SnapValue);
        buffer->appendf("ShowPreview=%d\nShowInspector=%d\nMagnet=%d\nInlineCurves=%d\n", s.ShowPreview ? 1 : 0,
                        s.ShowInspector ? 1 : 0, s.Magnet ? 1 : 0, s.InlineCurves ? 1 : 0);
        buffer->appendf("Range=%d,%.4f\nRangeOut=%.4f\nCurveChannels=%d,%d,%d,%d\n\n", s.RangeEnabled ? 1 : 0,
                        s.RangeIn, s.RangeOut, s.CurveChannels[0] ? 1 : 0, s.CurveChannels[1] ? 1 : 0,
                        s.CurveChannels[2] ? 1 : 0, s.CurveChannels[3] ? 1 : 0);
    }

    bool SameSettings(const SequencerWindow::ViewSettings& a, const SequencerWindow::ViewSettings& b)
    {
        return std::memcmp(&a, &b, sizeof(a)) == 0;
    }

    ImGuiID HashRow(EntityID entity, int kind, int a, int b)
    {
        const int data[4] = {static_cast<int>(entity), kind, a, b};
        return ImHashData(data, sizeof(data));
    }
} // namespace

// ============================================================================
// 構築・設定
// ============================================================================
SequencerWindow::SequencerWindow(ISequencerBackend& backend) : m_Backend(backend)
{
    m_Settings = g_PersistedSettings;
    m_SavedSettings = m_Settings;
    m_Rows.reserve(256);
    m_VisibleRows.reserve(256);
    m_ScratchTimes.reserve(256);
    m_ScratchPoints.reserve(1024);
}

SequencerWindow::~SequencerWindow() = default;

void SequencerWindow::RegisterSettingsHandler()
{
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context || ImGui::FindSettingsHandler("RPSequencer"))
    {
        return;
    }
    ImGuiSettingsHandler handler;
    handler.TypeName = "RPSequencer";
    handler.TypeHash = ImHashStr("RPSequencer");
    handler.ReadOpenFn = SettingsReadOpen;
    handler.ReadLineFn = SettingsReadLine;
    handler.WriteAllFn = SettingsWriteAll;
    ImGui::AddSettingsHandler(&handler);
}

void SequencerWindow::SetDirector(EntityID director)
{
    if (director != m_Director)
    {
        m_Director = director;
        m_SelectedKeys.clear();
        m_SelectedClips.clear();
        m_SelectedRows.clear();
        m_ActiveRow = 0;
        m_Drag = Drag::None;
        m_LastChecksum = 0;
    }
    m_DirectorPinned = director != g_kINVALID_ENTITY;
}

float SequencerWindow::GetFrameRate() const
{
    const TimelineComponent* timeline =
        m_Director != g_kINVALID_ENTITY ? const_cast<ISequencerBackend&>(m_Backend).GetTimeline(m_Director) : nullptr;
    if (timeline && timeline->FrameRate > 0.0f)
    {
        return timeline->FrameRate;
    }
    const float project = const_cast<ISequencerBackend&>(m_Backend).GetProjectFrameRate();
    return project > 0.0f ? project : 60.0f;
}

float SequencerWindow::TimeToX(float time) const
{
    return m_TimelineMin.x + (time - m_Settings.ViewStart) * m_Settings.PixelsPerSecond;
}

float SequencerWindow::XToTime(float x) const
{
    return m_Settings.ViewStart + (x - m_TimelineMin.x) / m_Settings.PixelsPerSecond;
}

ImU32 SequencerWindow::CategoryColor(Category category, float alpha) const
{
    ImU32 color;
    switch (category)
    {
        case Category::Position:
            color = IM_COL32(72, 140, 255, 255); // 青
            break;
        case Category::Rotation:
            color = IM_COL32(170, 100, 255, 255); // 紫
            break;
        case Category::Scale:
            color = IM_COL32(80, 205, 110, 255); // 緑
            break;
        case Category::Light:
            color = IM_COL32(255, 152, 48, 255); // オレンジ
            break;
        case Category::CameraFov:
            color = IM_COL32(250, 214, 60, 255); // 黄色
            break;
        case Category::Animation:
        case Category::Character:
            color = IM_COL32(255, 110, 180, 255); // ピンク
            break;
        case Category::Video:
            color = IM_COL32(125, 100, 255, 255); // 青紫
            break;
        case Category::Audio:
            color = IM_COL32(40, 200, 150, 255); // エメラルドグリーン
            break;
        case Category::Camera:
            color = IM_COL32(230, 205, 90, 255);
            break;
        case Category::CameraCut:
            color = IM_COL32(90, 200, 230, 255);
            break;
        case Category::Transform:
            color = IM_COL32(150, 170, 210, 255);
            break;
        default:
            color = IM_COL32(150, 160, 180, 255);
            break;
    }
    return WithAlpha(color, alpha);
}

void SequencerWindow::FormatTime(float time, char* buffer, size_t size) const
{
    const TimelineComponent* timeline =
        m_Director != g_kINVALID_ENTITY ? const_cast<ISequencerBackend&>(m_Backend).GetTimeline(m_Director) : nullptr;
    const float fps = GetFrameRate();
    switch (static_cast<TimeFormat>(m_Settings.TimeFormatValue))
    {
        case TimeFormat::Frames:
            std::snprintf(buffer, size, "%d f", static_cast<int>(std::lround(time * fps)));
            break;
        case TimeFormat::Timecode:
        {
            const bool negative = time < 0.0f;
            const long long frames = std::llround(std::fabs(time) * fps);
            const long long perSecond = std::max(1ll, static_cast<long long>(std::lround(fps)));
            const long long seconds = frames / perSecond;
            std::snprintf(buffer, size, "%s%02lld:%02lld:%02lld", negative ? "-" : "", seconds / 60, seconds % 60,
                          frames % perSecond);
            break;
        }
        case TimeFormat::Beats:
        {
            const float bpm = timeline ? std::max(1.0f, timeline->BeatsPerMinute) : 120.0f;
            const int perBar = timeline ? std::max(1, timeline->BeatsPerBar) : 4;
            const double beat = (time - (timeline ? timeline->BeatOffset : 0.0f)) * bpm / 60.0;
            const double whole = std::floor(beat + 1.0e-6);
            const long long beatIndex = static_cast<long long>(whole);
            long long bar = beatIndex >= 0 ? beatIndex / perBar : -((-beatIndex + perBar - 1) / perBar);
            const long long inBar = beatIndex - bar * perBar;
            const int sub = static_cast<int>(std::floor((beat - whole) * 100.0 + 1.0e-4));
            std::snprintf(buffer, size, "%lld.%lld.%02d", bar + 1, inBar + 1, std::clamp(sub, 0, 99));
            break;
        }
        case TimeFormat::Seconds:
        default:
            std::snprintf(buffer, size, "%.3fs", time);
            break;
    }
}

void SequencerWindow::ComputeTickSteps(float& major, float& minor, float& origin) const
{
    const TimelineComponent* timeline =
        m_Director != g_kINVALID_ENTITY ? const_cast<ISequencerBackend&>(m_Backend).GetTimeline(m_Director) : nullptr;
    const float pps = m_Settings.PixelsPerSecond;
    const float minimumLabel = 72.0f;
    const float minimumTick = 7.0f;
    float steps[24];
    int count = 0;
    origin = 0.0f;
    const float fps = GetFrameRate();
    switch (static_cast<TimeFormat>(m_Settings.TimeFormatValue))
    {
        case TimeFormat::Frames:
        case TimeFormat::Timecode:
        {
            const float frames[] = {1, 2, 5, 10};
            for (float f : frames)
            {
                if (f / fps < 1.0f)
                {
                    steps[count++] = f / fps;
                }
            }
            const float seconds[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800};
            for (float s : seconds)
            {
                steps[count++] = s;
            }
            break;
        }
        case TimeFormat::Beats:
        {
            const float bpm = timeline ? std::max(1.0f, timeline->BeatsPerMinute) : 120.0f;
            const float beat = 60.0f / bpm;
            const float bar = beat * (timeline ? std::max(1, timeline->BeatsPerBar) : 4);
            origin = timeline ? timeline->BeatOffset : 0.0f;
            const float beats[] = {0.25f, 0.5f, 1.0f};
            for (float b : beats)
            {
                steps[count++] = beat * b;
            }
            const float bars[] = {1, 2, 4, 8, 16, 32, 64, 128};
            for (float b : bars)
            {
                steps[count++] = bar * b;
            }
            break;
        }
        case TimeFormat::Seconds:
        default:
        {
            const float seconds[] = {0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800};
            for (float s : seconds)
            {
                steps[count++] = s;
            }
            break;
        }
    }
    major = steps[count - 1];
    int majorIndex = count - 1;
    for (int index = 0; index < count; ++index)
    {
        if (steps[index] * pps >= minimumLabel)
        {
            major = steps[index];
            majorIndex = index;
            break;
        }
    }
    minor = major;
    for (int index = majorIndex - 1; index >= 0; --index)
    {
        if (steps[index] * pps >= minimumTick)
        {
            minor = steps[index];
        }
        else
        {
            break;
        }
    }
    if (minor == major)
    {
        minor = major * 0.5f;
    }
}

float SequencerWindow::SnapTime(float time, bool allowMagnet) const
{
    const TimelineComponent* timeline =
        m_Director != g_kINVALID_ENTITY ? const_cast<ISequencerBackend&>(m_Backend).GetTimeline(m_Director) : nullptr;
    m_SnapLine = -1.0f;
    if (ImGui::GetCurrentContext() && ImGui::GetIO().KeyAlt)
    {
        return time; // Alt：一時的にスナップ無効
    }
    float snapped = time;
    bool hasGrid = true;
    const float fps = GetFrameRate();
    switch (static_cast<SnapMode>(m_Settings.SnapValue))
    {
        case SnapMode::Frame:
            snapped = std::round(time * fps) / fps;
            break;
        case SnapMode::Beat:
        case SnapMode::Bar:
        {
            const float bpm = timeline ? std::max(1.0f, timeline->BeatsPerMinute) : 120.0f;
            float step = 60.0f / bpm;
            if (static_cast<SnapMode>(m_Settings.SnapValue) == SnapMode::Bar)
            {
                step *= timeline ? std::max(1, timeline->BeatsPerBar) : 4;
            }
            const float origin = timeline ? timeline->BeatOffset : 0.0f;
            snapped = origin + std::round((time - origin) / step) * step;
            break;
        }
        case SnapMode::Tenth:
            snapped = std::round(time * 10.0f) / 10.0f;
            break;
        case SnapMode::Off:
        default:
            hasGrid = false;
            break;
    }
    if (allowMagnet && m_Settings.Magnet && !m_SnapTargets.empty())
    {
        const float tolerance = kMagnetPixels / m_Settings.PixelsPerSecond;
        const auto it = std::lower_bound(m_SnapTargets.begin(), m_SnapTargets.end(), time);
        float best = FLT_MAX;
        float bestTime = time;
        if (it != m_SnapTargets.end())
        {
            best = std::fabs(*it - time);
            bestTime = *it;
        }
        if (it != m_SnapTargets.begin() && std::fabs(*(it - 1) - time) < best)
        {
            best = std::fabs(*(it - 1) - time);
            bestTime = *(it - 1);
        }
        if (best <= tolerance && (!hasGrid || best <= std::fabs(snapped - time)))
        {
            m_SnapLine = bestTime;
            return bestTime;
        }
    }
    if (hasGrid)
    {
        m_SnapLine = snapped;
    }
    return snapped;
}

// ============================================================================
// フレーム処理
// ============================================================================
TimelineComponent* SequencerWindow::ResolveTimeline()
{
    if (m_Director != g_kINVALID_ENTITY && m_Backend.IsAlive(m_Director))
    {
        if (TimelineComponent* timeline = m_Backend.GetTimeline(m_Director))
        {
            // 固定していなければ、エディターで選択した LiveDirector に追従する
            const EntityID selected = m_Backend.GetEditorSelection();
            if (!m_DirectorPinned && selected != m_Director && selected != g_kINVALID_ENTITY &&
                m_Backend.GetTimeline(selected))
            {
                SetDirector(selected);
                m_DirectorPinned = false;
                return m_Backend.GetTimeline(selected);
            }
            return timeline;
        }
    }
    const EntityID selected = m_Backend.GetEditorSelection();
    if (selected != g_kINVALID_ENTITY && m_Backend.GetTimeline(selected))
    {
        SetDirector(selected);
        m_DirectorPinned = false;
        return m_Backend.GetTimeline(selected);
    }
    std::vector<EntityID>& directors = m_Objects; // 一時利用（BuildRows で作り直す）
    directors.clear();
    m_Backend.CollectDirectors(directors);
    if (!directors.empty())
    {
        SetDirector(directors.front());
        m_DirectorPinned = false;
        return m_Backend.GetTimeline(directors.front());
    }
    if (m_Director != g_kINVALID_ENTITY)
    {
        SetDirector(g_kINVALID_ENTITY);
    }
    return nullptr;
}

void SequencerWindow::SyncExternalChanges(TimelineComponent& timeline)
{
    // Undo / Redo やインスペクターでの変更を検知して、シーンを現在時刻で再評価する。
    const uint64_t checksum = TimelineCurve::ComputeChecksum(timeline);
    const uint64_t revision = m_Backend.GetHistoryRevision();
    if (checksum != m_LastChecksum || revision != m_LastHistoryRevision)
    {
        const bool external = m_LastChecksum != 0 && m_Drag == Drag::None && !m_EditOpen;
        m_LastChecksum = checksum;
        m_LastHistoryRevision = revision;
        ValidateSelection(timeline);
        if (external && !m_Backend.IsSimulationRunning())
        {
            m_Backend.Evaluate(m_Director, timeline, false);
        }
    }
}

void SequencerWindow::ValidateSelection(const TimelineComponent& timeline)
{
    m_SelectedKeys.erase(std::remove_if(m_SelectedKeys.begin(), m_SelectedKeys.end(),
                                        [&](const KeyRef& ref) { return !TimelineCurve::IsValid(timeline, ref); }),
                         m_SelectedKeys.end());
    m_SelectedClips.erase(std::remove_if(m_SelectedClips.begin(), m_SelectedClips.end(),
                                         [&](const ClipRef& ref) {
                                             switch (ref.Kind)
                                             {
                                                 case ClipRef::Type::Track:
                                                     return ref.Track < 0 ||
                                                            ref.Track >= static_cast<int>(timeline.Tracks.size()) ||
                                                            ref.Clip < 0 ||
                                                            ref.Clip >= static_cast<int>(
                                                                            timeline.Tracks[ref.Track].Clips.size());
                                                 case ClipRef::Type::Video:
                                                     return ref.Track < 0 ||
                                                            ref.Track >= static_cast<int>(timeline.Monitors.size());
                                                 case ClipRef::Type::Audio:
                                                 default:
                                                     return timeline.MusicPath.empty();
                                             }
                                         }),
                          m_SelectedClips.end());
}

void SequencerWindow::SetTime(TimelineComponent& timeline, float time, bool cut)
{
    time = std::clamp(time, 0.0f, std::max(0.0f, timeline.Duration));
    if (std::fabs(time - timeline.CurrentTime) < 1.0e-7f && m_LastEvaluatedTime == time)
    {
        return;
    }
    timeline.CurrentTime = time;
    timeline.SeekRequested = true; // 曲がある場合は TimeLineSystem が曲の再生位置を合わせる
    m_LastEvaluatedTime = time;
    // Play 中は TimeLineSystem::Update が評価する。編集中・一時停止中はここで評価する。
    if (!m_Backend.IsSimulationRunning())
    {
        m_Backend.Evaluate(m_Director, timeline, cut);
    }
}

void SequencerWindow::SetPlaying(TimelineComponent& timeline, bool playing)
{
    if (playing)
    {
        const float end = timeline.MusicDuration > 0.0f
                              ? std::min(timeline.Duration, std::max(0.0f, timeline.MusicDuration - timeline.MusicOffset))
                              : timeline.Duration;
        if (m_Settings.RangeEnabled &&
            (timeline.CurrentTime < m_Settings.RangeIn || timeline.CurrentTime >= m_Settings.RangeOut))
        {
            SetTime(timeline, m_Settings.RangeIn, true);
        }
        else if (timeline.CurrentTime >= end - 1.0e-4f)
        {
            SetTime(timeline, 0.0f, true);
        }
    }
    timeline.IsPlaying = playing;
    if (!m_Backend.IsSimulationRunning())
    {
        // 編集中のプレビュー再生：曲の再生 / 停止を同期する
        m_Backend.Advance(m_Director, timeline, 0.0f);
    }
}

void SequencerWindow::TickPlayback(TimelineComponent& timeline)
{
    if (!m_Backend.IsSimulationRunning() && (timeline.IsPlaying || m_WasPlaying))
    {
        // TimeLineSystem は Play 中しか更新されないため、編集中のプレビュー再生はここで同じ処理を進める。
        m_Backend.Advance(m_Director, timeline, timeline.IsPlaying ? m_Backend.GetDeltaTime() : 0.0f);
        m_LastEvaluatedTime = timeline.CurrentTime;
    }
    if (timeline.IsPlaying && m_Settings.RangeEnabled && m_Settings.RangeOut > m_Settings.RangeIn &&
        timeline.CurrentTime >= m_Settings.RangeOut)
    {
        if (timeline.Loop)
        {
            SetTime(timeline, m_Settings.RangeIn, true);
        }
        else
        {
            SetTime(timeline, m_Settings.RangeOut, false);
            SetPlaying(timeline, false);
        }
    }
    m_WasPlaying = timeline.IsPlaying;
    // 再生中は再生ヘッドを表示範囲内に保つ
    if (timeline.IsPlaying && m_Drag == Drag::None && m_TimelineMax.x > m_TimelineMin.x)
    {
        const float visible = (m_TimelineMax.x - m_TimelineMin.x) / m_Settings.PixelsPerSecond;
        if (timeline.CurrentTime > m_Settings.ViewStart + visible * 0.95f ||
            timeline.CurrentTime < m_Settings.ViewStart)
        {
            m_Settings.ViewStart = std::max(0.0f, timeline.CurrentTime - visible * 0.1f);
        }
    }
}

void SequencerWindow::StepToKey(TimelineComponent& timeline, int direction)
{
    float best = direction > 0 ? FLT_MAX : -FLT_MAX;
    const float now = timeline.CurrentTime;
    const float epsilon = 0.5f / GetFrameRate();
    for (const auto& track : timeline.Tracks)
    {
        for (const auto& clip : track.Clips)
        {
            auto consider = [&](float time) {
                if (direction > 0 && time > now + epsilon && time < best)
                {
                    best = time;
                }
                if (direction < 0 && time < now - epsilon && time > best)
                {
                    best = time;
                }
            };
            if (clip.Keys.empty())
            {
                consider(clip.StartTime);
                consider(clip.StartTime + clip.Duration);
            }
            for (const auto& key : clip.Keys)
            {
                consider(clip.StartTime + key.Time);
            }
        }
    }
    if (best != FLT_MAX && best != -FLT_MAX)
    {
        SetTime(timeline, best, true);
    }
}

void SequencerWindow::HandleShortcuts(TimelineComponent& timeline)
{
    ImGuiIO& io = ImGui::GetIO();
    if (!m_WindowFocused || io.WantTextInput || ImGui::IsAnyItemActive() || m_Drag != Drag::None)
    {
        return;
    }
    const bool ctrl = io.KeyCtrl;
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Space, false))
    {
        SetPlaying(timeline, !timeline.IsPlaying);
    }
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Home, false))
    {
        SetTime(timeline, m_Settings.RangeEnabled ? m_Settings.RangeIn : 0.0f, true);
    }
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_End, false))
    {
        SetTime(timeline, m_Settings.RangeEnabled ? m_Settings.RangeOut : timeline.Duration, true);
    }
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
    {
        SetTime(timeline, timeline.CurrentTime - 1.0f / GetFrameRate(), false);
    }
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
    {
        SetTime(timeline, timeline.CurrentTime + 1.0f / GetFrameRate(), false);
    }
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
    {
        StepToKey(timeline, -1);
    }
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
    {
        StepToKey(timeline, 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
    {
        DeleteSelection(timeline);
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
    {
        CopySelection(timeline);
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
    {
        Paste(timeline);
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
    {
        m_SelectedKeys.clear();
        for (int track = 0; track < static_cast<int>(timeline.Tracks.size()); ++track)
        {
            for (int clip = 0; clip < static_cast<int>(timeline.Tracks[track].Clips.size()); ++clip)
            {
                for (int key = 0; key < static_cast<int>(timeline.Tracks[track].Clips[clip].Keys.size()); ++key)
                {
                    m_SelectedKeys.push_back({track, clip, key});
                }
            }
        }
    }
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_I, false))
    {
        for (int index = 0; index < static_cast<int>(m_Rows.size()); ++index)
        {
            if (m_Rows[index].Id == m_ActiveRow)
            {
                AddKeyAtRow(timeline, index, timeline.CurrentTime, true);
                break;
            }
        }
    }
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
    {
        if (static_cast<Mode>(m_Settings.ModeValue) == Mode::Curve)
        {
            m_CurveAutoFit = true;
        }
        FrameAll(timeline);
    }
}

// ============================================================================
// 行ツリー（TimelineComponent から毎フレーム構築。ヒープ確保は容量拡張時のみ）
// ============================================================================
void SequencerWindow::BuildRows(TimelineComponent& timeline)
{
    m_Rows.clear();
    m_Objects.clear();
    for (const auto& track : timeline.Tracks)
    {
        if (track.Target != g_kINVALID_ENTITY &&
            std::find(m_Objects.begin(), m_Objects.end(), track.Target) == m_Objects.end())
        {
            m_Objects.push_back(track.Target);
        }
    }
    auto push = [&](RowKind kind, Category category, int depth, int parent, EntityID entity, int track, int channel,
                    int a, const char* label) -> int {
        Row row;
        row.Kind = kind;
        row.Cat = category;
        row.Depth = static_cast<uint8_t>(depth);
        row.Parent = parent;
        row.Entity = entity;
        row.Track = track;
        row.Channel = channel;
        if (kind == RowKind::Video || kind == RowKind::VideoClip)
        {
            row.Monitor = a;
        }
        if (kind == RowKind::AnimLayer)
        {
            row.Layer = a;
        }
        row.Id = HashRow(entity, static_cast<int>(kind), track * 64 + channel + 1, a);
        std::snprintf(row.Label, sizeof(row.Label), "%s", label);
        m_Rows.push_back(row);
        if (parent >= 0)
        {
            m_Rows[parent].HasChildren = true;
        }
        return static_cast<int>(m_Rows.size()) - 1;
    };
    char label[96];
    for (EntityID entity : m_Objects)
    {
        const SequencerEntityKind kind = m_Backend.GetEntityKind(entity);
        const char* kindName = "Object";
        Category category = Category::Generic;
        switch (kind)
        {
            case SequencerEntityKind::Character:
                kindName = "Character";
                category = Category::Character;
                break;
            case SequencerEntityKind::Camera:
                kindName = "Camera";
                category = Category::Camera;
                break;
            case SequencerEntityKind::Light:
                kindName = "Light";
                category = Category::Light;
                break;
            default:
                break;
        }
        const bool alive = m_Backend.IsAlive(entity);
        std::snprintf(label, sizeof(label), "%s : %s%s", kindName, alive ? m_Backend.GetEntityName(entity) : "(削除済み)",
                      alive ? "" : "");
        const int object = push(RowKind::Object, category, 0, -1, entity, -1, -1, 0, label);

        // Transform（Position / Rotation / Scale、ライトは Direction も同じグループ）
        int transform = -1;
        for (int index = 0; index < static_cast<int>(timeline.Tracks.size()); ++index)
        {
            const auto& track = timeline.Tracks[index];
            const bool lightDirection = track.Property == TimelineProperty::LightDirection;
            if (track.Target != entity || (!IsTransformProperty(track.Property) && !lightDirection))
            {
                continue;
            }
            if (transform < 0)
            {
                transform = push(RowKind::Group, Category::Transform, 1, object, entity, -1, -1, 1, "Transform");
            }
            Category propertyCategory = Category::Generic;
            switch (track.Property)
            {
                case TimelineProperty::TransformPosition:
                    propertyCategory = Category::Position;
                    break;
                case TimelineProperty::TransformRotation:
                case TimelineProperty::LightDirection:
                    propertyCategory = Category::Rotation;
                    break;
                default:
                    propertyCategory = Category::Scale;
                    break;
            }
            std::snprintf(label, sizeof(label), "%s", PropertyLabel(track.Property));
            const int property = push(RowKind::Property, propertyCategory, 2, transform, entity, index, -1, 0, label);
            for (int channel = 0; channel < 3; ++channel)
            {
                push(RowKind::Channel, propertyCategory, 3, property, entity, index, channel, 0,
                     ChannelName(channel, false));
            }
        }
        // Animation（AnimationTime トラック、VMD レイヤー）
        int animation = -1;
        for (int index = 0; index < static_cast<int>(timeline.Tracks.size()); ++index)
        {
            const auto& track = timeline.Tracks[index];
            if (track.Target != entity || track.Property != TimelineProperty::AnimationTime)
            {
                continue;
            }
            if (animation < 0)
            {
                animation = push(RowKind::Group, Category::Animation, 1, object, entity, -1, -1, 2, "Animation");
            }
            std::snprintf(label, sizeof(label), "%s", track.Name.empty() ? "Animation Time" : track.Name.c_str());
            const int property = push(RowKind::Property, Category::Animation, 2, animation, entity, index, -1, 0, label);
            for (int layer = 0; layer < static_cast<int>(track.AnimationPaths.size()); ++layer)
            {
                const std::string& path = track.AnimationPaths[layer];
                const size_t slash = path.find_last_of("/\\");
                const char* file = path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
                const char* role = layer == 0 ? "Body" : (layer == 1 ? "Face" : "Layer");
                std::snprintf(label, sizeof(label), "%s : %s", role, file);
                push(RowKind::AnimLayer, Category::Animation, 3, property, entity, index, -1, layer, label);
            }
        }
        // その他のプロパティ（FOV / Intensity / Color / Range / Spot Angle ...）とカメラカット
        for (int index = 0; index < static_cast<int>(timeline.Tracks.size()); ++index)
        {
            const auto& track = timeline.Tracks[index];
            if (track.Target != entity || IsTransformProperty(track.Property) ||
                track.Property == TimelineProperty::LightDirection || track.Property == TimelineProperty::AnimationTime)
            {
                continue;
            }
            Category propertyCategory = Category::Generic;
            if (track.Property == TimelineProperty::CameraFov)
            {
                propertyCategory = Category::CameraFov;
            }
            else if (track.Property >= TimelineProperty::CameraTarget &&
                     track.Property <= TimelineProperty::CameraLockOnOffset)
            {
                propertyCategory = Category::Camera;
            }
            else if ((track.Property >= TimelineProperty::LightIntensity &&
                      track.Property <= TimelineProperty::LightBeamDensity) ||
                     track.Property == TimelineProperty::LightInnerAngle ||
                     track.Property == TimelineProperty::LightOuterAngle)
            {
                propertyCategory = Category::Light;
            }
            if (track.Property == TimelineProperty::CameraCut)
            {
                push(RowKind::CameraCut, Category::CameraCut, 1, object, entity, index, -1, 0, "Camera Cut");
                continue;
            }
            std::snprintf(label, sizeof(label), "%s", PropertyLabel(track.Property));
            const int property = push(RowKind::Property, propertyCategory, 1, object, entity, index, -1, 0, label);
            if (HasChannelRows(track.Property))
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    push(RowKind::Channel, propertyCategory, 2, property, entity, index, channel, 0,
                         ChannelName(channel, false));
                }
            }
        }
    }
    // 映像（LED モニター / 連番）
    for (int index = 0; index < static_cast<int>(timeline.Monitors.size()); ++index)
    {
        const auto& monitor = timeline.Monitors[index];
        std::snprintf(label, sizeof(label), "Video : %s",
                      m_Backend.IsAlive(monitor.Target) ? m_Backend.GetEntityName(monitor.Target) : "LED Screen");
        const int video = push(RowKind::Video, Category::Video, 0, -1, monitor.Target, -1, -1, index, label);
        push(RowKind::VideoClip, Category::Video, 1, video, monitor.Target, -1, -1, index, "Video Clip");
    }
    // 音声（LiveDirector の曲）
    if (!timeline.MusicPath.empty())
    {
        const size_t slash = timeline.MusicPath.find_last_of("/\\");
        std::snprintf(label, sizeof(label), "Audio : %s",
                      timeline.MusicPath.c_str() + (slash == std::string::npos ? 0 : slash + 1));
        const int audio = push(RowKind::Audio, Category::Audio, 0, -1, m_Director, -1, -1, 0, label);
        push(RowKind::AudioClip, Category::Audio, 1, audio, m_Director, -1, -1, 0, "Audio Clip");
    }
}

void SequencerWindow::LayoutRows()
{
    const bool filtering = m_Filter.IsActive();
    // 絞り込み：一致した行と、その祖先・子孫を表示する
    for (auto& row : m_Rows)
    {
        row.Match = !filtering || m_Filter.PassFilter(row.Label);
        row.Open = m_Collapsed.find(row.Id) == m_Collapsed.end();
    }
    if (filtering)
    {
        for (int index = 0; index < static_cast<int>(m_Rows.size()); ++index)
        {
            if (!m_Rows[index].Match)
            {
                continue;
            }
            for (int parent = m_Rows[index].Parent; parent >= 0; parent = m_Rows[parent].Parent)
            {
                m_Rows[parent].Match = true;
            }
        }
        for (int index = 0; index < static_cast<int>(m_Rows.size()); ++index)
        {
            const int parent = m_Rows[index].Parent;
            if (parent >= 0 && m_Rows[parent].Match && m_Filter.PassFilter(m_Rows[parent].Label))
            {
                m_Rows[index].Match = true;
            }
        }
    }
    m_VisibleRows.clear();
    float top = 0.0f;
    for (int index = 0; index < static_cast<int>(m_Rows.size()); ++index)
    {
        auto& row = m_Rows[index];
        bool visible = row.Match;
        for (int parent = row.Parent; visible && parent >= 0; parent = m_Rows[parent].Parent)
        {
            visible = filtering || m_Rows[parent].Open;
        }
        row.Visible = visible;
        if (!visible)
        {
            continue;
        }
        float height = m_Settings.RowHeight;
        switch (row.Kind)
        {
            case RowKind::Object:
            case RowKind::Video:
            case RowKind::Audio:
                height = m_Settings.RowHeight + 4.0f;
                break;
            case RowKind::Channel:
            case RowKind::AnimLayer:
                height = std::max(16.0f, m_Settings.RowHeight - 3.0f);
                break;
            case RowKind::AudioClip:
                height = m_Settings.RowHeight * 2.4f;
                break;
            case RowKind::VideoClip:
                height = m_Settings.RowHeight * 1.5f;
                break;
            default:
                break;
        }
        const auto custom = m_RowHeights.find(row.Id);
        if (custom != m_RowHeights.end())
        {
            height = custom->second;
        }
        row.Height = height;
        row.Top = top;
        top += height;
        m_VisibleRows.push_back(index);
    }
    m_ContentHeight = top;
}

int SequencerWindow::FindRow(RowKind kind, int track, int channel) const
{
    for (int index = 0; index < static_cast<int>(m_Rows.size()); ++index)
    {
        const auto& row = m_Rows[index];
        if (row.Kind == kind && row.Track == track && (channel < 0 || row.Channel == channel))
        {
            return index;
        }
    }
    return -1;
}

void SequencerWindow::SetRowOpen(ImGuiID id, bool open)
{
    if (open)
    {
        m_Collapsed.erase(id);
    }
    else
    {
        m_Collapsed.insert(id);
    }
}

void SequencerWindow::CollectRowTracks(int rowIndex, std::vector<int>& tracks) const
{
    tracks.clear();
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_Rows.size()))
    {
        return;
    }
    const Row& row = m_Rows[rowIndex];
    if (row.Track >= 0)
    {
        tracks.push_back(row.Track);
        return;
    }
    for (int index = rowIndex + 1; index < static_cast<int>(m_Rows.size()) && m_Rows[index].Depth > row.Depth;
         ++index)
    {
        const Row& child = m_Rows[index];
        if ((child.Kind == RowKind::Property || child.Kind == RowKind::CameraCut) && child.Track >= 0 &&
            std::find(tracks.begin(), tracks.end(), child.Track) == tracks.end())
        {
            tracks.push_back(child.Track);
        }
    }
}

bool SequencerWindow::RowShowsClips(const TimelineComponent& timeline, const Row& row) const
{
    if (row.Kind == RowKind::CameraCut || row.Kind == RowKind::VideoClip || row.Kind == RowKind::AudioClip ||
        row.Kind == RowKind::AnimLayer)
    {
        return true;
    }
    if (row.Kind == RowKind::Property && row.Track >= 0 &&
        timeline.Tracks[row.Track].Property == TimelineProperty::AnimationTime)
    {
        return true; // VMD は区間クリップで編集する
    }
    return static_cast<Mode>(m_Settings.ModeValue) == Mode::Clip &&
           (row.Kind == RowKind::Property || row.Kind == RowKind::Channel);
}

// ============================================================================
// メイン描画
// ============================================================================
void SequencerWindow::Draw(bool* open)
{
    if (g_PersistedLoaded)
    {
        // .cfg の読み込みは最初の NewFrame で行われるため、初回描画時に取り込む
        m_Settings = g_PersistedSettings;
        m_SavedSettings = m_Settings;
        g_PersistedLoaded = false;
    }
    ImGui::SetNextWindowSize(ImVec2(1280.0f, 420.0f), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin("シーケンサー", open,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                          ImGuiWindowFlags_NoNavInputs);
    TimelineComponent* timeline = visible ? ResolveTimeline() : nullptr;
    if (open && !*open)
    {
        // ウィンドウを閉じたら編集中のプレビュー再生（曲）を止める
        if (TimelineComponent* current = m_Director != g_kINVALID_ENTITY ? m_Backend.GetTimeline(m_Director) : nullptr)
        {
            if (current->IsPlaying && !m_Backend.IsSimulationRunning())
            {
                SetPlaying(*current, false);
            }
        }
    }
    if (!visible)
    {
        ImGui::End();
        return;
    }
    m_WindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!timeline)
    {
        ImGui::Dummy(ImVec2(0.0f, 20.0f));
        ImGui::TextColored(ImVec4(0.75f, 0.77f, 0.82f, 1.0f), "TimelineComponent を持つ Entity（LiveDirector）がありません。");
        ImGui::TextWrapped("ヒエラルキーで Entity を選択し、インスペクターの「TimelineComponentを追加」から作成してください。");
        ImGui::End();
        return;
    }
    if (m_Settings.ViewStart < 0.0f)
    {
        m_Settings.ViewStart = 0.0f;
    }
    SyncExternalChanges(*timeline);
    TickPlayback(*timeline);
    BuildRows(*timeline);
    LayoutRows();
    HandleShortcuts(*timeline);

    DrawToolbar(*timeline);

    // ---- 本体レイアウト
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 bodyMin = ImGui::GetCursorScreenPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float bottomHeight = ImGui::GetFrameHeight() + style.ItemSpacing.y + 2.0f;
    const float inspectorWidth =
        m_Settings.ShowInspector ? std::clamp(m_Settings.InspectorWidth, 180.0f, std::max(180.0f, available.x * 0.5f))
                                 : 0.0f;
    const float inspectorTotal = m_Settings.ShowInspector ? inspectorWidth + kSplitter : 0.0f;
    m_Settings.TreeWidth = std::clamp(m_Settings.TreeWidth, 120.0f, std::max(120.0f, available.x - inspectorTotal - 160.0f));
    const float headerHeight = m_Settings.ShowPreview ? std::clamp(m_Settings.PreviewHeight, 60.0f, 600.0f) : 30.0f;
    const float bodyHeight = std::max(80.0f, available.y - bottomHeight);
    const float rowsLeft = bodyMin.x;
    const float rowsRight = bodyMin.x + available.x - inspectorTotal;
    const float rowsTop = bodyMin.y + headerHeight + kRulerHeight;
    const float rowsBottom = bodyMin.y + bodyHeight - kScrollbarHeight;
    const float scrollbarWidth = style.ScrollbarSize;
    m_TreeMin = {rowsLeft, rowsTop};
    m_TreeMax = {rowsLeft + m_Settings.TreeWidth, rowsBottom};
    m_TimelineMin = {rowsLeft + m_Settings.TreeWidth + kSplitter, rowsTop};
    m_TimelineMax = {std::max(m_TimelineMin.x + 20.0f, rowsRight - scrollbarWidth), rowsBottom};
    m_RulerMin = {m_TimelineMin.x, bodyMin.y + headerHeight};
    m_RulerMax = {m_TimelineMax.x, rowsTop};

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(bodyMin, {rowsRight, bodyMin.y + bodyHeight}, kBackground);

    // 左上：カメラプレビュー + 検索
    DrawPreview(*timeline, bodyMin, {m_TreeMax.x, bodyMin.y + headerHeight});
    {
        ImGui::SetCursorScreenPos({rowsLeft + 4.0f, m_RulerMin.y + (kRulerHeight - ImGui::GetFrameHeight()) * 0.5f});
        ImGui::SetNextItemWidth(m_Settings.TreeWidth - 8.0f);
        if (ImGui::InputTextWithHint("##SeqFilter", "トラックを検索", m_Filter.InputBuf, IM_ARRAYSIZE(m_Filter.InputBuf)))
        {
            m_Filter.Build();
            LayoutRows();
        }
    }
    // 中央上：全体ナビゲーター、時間目盛り
    DrawOverview(*timeline, {m_TimelineMin.x, bodyMin.y}, {m_TimelineMax.x, bodyMin.y + headerHeight - 2.0f});
    DrawRuler(*timeline, drawList, m_RulerMin, m_RulerMax);

    // 行（ツリー + タイムライン）
    ImGui::SetCursorScreenPos({rowsLeft, rowsTop});
    DrawRows(*timeline, {rowsRight - rowsLeft, rowsBottom - rowsTop});
    DrawHorizontalScrollbar(*timeline, {m_TimelineMin.x, rowsBottom}, {m_TimelineMax.x, rowsBottom + kScrollbarHeight});

    // 左右のスプリッター
    {
        const ImVec2 splitMin = {m_TreeMax.x, bodyMin.y};
        const ImVec2 splitMax = {m_TimelineMin.x, bodyMin.y + bodyHeight};
        ImGui::SetCursorScreenPos(splitMin);
        ImGui::InvisibleButton("##TreeSplit", {std::max(1.0f, splitMax.x - splitMin.x), splitMax.y - splitMin.y});
        const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemActive())
        {
            m_Settings.TreeWidth = std::max(120.0f, m_Settings.TreeWidth + ImGui::GetIO().MouseDelta.x);
        }
        drawList->AddRectFilled(splitMin, splitMax, hovered ? IM_COL32(90, 140, 255, 160) : IM_COL32(16, 17, 20, 255));
    }
    if (m_Settings.ShowPreview)
    {
        const ImVec2 splitMin = {bodyMin.x, bodyMin.y + headerHeight - 3.0f};
        ImGui::SetCursorScreenPos(splitMin);
        ImGui::InvisibleButton("##PreviewSplit", {m_Settings.TreeWidth, 4.0f});
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        if (ImGui::IsItemActive())
        {
            m_Settings.PreviewHeight = std::clamp(m_Settings.PreviewHeight + ImGui::GetIO().MouseDelta.y, 60.0f, 600.0f);
        }
    }
    if (m_Settings.ShowInspector)
    {
        const ImVec2 splitMin = {rowsRight, bodyMin.y};
        ImGui::SetCursorScreenPos(splitMin);
        ImGui::InvisibleButton("##InspectorSplit", {kSplitter, bodyHeight});
        const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemActive())
        {
            m_Settings.InspectorWidth = std::clamp(m_Settings.InspectorWidth - ImGui::GetIO().MouseDelta.x, 180.0f, 900.0f);
        }
        drawList->AddRectFilled(splitMin, {splitMin.x + kSplitter, splitMin.y + bodyHeight},
                                hovered ? IM_COL32(90, 140, 255, 160) : IM_COL32(16, 17, 20, 255));
        ImGui::SetCursorScreenPos({rowsRight + kSplitter, bodyMin.y});
        ImGui::BeginChild("##SeqInspector", {inspectorWidth, bodyHeight}, ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawInspector(*timeline);
        ImGui::EndChild();
    }

    ImGui::SetCursorScreenPos({bodyMin.x, bodyMin.y + bodyHeight + 2.0f});
    DrawBottomBar(*timeline);
    DrawContextMenus(*timeline);

    if (m_EditOpen && m_WidgetEdit && !ImGui::IsAnyItemActive())
    {
        CommitEdit();
    }
    if (!SameSettings(m_Settings, m_SavedSettings))
    {
        m_SavedSettings = m_Settings;
        g_PersistedSettings = m_Settings;
        ImGui::MarkIniSettingsDirty();
    }
    ImGui::End();
}

bool SequencerWindow::IconButton(const char* id, int icon, const char* tooltip, bool active)
{
    const float size = ImGui::GetFrameHeight();
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, {size, size});
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImU32 background = active ? IM_COL32(70, 110, 200, 200) : IM_COL32(255, 255, 255, 0);
    if (hovered)
    {
        background = active ? IM_COL32(90, 130, 220, 230) : IM_COL32(255, 255, 255, 28);
    }
    if (ImGui::IsItemActive())
    {
        background = IM_COL32(255, 255, 255, 50);
    }
    drawList->AddRectFilled(position, {position.x + size, position.y + size}, background, 4.0f);
    DrawGlyph(drawList, {position.x + size * 0.5f, position.y + size * 0.5f}, size * 0.36f, icon,
              active || hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 204, 214, 255));
    if (tooltip && *tooltip)
    {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return pressed;
}

void SequencerWindow::DrawToolbar(TimelineComponent& timeline)
{
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.0f, 4.0f});
    // LiveDirector の選択
    {
        ImGui::SetNextItemWidth(170.0f);
        const char* name = m_Backend.GetEntityName(m_Director);
        if (ImGui::BeginCombo("##SeqDirector", name))
        {
            std::vector<EntityID> directors;
            m_Backend.CollectDirectors(directors);
            for (EntityID director : directors)
            {
                ImGui::PushID(static_cast<int>(director));
                if (ImGui::Selectable(m_Backend.GetEntityName(director), director == m_Director))
                {
                    SetDirector(director);
                }
                ImGui::PopID();
            }
            if (ImGui::Selectable("選択に追従", !m_DirectorPinned))
            {
                m_DirectorPinned = false;
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("編集する LiveDirector（TimelineComponent）");
    }
    ImGui::SameLine(0.0f, 10.0f);
    if (IconButton("##ToStart", IconToStart, "先頭へ (Home)"))
    {
        SetTime(timeline, m_Settings.RangeEnabled ? m_Settings.RangeIn : 0.0f, true);
    }
    ImGui::SameLine();
    if (IconButton("##PrevKey", IconPrevKey, "前のキーフレーム (↑)"))
    {
        StepToKey(timeline, -1);
    }
    ImGui::SameLine();
    if (IconButton("##Play", timeline.IsPlaying ? IconPause : IconPlay, timeline.IsPlaying ? "一時停止 (Space)" : "再生 (Space)",
                   timeline.IsPlaying))
    {
        SetPlaying(timeline, !timeline.IsPlaying);
    }
    ImGui::SameLine();
    if (IconButton("##Stop", IconStop, "停止（先頭に戻る）"))
    {
        SetPlaying(timeline, false);
        SetTime(timeline, m_Settings.RangeEnabled ? m_Settings.RangeIn : 0.0f, true);
    }
    ImGui::SameLine();
    if (IconButton("##NextKey", IconNextKey, "次のキーフレーム (↓)"))
    {
        StepToKey(timeline, 1);
    }
    ImGui::SameLine();
    if (IconButton("##ToEnd", IconToEnd, "末尾へ (End)"))
    {
        SetTime(timeline, m_Settings.RangeEnabled ? m_Settings.RangeOut : timeline.Duration, true);
    }
    ImGui::SameLine(0.0f, 10.0f);
    if (IconButton("##Loop", IconLoop, "ループ再生（TimelineComponent の Loop）", timeline.Loop))
    {
        BeginEdit();
        timeline.Loop = !timeline.Loop;
        MarkChanged(timeline);
        CommitEdit();
    }
    ImGui::SameLine();
    if (IconButton("##Range", IconRange, "範囲再生（目盛りの [ ] をドラッグして範囲を指定）", m_Settings.RangeEnabled))
    {
        m_Settings.RangeEnabled = !m_Settings.RangeEnabled;
        if (m_Settings.RangeEnabled && m_Settings.RangeOut <= m_Settings.RangeIn)
        {
            m_Settings.RangeIn = 0.0f;
            m_Settings.RangeOut = timeline.Duration;
        }
    }
    // 現在時刻
    ImGui::SameLine(0.0f, 12.0f);
    {
        char now[48];
        char total[48];
        FormatTime(timeline.CurrentTime, now, sizeof(now));
        FormatTime(timeline.Duration, total, sizeof(total));
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const float height = ImGui::GetFrameHeight();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float width = 190.0f;
        ImGui::InvisibleButton("##TimeDisplay", {width, height});
        const bool hovered = ImGui::IsItemHovered();
        drawList->AddRectFilled(position, {position.x + width, position.y + height},
                                hovered ? IM_COL32(44, 46, 54, 255) : IM_COL32(16, 17, 20, 255), 4.0f);
        drawList->AddText({position.x + 8.0f, position.y + (height - ImGui::GetFontSize()) * 0.5f}, kAccent, now);
        const float nowWidth = ImGui::CalcTextSize(now).x;
        char suffix[64];
        std::snprintf(suffix, sizeof(suffix), " / %s", total);
        drawList->AddText({position.x + 8.0f + nowWidth, position.y + (height - ImGui::GetFontSize()) * 0.5f}, kTextDim,
                          suffix);
        ImGui::SetItemTooltip("クリックで時刻を入力。表示形式は右の「表示」で切り替え");
        if (ImGui::IsItemClicked())
        {
            ImGui::OpenPopup("##TimeInput");
        }
        if (ImGui::BeginPopup("##TimeInput"))
        {
            float seconds = timeline.CurrentTime;
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::InputFloat("秒", &seconds, 0.0f, 0.0f, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue))
            {
                SetTime(timeline, seconds, true);
                ImGui::CloseCurrentPopup();
            }
            int frame = static_cast<int>(std::lround(timeline.CurrentTime * GetFrameRate()));
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::InputInt("フレーム", &frame, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
            {
                SetTime(timeline, frame / GetFrameRate(), true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine(0.0f, 12.0f);
    {
        ImGui::AlignTextToFramePadding();
        if (timeline.FrameRate > 0.0f)
        {
            ImGui::TextDisabled("%.0f fps", timeline.FrameRate);
        }
        else
        {
            ImGui::TextDisabled("%.0f fps (Project)", GetFrameRate());
        }
        ImGui::SetItemTooltip("フレーム表示・フレームスナップの基準。インスペクターの Timeline 設定で変更（0 = プロジェクトの目標FPS）");
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextDisabled("BPM %.1f", timeline.BeatsPerMinute);
    }
    ImGui::SameLine(0.0f, 12.0f);
    {
        const char* snaps[] = {"スナップなし", "フレーム", "拍", "小節", "0.1秒"};
        ImGui::SetNextItemWidth(96.0f);
        ImGui::Combo("##Snap", &m_Settings.SnapValue, snaps, IM_ARRAYSIZE(snaps));
        ImGui::SetItemTooltip("スナップ（Alt を押している間は無効）");
        ImGui::SameLine();
        if (IconButton("##Magnet", IconMagnet, "マグネット：キー・クリップ端・再生ヘッドに吸着", m_Settings.Magnet))
        {
            m_Settings.Magnet = !m_Settings.Magnet;
        }
        ImGui::SameLine();
        const char* formats[] = {"秒", "フレーム", "分:秒:フレーム", "小節.拍"};
        ImGui::SetNextItemWidth(118.0f);
        ImGui::Combo("##TimeFormat", &m_Settings.TimeFormatValue, formats, IM_ARRAYSIZE(formats));
        ImGui::SetItemTooltip("時間の表示形式");
    }
    ImGui::SameLine(0.0f, 12.0f);
    {
        const char* modes[] = {"Dope Sheet", "Curve", "Clip"};
        for (int mode = 0; mode < 3; ++mode)
        {
            if (mode > 0)
            {
                ImGui::SameLine(0.0f, 0.0f);
            }
            const bool selected = m_Settings.ModeValue == mode;
            ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.27f, 0.43f, 0.80f, 1.0f)
                                                            : ImVec4(0.16f, 0.17f, 0.20f, 1.0f));
            if (ImGui::Button(modes[mode]))
            {
                m_Settings.ModeValue = mode;
            }
            ImGui::PopStyleColor();
        }
    }
    ImGui::SameLine(0.0f, 12.0f);
    if (IconButton("##AddTrack", IconPlus, "トラックを追加"))
    {
        ImGui::OpenPopup("##AddTrackPopup");
    }
    if (ImGui::BeginPopup("##AddTrackPopup"))
    {
        const EntityID selected = m_Backend.GetEditorSelection();
        if (selected != g_kINVALID_ENTITY && m_Backend.IsAlive(selected))
        {
            ImGui::TextDisabled("選択中: %s", m_Backend.GetEntityName(selected));
            DrawAddTrackMenu(timeline, selected);
            ImGui::Separator();
        }
        if (ImGui::BeginMenu("Entity を指定"))
        {
            std::vector<EntityID> entities;
            m_Backend.CollectTrackableEntities(entities);
            for (EntityID entity : entities)
            {
                ImGui::PushID(static_cast<int>(entity));
                if (ImGui::BeginMenu(m_Backend.GetEntityName(entity)))
                {
                    DrawAddTrackMenu(timeline, entity);
                    ImGui::EndMenu();
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("映像（LED モニター / 連番）"))
        {
            BeginEdit();
            LiveMonitorBinding monitor{};
            monitor.Target = selected;
            monitor.StartTime = timeline.CurrentTime;
            timeline.Monitors.push_back(monitor);
            MarkChanged(timeline);
            CommitEdit();
            m_SelectedClips = {{ClipRef::Type::Video, static_cast<int>(timeline.Monitors.size()) - 1, 0}};
            m_SelectedKeys.clear();
        }
        ImGui::BeginDisabled(!timeline.MusicPath.empty());
        if (ImGui::MenuItem("音声（WAV）", nullptr, false))
        {
            m_SelectedClips = {{ClipRef::Type::Audio, 0, 0}};
            m_SelectedKeys.clear();
            m_ActiveRow = 0;
            m_Settings.ShowInspector = true;
            m_Backend.Log("Sequencer: インスペクターの「曲 (WAV)」にパスを入力するか、WAV をタイムラインへドロップしてください。");
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 12.0f);
    if (IconButton("##Preview", IconPreview, "カメラプレビューの表示切り替え", m_Settings.ShowPreview))
    {
        m_Settings.ShowPreview = !m_Settings.ShowPreview;
    }
    ImGui::SameLine();
    if (IconButton("##Inspector", IconInspector, "インスペクターの表示切り替え", m_Settings.ShowInspector))
    {
        m_Settings.ShowInspector = !m_Settings.ShowInspector;
    }
    if (static_cast<Mode>(m_Settings.ModeValue) == Mode::Curve)
    {
        ImGui::SameLine(0.0f, 12.0f);
        for (int channel = 0; channel < 4; ++channel)
        {
            ImGui::SameLine(0.0f, channel == 0 ? 0.0f : 2.0f);
            const ImU32 color = ChannelColor(channel, false);
            ImGui::PushStyleColor(ImGuiCol_Button, m_Settings.CurveChannels[channel]
                                                       ? ImGui::ColorConvertU32ToFloat4(Scale(color, 0.55f))
                                                       : ImVec4(0.16f, 0.17f, 0.20f, 1.0f));
            ImGui::PushID(channel);
            if (ImGui::Button(channel == 3 ? "W" : ChannelName(channel, false)))
            {
                m_Settings.CurveChannels[channel] = !m_Settings.CurveChannels[channel];
            }
            ImGui::PopID();
            ImGui::PopStyleColor();
        }
        ImGui::SetItemTooltip("曲線エディターで表示する成分（X/Y/Z、色は R/G/B、W はスカラー値）");
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::Button("値を自動調整"))
        {
            m_CurveAutoFit = true;
        }
    }
    ImGui::PopStyleVar();
}

void SequencerWindow::DrawAddTrackMenu(TimelineComponent& timeline, EntityID entity)
{
    DirectX::XMFLOAT4 value{};
    const bool hasTransform = m_Backend.ReadProperty(entity, TimelineProperty::TransformPosition, 0, value);
    if (hasTransform && ImGui::MenuItem("Transform（位置 / 回転 / スケール）"))
    {
        BeginEdit();
        for (TimelineProperty property : {TimelineProperty::TransformPosition, TimelineProperty::TransformRotation,
                                          TimelineProperty::TransformScale})
        {
            bool exists = false;
            for (const auto& track : timeline.Tracks)
            {
                exists |= track.Target == entity && track.Property == property;
            }
            if (!exists)
            {
                AddTrack(timeline, entity, property);
            }
        }
        MarkChanged(timeline);
        CommitEdit();
    }
    for (TimelineProperty property : kAllProperties)
    {
        if (property == TimelineProperty::LightInnerAngle || property == TimelineProperty::LightOuterAngle)
        {
            if (!m_Backend.IsSpotLight(entity))
            {
                continue;
            }
        }
        if (!m_Backend.ReadProperty(entity, property, 0, value))
        {
            continue;
        }
        char label[64];
        std::snprintf(label, sizeof(label), "%s", PropertyLabel(property));
        if (property == TimelineProperty::AnimationTime)
        {
            std::snprintf(label, sizeof(label), "Animation (VMD)");
        }
        if (ImGui::MenuItem(label))
        {
            BeginEdit();
            const int track = AddTrack(timeline, entity, property);
            if (property == TimelineProperty::AnimationTime || property == TimelineProperty::CameraCut)
            {
                AddClipAtPlayhead(timeline, track);
            }
            MarkChanged(timeline);
            CommitEdit();
        }
    }
}

void SequencerWindow::DrawPreview(TimelineComponent& timeline, ImVec2 min, ImVec2 max)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, IM_COL32(14, 15, 18, 255));
    if (!m_Settings.ShowPreview || max.y - min.y < 40.0f)
    {
        drawList->AddText({min.x + 8.0f, min.y + 7.0f}, kTextDim, m_Backend.GetEntityName(m_Director));
        return;
    }
    const float aspect = std::max(0.1f, m_Backend.GetPreviewAspect());
    float width = max.x - min.x - 8.0f;
    float height = max.y - min.y - 8.0f;
    if (width / height > aspect)
    {
        width = height * aspect;
    }
    else
    {
        height = width / aspect;
    }
    const ImVec2 position = {min.x + (max.x - min.x - width) * 0.5f, min.y + (max.y - min.y - height) * 0.5f};
    ImGui::SetCursorScreenPos(position);
    if (!m_Backend.DrawPreviewImage(width, height))
    {
        drawList->AddRectFilled(position, {position.x + width, position.y + height}, IM_COL32(28, 29, 34, 255));
        DrawGlyph(drawList, {position.x + width * 0.5f, position.y + height * 0.5f}, 14.0f, IconCamera, kTextDim);
    }
    // 現在のカメラカットと時刻を重ねて表示
    char text[128];
    const EntityID cut = TimelineCurve::FindCameraCut(timeline, timeline.CurrentTime);
    char now[48];
    FormatTime(timeline.CurrentTime, now, sizeof(now));
    if (cut != g_kINVALID_ENTITY)
    {
        std::snprintf(text, sizeof(text), "%s  |  CUT: %s", now, m_Backend.GetEntityName(cut));
    }
    else
    {
        std::snprintf(text, sizeof(text), "%s  |  %s", now, m_Backend.GetPreviewLabel());
    }
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    drawList->AddRectFilled({position.x, position.y + height - textSize.y - 6.0f}, {position.x + width, position.y + height},
                            IM_COL32(0, 0, 0, 150));
    drawList->PushClipRect(position, {position.x + width, position.y + height}, true);
    drawList->AddText({position.x + 6.0f, position.y + height - textSize.y - 3.0f}, kText, text);
    drawList->PopClipRect();
}

void SequencerWindow::DrawOverview(TimelineComponent& timeline, ImVec2 min, ImVec2 max)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, IM_COL32(18, 19, 23, 255));
    const float total = std::max(0.01f, std::max(timeline.Duration, m_Settings.RangeOut));
    const float width = max.x - min.x;
    if (width < 10.0f)
    {
        return;
    }
    auto toX = [&](float time) { return min.x + time / total * width; };
    // 各トラックの区間を細い帯で、キーを点で表示（全体の俯瞰）
    const float laneTop = min.y + 4.0f;
    const float laneBottom = max.y - 14.0f;
    const int lanes = std::max(1, static_cast<int>(timeline.Tracks.size() + timeline.Monitors.size() +
                                                   (timeline.MusicPath.empty() ? 0 : 1)));
    const float laneHeight = std::max(1.0f, std::min(6.0f, (laneBottom - laneTop) / lanes));
    int lane = 0;
    for (const auto& track : timeline.Tracks)
    {
        Category category = Category::Generic;
        switch (track.Property)
        {
            case TimelineProperty::TransformPosition:
                category = Category::Position;
                break;
            case TimelineProperty::TransformRotation:
                category = Category::Rotation;
                break;
            case TimelineProperty::TransformScale:
                category = Category::Scale;
                break;
            case TimelineProperty::AnimationTime:
                category = Category::Animation;
                break;
            case TimelineProperty::CameraFov:
                category = Category::CameraFov;
                break;
            case TimelineProperty::CameraCut:
                category = Category::CameraCut;
                break;
            default:
                category = track.Property >= TimelineProperty::LightIntensity &&
                                   track.Property <= TimelineProperty::LightBeamDensity
                               ? Category::Light
                               : Category::Generic;
                break;
        }
        const float y = laneTop + lane * laneHeight;
        for (const auto& clip : track.Clips)
        {
            drawList->AddRectFilled({toX(clip.StartTime), y}, {std::max(toX(clip.StartTime) + 1.0f, toX(clip.StartTime + clip.Duration)), y + laneHeight - 1.0f},
                                    CategoryColor(category, clip.Enabled ? 0.55f : 0.2f));
        }
        ++lane;
    }
    for (const auto& monitor : timeline.Monitors)
    {
        const float y = laneTop + lane * laneHeight;
        drawList->AddRectFilled({toX(monitor.StartTime), y},
                                {toX(TimelineCurve::MonitorEndTime(monitor, timeline.Duration)), y + laneHeight - 1.0f},
                                CategoryColor(Category::Video, 0.55f));
        ++lane;
    }
    if (!timeline.MusicPath.empty())
    {
        float start = 0.0f;
        float end = 0.0f;
        TimelineCurve::AudioRange(timeline, start, end);
        const float y = laneTop + lane * laneHeight;
        drawList->AddRectFilled({toX(std::max(0.0f, start)), y}, {toX(std::min(end, total)), y + laneHeight - 1.0f},
                                CategoryColor(Category::Audio, 0.55f));
    }
    // 表示中の範囲
    const float visible = (m_TimelineMax.x - m_TimelineMin.x) / m_Settings.PixelsPerSecond;
    const ImVec2 viewMin = {toX(m_Settings.ViewStart), min.y + 1.0f};
    const ImVec2 viewMax = {std::max(viewMin.x + 4.0f, toX(m_Settings.ViewStart + visible)), max.y - 1.0f};
    drawList->AddRectFilled(viewMin, viewMax, IM_COL32(255, 255, 255, 18), 3.0f);
    drawList->AddRect(viewMin, viewMax, IM_COL32(255, 255, 255, 90), 3.0f);
    drawList->AddLine({toX(timeline.CurrentTime), min.y}, {toX(timeline.CurrentTime), max.y}, kPlayhead, 1.5f);
    drawList->AddLine({toX(timeline.Duration), min.y}, {toX(timeline.Duration), max.y}, IM_COL32(255, 255, 255, 80));
    char label[96];
    char duration[48];
    FormatTime(timeline.Duration, duration, sizeof(duration));
    std::snprintf(label, sizeof(label), "全体 %s  ·  トラック %zu", duration, timeline.Tracks.size());
    drawList->AddText({min.x + 6.0f, max.y - 14.0f}, kTextDim, label);

    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##Overview", {width, std::max(1.0f, max.y - min.y)});
    if (ImGui::IsItemHovered())
    {
        ImGui::SetItemTooltip("ドラッグで表示範囲を移動");
    }
    if (ImGui::IsItemActive())
    {
        // 表示範囲の中心をマウス位置に合わせる
        const float time = (ImGui::GetIO().MousePos.x - min.x) / width * total;
        m_Settings.ViewStart = std::max(0.0f, time - visible * 0.5f);
    }
}

void SequencerWindow::DrawRuler(TimelineComponent& timeline, ImDrawList* drawList, ImVec2 min, ImVec2 max)
{
    drawList->AddRectFilled(min, max, IM_COL32(20, 21, 25, 255));
    drawList->AddLine({min.x, max.y - 0.5f}, {max.x, max.y - 0.5f}, IM_COL32(0, 0, 0, 255));
    drawList->PushClipRect(min, max, true);
    float major = 1.0f;
    float minor = 0.5f;
    float origin = 0.0f;
    ComputeTickSteps(major, minor, origin);
    const float viewEnd = XToTime(max.x);
    // 補助目盛り
    {
        const long long first = static_cast<long long>(std::floor((m_Settings.ViewStart - origin) / minor));
        const long long last = static_cast<long long>(std::ceil((viewEnd - origin) / minor));
        for (long long index = first; index <= last && index - first < 4000; ++index)
        {
            const float x = TimeToX(origin + index * minor);
            drawList->AddLine({x, max.y - 5.0f}, {x, max.y}, IM_COL32(255, 255, 255, 60));
        }
    }
    {
        const long long first = static_cast<long long>(std::floor((m_Settings.ViewStart - origin) / major));
        const long long last = static_cast<long long>(std::ceil((viewEnd - origin) / major));
        char label[48];
        for (long long index = first; index <= last && index - first < 2000; ++index)
        {
            const float time = origin + index * major;
            const float x = TimeToX(time);
            drawList->AddLine({x, max.y - 11.0f}, {x, max.y}, IM_COL32(255, 255, 255, 130));
            if (static_cast<TimeFormat>(m_Settings.TimeFormatValue) == TimeFormat::Seconds)
            {
                std::snprintf(label, sizeof(label), major < 0.1f ? "%.2fs" : (major < 1.0f ? "%.1fs" : "%.0fs"), time);
            }
            else
            {
                FormatTime(time, label, sizeof(label));
            }
            drawList->AddText({x + 3.0f, min.y + 2.0f}, time < 0.0f ? kTextDim : kText, label);
        }
    }
    // タイムラインの終端（以降を暗く）
    const float endX = TimeToX(timeline.Duration);
    if (endX < max.x)
    {
        drawList->AddRectFilled({std::max(min.x, endX), min.y}, max, kOutside);
    }
    // 範囲再生のマーカー
    if (m_Settings.RangeEnabled)
    {
        const float inX = TimeToX(m_Settings.RangeIn);
        const float outX = TimeToX(m_Settings.RangeOut);
        drawList->AddRectFilled({inX, max.y - 6.0f}, {outX, max.y}, IM_COL32(255, 196, 64, 70));
        drawList->AddTriangleFilled({inX, min.y + 4.0f}, {inX + 8.0f, min.y + 4.0f}, {inX, max.y}, kAccent);
        drawList->AddTriangleFilled({outX, min.y + 4.0f}, {outX - 8.0f, min.y + 4.0f}, {outX, max.y}, kAccent);
    }
    // 終端マーカー
    drawList->AddRectFilled({endX - 2.0f, min.y + 3.0f}, {endX + 2.0f, max.y}, IM_COL32(255, 255, 255, 120), 1.0f);
    // スナップのガイド
    if (m_SnapLine >= 0.0f && m_Drag != Drag::None && m_Drag != Drag::Pending)
    {
        const float x = TimeToX(m_SnapLine);
        drawList->AddLine({x, min.y}, {x, max.y}, kAccent, 1.0f);
    }
    // 再生ヘッド
    const float headX = TimeToX(timeline.CurrentTime);
    drawList->AddTriangleFilled({headX - 6.0f, min.y + 10.0f}, {headX + 6.0f, min.y + 10.0f}, {headX, max.y - 2.0f},
                                kPlayhead);
    drawList->AddRectFilled({headX - 6.0f, min.y}, {headX + 6.0f, min.y + 10.0f}, kPlayhead, 2.0f);
    drawList->PopClipRect();

    // ---- 入力：クリック / ドラッグで再生ヘッドを移動、[ ] で範囲、終端マーカーで長さを変更
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##Ruler", {std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y)},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsItemHovered();
    Drag candidate = Drag::Scrub;
    if (m_Settings.RangeEnabled && std::fabs(mouse.x - TimeToX(m_Settings.RangeIn)) < 6.0f)
    {
        candidate = Drag::RangeIn;
    }
    else if (m_Settings.RangeEnabled && std::fabs(mouse.x - TimeToX(m_Settings.RangeOut)) < 6.0f)
    {
        candidate = Drag::RangeOut;
    }
    else if (std::fabs(mouse.x - endX) < 5.0f && std::fabs(mouse.x - headX) > 6.0f)
    {
        candidate = Drag::DurationEnd;
    }
    if (hovered && candidate != Drag::Scrub)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::SetTooltip(candidate == Drag::DurationEnd ? "ドラッグでタイムラインの長さを変更"
                                                         : "ドラッグで範囲再生の開始 / 終了を変更");
    }
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        m_Drag = candidate;
        m_DragButton = ImGuiMouseButton_Left;
        m_PressMouse = mouse;
        m_SnapTargets.clear();
        BuildSnapTargets(timeline, false);
        if (candidate == Drag::Scrub)
        {
            if (timeline.IsPlaying)
            {
                SetPlaying(timeline, false);
            }
            SetTime(timeline, SnapTime(XToTime(mouse.x), true), true);
        }
        else if (candidate == Drag::DurationEnd)
        {
            BeginEdit();
        }
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        m_ContextHit = {};
        m_ContextHit.Time = XToTime(mouse.x);
        ImGui::OpenPopup("##RulerMenu");
    }
    if (ImGui::BeginPopup("##RulerMenu"))
    {
        if (ImGui::MenuItem("範囲再生の開始をここに"))
        {
            m_Settings.RangeEnabled = true;
            m_Settings.RangeIn = std::max(0.0f, m_ContextHit.Time);
            m_Settings.RangeOut = std::max(m_Settings.RangeOut, m_Settings.RangeIn + 0.1f);
        }
        if (ImGui::MenuItem("範囲再生の終了をここに"))
        {
            m_Settings.RangeEnabled = true;
            m_Settings.RangeOut = std::max(0.01f, m_ContextHit.Time);
            m_Settings.RangeIn = std::min(m_Settings.RangeIn, m_Settings.RangeOut - 0.1f);
        }
        if (ImGui::MenuItem("タイムラインの長さをここまでに"))
        {
            BeginEdit();
            timeline.Duration = std::max(0.01f, m_ContextHit.Time);
            MarkChanged(timeline);
            CommitEdit();
        }
        ImGui::EndPopup();
    }
}

// ============================================================================
// 行領域（ツリー + タイムライン）
// ============================================================================
void SequencerWindow::UpdateHover(const HitInfo& candidate)
{
    // キー > クリップ端 > クリップ > 行 の優先度。同種なら近い方。
    auto priority = [](Hit type) {
        switch (type)
        {
            case Hit::CurveHandle:
                return 9;
            case Hit::Key:
            case Hit::CurveKey:
                return 8;
            case Hit::SummaryKey:
                return 7;
            case Hit::ClipLeft:
            case Hit::ClipRight:
                return 6;
            case Hit::Playhead:
                return 5;
            case Hit::Clip:
            case Hit::GroupClip:
                return 4;
            case Hit::RowResize:
            case Hit::TreeArrow:
            case Hit::TreeKeyButton:
                return 3;
            case Hit::RowEmpty:
            case Hit::TreeRow:
            case Hit::CurveEmpty:
                return 1;
            default:
                return 0;
        }
    };
    const int current = priority(m_Hover.Type);
    const int next = priority(candidate.Type);
    if (next > current || (next == current && candidate.Distance < m_Hover.Distance))
    {
        m_Hover = candidate;
    }
}

void SequencerWindow::DrawRows(TimelineComponent& timeline, ImVec2 size)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kBackground));
    if (m_PendingScrollY >= 0.0f)
    {
        ImGui::SetNextWindowScroll({0.0f, m_PendingScrollY});
        m_PendingScrollY = -1.0f;
    }
    ImGui::BeginChild("##SeqRows", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    m_ScrollY = ImGui::GetScrollY();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    m_RowsClipMin = windowPos;
    m_RowsClipMax = {windowPos.x + windowSize.x - ImGui::GetStyle().ScrollbarSize, windowPos.y + windowSize.y};
    m_TreeMin.y = m_TimelineMin.y = windowPos.y;
    m_TreeMax.y = m_TimelineMax.y = windowPos.y + windowSize.y;
    const ImVec2 origin = ImGui::GetCursorScreenPos(); // スクロール済みの行 0 の位置

    // 入力受け（ツリー + タイムライン全体で 1 つ）
    const float contentHeight = std::max(m_ContentHeight + m_Settings.RowHeight * 2.0f, windowSize.y);
    ImGui::InvisibleButton("##RowsInput", {std::max(1.0f, m_RowsClipMax.x - windowPos.x), contentHeight},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    m_RowsHovered = ImGui::IsItemHovered();
    const bool rowsActivated = ImGui::IsItemActivated();

    // 行のドラッグ&ドロップ（アセットブラウザの WAV / VMD）
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
        {
            if (payload->Data && payload->DataSize > 0)
            {
                const std::string path(static_cast<const char*>(payload->Data),
                                       strnlen(static_cast<const char*>(payload->Data), payload->DataSize));
                std::string lower = path;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const bool wav = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".wav") == 0;
                const bool vmd = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".vmd") == 0;
                const int row = m_Hover.Row;
                if (wav)
                {
                    BeginEdit();
                    timeline.MusicPath = m_Backend.MakeAssetRelativePath(path);
                    timeline.MusicDuration = 0.0f;
                    timeline.SeekRequested = true;
                    MarkChanged(timeline);
                    CommitEdit();
                }
                else if (vmd && row >= 0 && m_Rows[row].Track >= 0 &&
                         timeline.Tracks[m_Rows[row].Track].Property == TimelineProperty::AnimationTime)
                {
                    BeginEdit();
                    if (m_Backend.AppendAnimation(timeline.Tracks[m_Rows[row].Track], path))
                    {
                        MarkChanged(timeline);
                    }
                    CommitEdit();
                }
                else
                {
                    m_Backend.Log("Sequencer: WAV は音声として、VMD は Animation トラックの行へドロップしてください。");
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // ---- 描画（表示範囲内の行のみ）
    m_Hover = {};
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool curveMode = static_cast<Mode>(m_Settings.ModeValue) == Mode::Curve;
    const float visibleTop = windowPos.y;
    const float visibleBottom = windowPos.y + windowSize.y;
    // 行の背景（ツリーとタイムラインで共通）。グリッドより先に描く。
    for (int index : m_VisibleRows)
    {
        const Row& row = m_Rows[index];
        const float y = origin.y + row.Top;
        if (y + row.Height < visibleTop || y > visibleBottom)
        {
            continue;
        }
        const bool header = row.Kind == RowKind::Object || row.Kind == RowKind::Video || row.Kind == RowKind::Audio;
        ImU32 background = header ? kRowGroup : (row.Kind == RowKind::Group ? IM_COL32(36, 38, 45, 255) : kRowEven);
        if (row.Kind == RowKind::Channel || row.Kind == RowKind::AnimLayer)
        {
            background = kRowOdd;
        }
        const ImVec2 min = {m_TreeMin.x, y};
        const ImVec2 max = {m_RowsClipMax.x, y + row.Height};
        drawList->AddRectFilled(min, max, background);
        if (m_SelectedRows.count(row.Id) != 0 || m_ActiveRow == row.Id)
        {
            drawList->AddRectFilled(min, max, kRowSelected);
        }
        if (m_RowsHovered && mouse.y >= min.y && mouse.y < max.y && m_Drag == Drag::None)
        {
            drawList->AddRectFilled(min, max, kRowHover);
        }
        drawList->AddLine({min.x, max.y - 0.5f}, {max.x, max.y - 0.5f}, IM_COL32(0, 0, 0, 120));
    }
    // 縦のグリッド（タイムライン側）
    drawList->PushClipRect({m_TimelineMin.x, visibleTop}, {m_TimelineMax.x, visibleBottom}, true);
    {
        float major = 1.0f;
        float minor = 0.5f;
        float originTime = 0.0f;
        ComputeTickSteps(major, minor, originTime);
        const float viewEnd = XToTime(m_TimelineMax.x);
        const long long first = static_cast<long long>(std::floor((m_Settings.ViewStart - originTime) / minor));
        const long long last = static_cast<long long>(std::ceil((viewEnd - originTime) / minor));
        const float ratio = major / minor;
        for (long long index = first; index <= last && index - first < 4000; ++index)
        {
            const float x = TimeToX(originTime + index * minor);
            const bool isMajor = std::fabs(std::fmod(static_cast<float>(index), ratio)) < 0.01f;
            drawList->AddLine({x, visibleTop}, {x, visibleBottom}, isMajor ? kGridMajor : kGridMinor);
        }
    }
    drawList->PopClipRect();

    for (int visibleIndex = 0; visibleIndex < static_cast<int>(m_VisibleRows.size()); ++visibleIndex)
    {
        const int index = m_VisibleRows[visibleIndex];
        Row& row = m_Rows[index];
        const float y = origin.y + row.Top;
        row.ScreenY = y;
        if (y + row.Height < visibleTop || y > visibleBottom)
        {
            continue; // 表示範囲外の行は描画しない
        }
        DrawTreeRow(timeline, drawList, index, y);
        if (!curveMode)
        {
            DrawTimelineRow(timeline, drawList, index, y);
        }
        else
        {
            // 曲線モードでも行の選択だけはできるように
            if (mouse.y >= y && mouse.y < y + row.Height && mouse.x >= m_TreeMin.x && mouse.x < m_TreeMax.x)
            {
                (void)0;
            }
        }
    }
    if (curveMode)
    {
        DrawCurveEditor(timeline, drawList, {m_TimelineMin.x, visibleTop}, {m_TimelineMax.x, visibleBottom});
    }
    DrawOverlays(timeline, drawList, {m_TimelineMin.x, visibleTop}, {m_TimelineMax.x, visibleBottom});

    // 空き領域のホバー（行の下）
    if (m_Hover.Type == Hit::None && m_RowsHovered && mouse.x >= m_TimelineMin.x)
    {
        HitInfo hit;
        hit.Type = curveMode ? Hit::CurveEmpty : Hit::RowEmpty;
        hit.Time = XToTime(mouse.x);
        UpdateHover(hit);
    }
    (void)rowsActivated;
    HandleRowsInput(timeline);
    ImGui::EndChild();
}

void SequencerWindow::DrawTreeRow(TimelineComponent& timeline, ImDrawList* drawList, int index, float y)
{
    Row& row = m_Rows[index];
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 min = {m_TreeMin.x, y};
    const ImVec2 max = {m_TreeMax.x, y + row.Height};
    const bool hovered = m_RowsHovered && mouse.y >= min.y && mouse.y < max.y;
    const bool selected = m_SelectedRows.count(row.Id) != 0 || m_ActiveRow == row.Id;
    const bool header = row.Kind == RowKind::Object || row.Kind == RowKind::Video || row.Kind == RowKind::Audio;
    drawList->PushClipRect(min, max, true);
    const ImU32 color = CategoryColor(row.Cat);
    // カテゴリ色の帯（色だけに依存しないよう、アイコンとラベルも併記）
    drawList->AddRectFilled({min.x, min.y + 1.0f}, {min.x + 3.0f, max.y - 1.0f}, color);
    float x = min.x + 6.0f + row.Depth * kIndent;
    const float centerY = (min.y + max.y) * 0.5f;
    if (row.HasChildren)
    {
        const bool open = row.Open || m_Filter.IsActive();
        const ImVec2 c = {x + 5.0f, centerY};
        if (open)
        {
            drawList->AddTriangleFilled({c.x - 4.0f, c.y - 2.0f}, {c.x + 4.0f, c.y - 2.0f}, {c.x, c.y + 3.0f}, kTextDim);
        }
        else
        {
            drawList->AddTriangleFilled({c.x - 2.0f, c.y - 4.0f}, {c.x + 3.0f, c.y}, {c.x - 2.0f, c.y + 4.0f}, kTextDim);
        }
        if (hovered && mouse.x >= x - 2.0f && mouse.x < x + 12.0f)
        {
            HitInfo hit;
            hit.Type = Hit::TreeArrow;
            hit.Row = index;
            UpdateHover(hit);
        }
    }
    x += 13.0f;
    if (row.Kind == RowKind::Channel)
    {
        const ImU32 axis = ChannelColor(row.Channel, false);
        drawList->AddCircleFilled({x + 6.0f, centerY}, 4.0f, axis, 10);
    }
    else
    {
        DrawGlyph(drawList, {x + 7.0f, centerY}, 7.0f, CategoryIcon(row.Cat), color);
    }
    x += 18.0f;
    bool enabled = true;
    if (row.Track >= 0)
    {
        enabled = timeline.Tracks[row.Track].Enabled;
    }
    if (row.Kind == RowKind::Video || row.Kind == RowKind::VideoClip)
    {
        enabled = row.Monitor >= 0 && timeline.Monitors[row.Monitor].Enabled;
    }
    const ImU32 textColor = header ? kText : (enabled ? IM_COL32(205, 208, 216, 255) : IM_COL32(120, 122, 130, 255));
    drawList->AddText({x, centerY - ImGui::GetFontSize() * 0.5f}, textColor, row.Label);

    // 右端：現在値でキー（◆）ボタン
    const bool keyable = row.Kind == RowKind::Property || row.Kind == RowKind::Channel || row.Kind == RowKind::Group ||
                         row.Kind == RowKind::Object;
    if (keyable && (hovered || selected) && !(row.Kind == RowKind::Object && row.Track < 0 && !row.HasChildren))
    {
        const ImVec2 c = {max.x - 12.0f, centerY};
        const bool buttonHovered = hovered && std::fabs(mouse.x - c.x) < 8.0f;
        DrawDiamond(drawList, c, buttonHovered ? 6.0f : 5.0f, buttonHovered ? kAccent : IM_COL32(170, 170, 180, 200), 0,
                    1.0f);
        if (buttonHovered)
        {
            HitInfo hit;
            hit.Type = Hit::TreeKeyButton;
            hit.Row = index;
            UpdateHover(hit);
            ImGui::SetTooltip("再生ヘッドの位置に現在のシーンの値でキーを追加 (I)");
        }
    }
    drawList->PopClipRect();

    if (hovered && mouse.x < m_TreeMax.x)
    {
        HitInfo hit;
        hit.Row = index;
        // 行の下端：高さ変更
        if (std::fabs(mouse.y - max.y) < 3.0f)
        {
            hit.Type = Hit::RowResize;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        else
        {
            hit.Type = Hit::TreeRow;
        }
        UpdateHover(hit);
    }
}

void SequencerWindow::DrawTimelineRow(TimelineComponent& timeline, ImDrawList* drawList, int index, float y)
{
    Row& row = m_Rows[index];
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float y0 = y;
    const float y1 = y + row.Height;
    drawList->PushClipRect({m_TimelineMin.x, std::max(y0, m_RowsClipMin.y)}, {m_TimelineMax.x, std::min(y1, m_RowsClipMax.y)},
                           true);
    const bool rowHovered = m_RowsHovered && mouse.y >= y0 && mouse.y < y1 && mouse.x >= m_TimelineMin.x &&
                            mouse.x < m_TimelineMax.x;
    if (rowHovered)
    {
        HitInfo hit;
        hit.Type = Hit::RowEmpty;
        hit.Row = index;
        hit.Time = XToTime(mouse.x);
        UpdateHover(hit);
    }
    switch (row.Kind)
    {
        case RowKind::Object:
        case RowKind::Group:
            if (static_cast<Mode>(m_Settings.ModeValue) == Mode::Clip && row.Kind == RowKind::Group)
            {
                DrawGroupClip(timeline, drawList, index, y0, y1);
            }
            else
            {
                DrawSummaryKeys(timeline, drawList, index, y0, y1);
            }
            break;
        case RowKind::Property:
        case RowKind::Channel:
        case RowKind::CameraCut:
        case RowKind::AnimLayer:
            if (RowShowsClips(timeline, row))
            {
                DrawTrackClips(timeline, drawList, row, index, y0, y1,
                               row.Kind != RowKind::AnimLayer && row.Kind != RowKind::Channel);
            }
            else
            {
                if (m_Settings.InlineCurves)
                {
                    DrawInlineCurve(timeline, drawList, row, y0, y1);
                }
                DrawKeysForTrack(timeline, drawList, row, index, y0, y1, row.Kind == RowKind::Channel);
            }
            break;
        case RowKind::VideoClip:
            DrawVideoClip(timeline, drawList, row, index, y0, y1);
            break;
        case RowKind::AudioClip:
            DrawAudioClip(timeline, drawList, row, index, y0, y1);
            break;
        case RowKind::Video:
        case RowKind::Audio:
        default:
            break;
    }
    drawList->PopClipRect();
}

void SequencerWindow::DrawKeysForTrack(TimelineComponent& timeline, ImDrawList* drawList, const Row& row, int rowIndex,
                                       float y0, float y1, bool compact)
{
    const auto& track = timeline.Tracks[row.Track];
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float centerY = (y0 + y1) * 0.5f;
    const float viewStart = XToTime(m_TimelineMin.x - kKeyRadius * 2.0f);
    const float viewEnd = XToTime(m_TimelineMax.x + kKeyRadius * 2.0f);
    const ImU32 base = row.Kind == RowKind::Channel ? ChannelColor(row.Channel, false) : CategoryColor(row.Cat);
    const float radius = compact ? kKeyRadius - 1.5f : kKeyRadius;
    // クリップ範囲を薄い帯で表示（キーは区間内で評価される）
    for (const auto& clip : track.Clips)
    {
        const float start = clip.StartTime;
        const float end = clip.StartTime + clip.Duration;
        if (end < viewStart || start > viewEnd)
        {
            continue;
        }
        drawList->AddRectFilled({TimeToX(start), centerY - 2.0f}, {TimeToX(end), centerY + 2.0f},
                                WithAlpha(base, clip.Enabled ? 0.25f : 0.08f), 2.0f);
    }
    const bool dragging = m_Drag == Drag::Keys;
    for (int clipIndex = 0; clipIndex < static_cast<int>(track.Clips.size()); ++clipIndex)
    {
        const auto& clip = track.Clips[clipIndex];
        // 表示範囲内のキーだけを処理する（キーは時刻順）
        const float localStart = viewStart - clip.StartTime;
        const float localEnd = viewEnd - clip.StartTime;
        auto it = std::lower_bound(clip.Keys.begin(), clip.Keys.end(), localStart,
                                   [](const TimelineKeyframe& key, float time) { return key.Time < time; });
        for (; it != clip.Keys.end() && it->Time <= localEnd; ++it)
        {
            const int keyIndex = static_cast<int>(it - clip.Keys.begin());
            const KeyRef ref{row.Track, clipIndex, keyIndex};
            const float time = clip.StartTime + it->Time;
            const ImVec2 c = {TimeToX(time), centerY};
            const bool selected = IsKeySelected(ref);
            const float distance = std::fabs(mouse.x - c.x) + std::fabs(mouse.y - c.y);
            const bool hovered = m_RowsHovered && distance <= radius + 2.0f && !dragging;
            ImU32 fill = selected ? Scale(base, 1.35f) : Scale(base, 0.85f);
            if (!clip.Enabled || !track.Enabled)
            {
                fill = WithAlpha(fill, 0.35f);
            }
            // 補間の種類をキーの形で示す（Step = 四角、ベジェ = 丸）
            const float r = hovered ? radius + 1.5f : radius;
            if (it->Interpolation == TimelineInterpolation::Step)
            {
                drawList->AddRectFilled({c.x - r * 0.75f, c.y - r * 0.75f}, {c.x + r * 0.75f, c.y + r * 0.75f}, fill);
                if (selected)
                {
                    drawList->AddRect({c.x - r * 0.75f - 1, c.y - r * 0.75f - 1}, {c.x + r * 0.75f + 1, c.y + r * 0.75f + 1},
                                      kSelection, 0.0f, 0, 1.5f);
                }
            }
            else if (it->Interpolation == TimelineInterpolation::CubicBezier)
            {
                drawList->AddCircleFilled(c, r * 0.85f, fill, 12);
                if (selected)
                {
                    drawList->AddCircle(c, r * 0.85f + 1.0f, kSelection, 12, 1.5f);
                }
            }
            else
            {
                DrawDiamond(drawList, c, r, fill, selected ? kSelection : IM_COL32(0, 0, 0, 160), selected ? 1.5f : 1.0f);
            }
            if (hovered)
            {
                HitInfo hit;
                hit.Type = Hit::Key;
                hit.Row = rowIndex;
                hit.Key = ref;
                hit.Time = time;
                hit.Distance = distance;
                UpdateHover(hit);
            }
        }
    }
    // ドラッグ中：元の位置をゴースト表示
    if (dragging)
    {
        for (const auto& origin : m_KeyOrigins)
        {
            if (origin.Ref.Track != row.Track)
            {
                continue;
            }
            DrawDiamond(drawList, {TimeToX(origin.AbsoluteTime), centerY}, radius, IM_COL32(255, 255, 255, 30),
                        IM_COL32(255, 255, 255, 90), 1.0f);
        }
    }
}

void SequencerWindow::DrawInlineCurve(TimelineComponent& timeline, ImDrawList* drawList, const Row& row, float y0,
                                      float y1)
{
    const auto& track = timeline.Tracks[row.Track];
    if (track.Clips.empty())
    {
        return;
    }
    const bool color = TimelineCurve::IsColorProperty(track.Property);
    const float viewStart = std::max(0.0f, XToTime(m_TimelineMin.x));
    const float viewEnd = XToTime(m_TimelineMax.x);
    if (color)
    {
        // 色トラック：実際の評価結果をグラデーションの帯で表示
        const float step = 4.0f / m_Settings.PixelsPerSecond;
        for (float time = viewStart; time < viewEnd; time += step)
        {
            DirectX::XMFLOAT4 value{};
            if (TimelineCurve::EvaluateTrack(track, time, value) == TimelineCurve::TrackSample::None)
            {
                continue;
            }
            const ImU32 c = ImGui::ColorConvertFloat4ToU32(
                {std::clamp(value.x, 0.0f, 1.0f), std::clamp(value.y, 0.0f, 1.0f), std::clamp(value.z, 0.0f, 1.0f), 0.85f});
            drawList->AddRectFilled({TimeToX(time), y0 + 3.0f}, {TimeToX(time + step) + 0.5f, y0 + 7.0f}, c);
        }
        return;
    }
    const int dimension = TimelineCurve::ValueDimension(track.Property);
    if (row.Kind == RowKind::Property && dimension != 1)
    {
        return; // ベクトルは X / Y / Z の子行に表示
    }
    const int channel = row.Kind == RowKind::Channel ? row.Channel : 0;
    // キー値の範囲で正規化
    float minimum = FLT_MAX;
    float maximum = -FLT_MAX;
    for (const auto& clip : track.Clips)
    {
        for (const auto& key : clip.Keys)
        {
            const float v = TimelineCurve::GetComponent(key.Value, channel);
            minimum = std::min(minimum, v);
            maximum = std::max(maximum, v);
        }
    }
    if (minimum == FLT_MAX)
    {
        return;
    }
    if (maximum - minimum < 1.0e-5f)
    {
        minimum -= 1.0f;
        maximum += 1.0f;
    }
    const float top = y0 + 3.0f;
    const float bottom = y1 - 3.0f;
    m_ScratchPoints.clear();
    const float stepPixels = 3.0f;
    for (float x = std::max(m_TimelineMin.x, TimeToX(0.0f)); x <= m_TimelineMax.x; x += stepPixels)
    {
        DirectX::XMFLOAT4 value{};
        if (TimelineCurve::EvaluateTrack(track, XToTime(x), value) == TimelineCurve::TrackSample::None)
        {
            continue;
        }
        const float v = (TimelineCurve::GetComponent(value, channel) - minimum) / (maximum - minimum);
        m_ScratchPoints.push_back({x, bottom - v * (bottom - top)});
    }
    if (m_ScratchPoints.size() >= 2)
    {
        const ImU32 base = row.Kind == RowKind::Channel ? ChannelColor(channel, false) : CategoryColor(row.Cat);
        drawList->AddPolyline(m_ScratchPoints.data(), static_cast<int>(m_ScratchPoints.size()), WithAlpha(base, 0.45f),
                              ImDrawFlags_None, 1.2f);
    }
}

void SequencerWindow::CollectKeysAt(const TimelineComponent& timeline, int rowIndex, float time,
                                    std::vector<KeyRef>& out) const
{
    out.clear();
    std::vector<int> tracks;
    CollectRowTracks(rowIndex, tracks);
    const float tolerance = std::max(1.0e-3f, 0.5f / m_Settings.PixelsPerSecond);
    for (int track : tracks)
    {
        const auto& data = timeline.Tracks[track];
        for (int clip = 0; clip < static_cast<int>(data.Clips.size()); ++clip)
        {
            for (int key = 0; key < static_cast<int>(data.Clips[clip].Keys.size()); ++key)
            {
                if (std::fabs(data.Clips[clip].StartTime + data.Clips[clip].Keys[key].Time - time) <= tolerance)
                {
                    out.push_back({track, clip, key});
                }
            }
        }
    }
}

void SequencerWindow::DrawSummaryKeys(TimelineComponent& timeline, ImDrawList* drawList, int rowIndex, float y0,
                                      float y1)
{
    CollectRowTracks(rowIndex, m_ScratchTracks);
    if (m_ScratchTracks.empty())
    {
        return;
    }
    const Row& row = m_Rows[rowIndex];
    const float viewStart = XToTime(m_TimelineMin.x - kKeyRadius * 2.0f);
    const float viewEnd = XToTime(m_TimelineMax.x + kKeyRadius * 2.0f);
    // まとめキーは 1 ピクセル単位のビンに集約する（大量トラックでもキー数 × log ではなく線形）。
    // 値：bit0 = キーあり、bit1 = 未選択のキーあり
    const int width = std::max(1, static_cast<int>(m_TimelineMax.x - m_TimelineMin.x) + static_cast<int>(kKeyRadius * 4.0f));
    m_SummaryBins.assign(static_cast<size_t>(width), 0);
    m_SummaryTimes.resize(static_cast<size_t>(width));
    const float left = m_TimelineMin.x - kKeyRadius * 2.0f;
    for (int track : m_ScratchTracks)
    {
        const auto& data = timeline.Tracks[track];
        if (data.Property == TimelineProperty::CameraCut || data.Property == TimelineProperty::AnimationTime)
        {
            continue; // 区間で表示するトラックはまとめに含めない
        }
        for (int clipIndex = 0; clipIndex < static_cast<int>(data.Clips.size()); ++clipIndex)
        {
            const auto& clip = data.Clips[clipIndex];
            if (clip.StartTime + clip.Duration < viewStart - 1.0f || clip.StartTime > viewEnd + 1.0f)
            {
                continue;
            }
            auto it = std::lower_bound(clip.Keys.begin(), clip.Keys.end(), viewStart - clip.StartTime,
                                       [](const TimelineKeyframe& key, float time) { return key.Time < time; });
            for (; it != clip.Keys.end() && clip.StartTime + it->Time <= viewEnd; ++it)
            {
                const float time = clip.StartTime + it->Time;
                const int bin = static_cast<int>(TimeToX(time) - left);
                if (bin < 0 || bin >= width)
                {
                    continue;
                }
                uint8_t& flags = m_SummaryBins[bin];
                if (!(flags & 1))
                {
                    m_SummaryTimes[bin] = time;
                }
                flags |= 1;
                const KeyRef ref{track, clipIndex, static_cast<int>(it - clip.Keys.begin())};
                if (m_SelectedKeys.empty() || !IsKeySelected(ref))
                {
                    flags |= 2;
                }
            }
        }
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float centerY = (y0 + y1) * 0.5f;
    const ImU32 color = row.Kind == RowKind::Object ? IM_COL32(200, 205, 220, 255) : CategoryColor(row.Cat);
    int lastDrawn = -1000;
    for (int bin = 0; bin < width; ++bin)
    {
        const uint8_t flags = m_SummaryBins[bin];
        if (!(flags & 1) || bin - lastDrawn < 1)
        {
            continue;
        }
        lastDrawn = bin;
        const float time = m_SummaryTimes[bin];
        const bool allSelected = !(flags & 2);
        const ImVec2 c = {TimeToX(time), centerY};
        const float distance = std::fabs(mouse.x - c.x) + std::fabs(mouse.y - c.y);
        const bool hovered = m_RowsHovered && distance <= kKeyRadius + 2.0f && m_Drag != Drag::Keys;
        const float r = (hovered ? kKeyRadius + 1.0f : kKeyRadius) - 0.5f;
        DrawDiamond(drawList, c, r, allSelected ? Scale(color, 1.2f) : WithAlpha(Scale(color, 0.7f), 0.9f),
                    allSelected ? kSelection : IM_COL32(0, 0, 0, 160), allSelected ? 1.5f : 1.0f);
        if (hovered)
        {
            HitInfo hit;
            hit.Type = Hit::SummaryKey;
            hit.Row = rowIndex;
            hit.Time = time;
            hit.Distance = distance;
            UpdateHover(hit);
        }
    }
}

void SequencerWindow::DrawClipBar(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color, const char* label,
                                  float start, float end, bool selected, bool hovered, int hoveredEdge, bool enabled,
                                  bool resizable)
{
    const float width = max.x - min.x;
    const ImU32 fill = enabled ? Scale(color, 0.55f) : IM_COL32(60, 62, 70, 255);
    const ImU32 top = enabled ? Scale(color, 0.8f) : IM_COL32(80, 82, 90, 255);
    drawList->AddRectFilled(min, max, WithAlpha(fill, hovered ? 1.0f : 0.92f), 4.0f);
    drawList->AddRectFilled(min, {max.x, min.y + 3.0f}, top, 4.0f, ImDrawFlags_RoundCornersTop);
    drawList->AddRect(min, max, selected ? kSelection : WithAlpha(Scale(color, 1.2f), hovered ? 0.9f : 0.5f), 4.0f, 0,
                      selected ? 2.0f : 1.0f);
    // リサイズハンドル
    if (resizable && width > 14.0f)
    {
        const ImU32 handle = IM_COL32(255, 255, 255, 60);
        const ImU32 active = IM_COL32(255, 255, 255, 200);
        drawList->AddRectFilled({min.x + 1.0f, min.y + 4.0f}, {min.x + kHandleWidth * 0.6f, max.y - 3.0f},
                                hoveredEdge == 0 ? active : handle, 2.0f);
        drawList->AddRectFilled({max.x - kHandleWidth * 0.6f, min.y + 4.0f}, {max.x - 1.0f, max.y - 3.0f},
                                hoveredEdge == 1 ? active : handle, 2.0f);
    }
    // 名前と開始・終了・長さ（幅に応じて段階的に省略）
    if (width > 24.0f)
    {
        drawList->PushClipRect({min.x + 6.0f, min.y}, {max.x - 6.0f, max.y}, true);
        const float fontSize = ImGui::GetFontSize();
        drawList->AddText({min.x + 8.0f, min.y + 3.0f}, enabled ? kText : kTextDim, label);
        if (max.y - min.y > fontSize * 2.0f + 4.0f || width > ImGui::CalcTextSize(label).x + 150.0f)
        {
            char a[32];
            char b[32];
            char d[32];
            FormatTime(start, a, sizeof(a));
            FormatTime(end, b, sizeof(b));
            FormatTime(end - start, d, sizeof(d));
            char text[112];
            std::snprintf(text, sizeof(text), "%s – %s  (%s)", a, b, d);
            const bool twoLines = max.y - min.y > fontSize * 2.0f + 4.0f;
            const ImVec2 position = twoLines ? ImVec2(min.x + 8.0f, min.y + fontSize + 4.0f)
                                             : ImVec2(min.x + 16.0f + ImGui::CalcTextSize(label).x, min.y + 3.0f);
            drawList->AddText(position, WithAlpha(kText, 0.7f), text);
        }
        drawList->PopClipRect();
    }
}

void SequencerWindow::DrawTrackClips(TimelineComponent& timeline, ImDrawList* drawList, const Row& row, int rowIndex,
                                     float y0, float y1, bool interactive)
{
    const auto& track = timeline.Tracks[row.Track];
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float viewStart = XToTime(m_TimelineMin.x);
    const float viewEnd = XToTime(m_TimelineMax.x);
    Category category = row.Cat;
    for (int clipIndex = 0; clipIndex < static_cast<int>(track.Clips.size()); ++clipIndex)
    {
        const auto& clip = track.Clips[clipIndex];
        const float start = clip.StartTime;
        const float end = clip.StartTime + clip.Duration;
        if (end < viewStart || start > viewEnd)
        {
            continue; // 表示範囲外のクリップは描画しない
        }
        const ClipRef ref{ClipRef::Type::Track, row.Track, clipIndex};
        const ImVec2 min = {TimeToX(start), y0 + 2.0f};
        const ImVec2 max = {std::max(TimeToX(end), min.x + 3.0f), y1 - 2.0f};
        const bool inside = m_RowsHovered && mouse.x >= min.x - 3.0f && mouse.x <= max.x + 3.0f && mouse.y >= min.y &&
                            mouse.y <= max.y;
        int edge = -1;
        if (inside && interactive)
        {
            HitInfo hit;
            hit.Row = rowIndex;
            hit.Clip = ref;
            hit.Time = XToTime(mouse.x);
            if (std::fabs(mouse.x - min.x) <= kHandleWidth)
            {
                hit.Type = Hit::ClipLeft;
                edge = 0;
            }
            else if (std::fabs(mouse.x - max.x) <= kHandleWidth)
            {
                hit.Type = Hit::ClipRight;
                edge = 1;
            }
            else
            {
                hit.Type = Hit::Clip;
            }
            UpdateHover(hit);
            if (edge >= 0)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
        }
        const char* name = clip.Name.c_str();
        char label[96];
        if (track.Property == TimelineProperty::CameraCut)
        {
            std::snprintf(label, sizeof(label), "%s", m_Backend.GetEntityName(track.Target));
            name = label;
        }
        else if (row.Kind == RowKind::AnimLayer && row.Layer >= 0 &&
                 row.Layer < static_cast<int>(track.AnimationPaths.size()))
        {
            const std::string& path = track.AnimationPaths[row.Layer];
            const size_t slash = path.find_last_of("/\\");
            std::snprintf(label, sizeof(label), "%s", path.c_str() + (slash == std::string::npos ? 0 : slash + 1));
            name = label;
        }
        const ImU32 color = row.Kind == RowKind::Channel ? ChannelColor(row.Channel, false) : CategoryColor(category);
        DrawClipBar(drawList, min, max, color, name, start, end, IsClipSelected(ref), inside && interactive, edge,
                    clip.Enabled && track.Enabled, interactive);
        // ブレンドイン / アウトを斜線で
        if (clip.BlendIn > 0.0f)
        {
            drawList->AddLine({min.x, max.y}, {TimeToX(start + clip.BlendIn), min.y + 3.0f}, IM_COL32(255, 255, 255, 90));
        }
        if (clip.BlendOut > 0.0f)
        {
            drawList->AddLine({TimeToX(end - clip.BlendOut), min.y + 3.0f}, {max.x, max.y}, IM_COL32(255, 255, 255, 90));
        }
        // キー（区間内の目盛り）
        for (const auto& key : clip.Keys)
        {
            const float x = TimeToX(start + key.Time);
            if (x >= min.x && x <= max.x)
            {
                drawList->AddLine({x, max.y - 6.0f}, {x, max.y - 1.0f}, IM_COL32(255, 255, 255, 140));
            }
        }
        // 区間の開始・終了の縦線（選択・ホバー時）
        if (IsClipSelected(ref) || (inside && interactive))
        {
            drawList->AddLine({min.x, m_RowsClipMin.y}, {min.x, m_RowsClipMax.y}, WithAlpha(color, 0.35f), 1.0f);
            drawList->AddLine({max.x, m_RowsClipMin.y}, {max.x, m_RowsClipMax.y}, WithAlpha(color, 0.35f), 1.0f);
        }
    }
    // ドラッグ中のゴースト
    if (m_Drag == Drag::Clips || m_Drag == Drag::ClipLeft || m_Drag == Drag::ClipRight)
    {
        for (const auto& origin : m_ClipOrigins)
        {
            if (origin.Ref.Kind == ClipRef::Type::Track && origin.Ref.Track == row.Track)
            {
                drawList->AddRect({TimeToX(origin.Start), y0 + 2.0f}, {TimeToX(origin.Start + origin.Duration), y1 - 2.0f},
                                  IM_COL32(255, 255, 255, 70), 4.0f, 0, 1.0f);
            }
        }
    }
}

void SequencerWindow::DrawGroupClip(TimelineComponent& timeline, ImDrawList* drawList, int rowIndex, float y0, float y1)
{
    // Clip View：Transform などのグループ行に、子トラックのクリップ全体を 1 本の区間としてまとめて表示
    CollectRowTracks(rowIndex, m_ScratchTracks);
    float start = FLT_MAX;
    float end = -FLT_MAX;
    for (int track : m_ScratchTracks)
    {
        for (const auto& clip : timeline.Tracks[track].Clips)
        {
            start = std::min(start, clip.StartTime);
            end = std::max(end, clip.StartTime + clip.Duration);
        }
    }
    if (start > end)
    {
        return;
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 min = {TimeToX(start), y0 + 3.0f};
    const ImVec2 max = {std::max(TimeToX(end), min.x + 3.0f), y1 - 3.0f};
    const bool inside = m_RowsHovered && mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
    if (inside)
    {
        HitInfo hit;
        hit.Type = Hit::GroupClip;
        hit.Row = rowIndex;
        hit.Time = XToTime(mouse.x);
        UpdateHover(hit);
    }
    DrawClipBar(drawList, min, max, CategoryColor(m_Rows[rowIndex].Cat), m_Rows[rowIndex].Label, start, end, false, inside,
                -1, true, false);
}

void SequencerWindow::DrawVideoClip(TimelineComponent& timeline, ImDrawList* drawList, const Row& row, int rowIndex,
                                    float y0, float y1)
{
    if (row.Monitor < 0 || row.Monitor >= static_cast<int>(timeline.Monitors.size()))
    {
        return;
    }
    const auto& monitor = timeline.Monitors[row.Monitor];
    const float start = monitor.StartTime;
    const float end = TimelineCurve::MonitorEndTime(monitor, timeline.Duration);
    const ClipRef ref{ClipRef::Type::Video, row.Monitor, 0};
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 min = {TimeToX(start), y0 + 2.0f};
    const ImVec2 max = {std::max(TimeToX(end), min.x + 3.0f), y1 - 2.0f};
    const bool inside =
        m_RowsHovered && mouse.x >= min.x - 3.0f && mouse.x <= max.x + 3.0f && mouse.y >= min.y && mouse.y <= max.y;
    int edge = -1;
    if (inside)
    {
        HitInfo hit;
        hit.Row = rowIndex;
        hit.Clip = ref;
        hit.Time = XToTime(mouse.x);
        hit.Type = Hit::Clip;
        if (std::fabs(mouse.x - min.x) <= kHandleWidth)
        {
            hit.Type = Hit::ClipLeft;
            edge = 0;
        }
        else if (std::fabs(mouse.x - max.x) <= kHandleWidth)
        {
            hit.Type = Hit::ClipRight;
            edge = 1;
        }
        UpdateHover(hit);
        if (edge >= 0)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
    }
    char label[128];
    std::snprintf(label, sizeof(label), "%s  [%zu フレーム / %.0f fps%s]", m_Backend.GetEntityName(monitor.Target),
                  monitor.Frames.size(), monitor.FramesPerSecond, monitor.Loop ? " / ループ" : "");
    DrawClipBar(drawList, min, max, CategoryColor(Category::Video), label, start, end, IsClipSelected(ref), inside, edge,
                monitor.Enabled, true);
    // 連番の 1 周ごとの区切り
    const float length = TimelineCurve::MonitorSequenceLength(monitor);
    if (length > 0.0f && length * m_Settings.PixelsPerSecond > 6.0f)
    {
        for (float t = start + length - monitor.Offset; t < end; t += length)
        {
            if (t > start)
            {
                drawList->AddLine({TimeToX(t), max.y - 7.0f}, {TimeToX(t), max.y - 1.0f}, IM_COL32(255, 255, 255, 120));
            }
        }
    }
}

void SequencerWindow::DrawAudioClip(TimelineComponent& timeline, ImDrawList* drawList, const Row& row, int rowIndex,
                                    float y0, float y1)
{
    (void)row;
    float start = 0.0f;
    float end = 0.0f;
    TimelineCurve::AudioRange(timeline, start, end);
    const float visibleStart = std::max(0.0f, start);
    const ClipRef ref{ClipRef::Type::Audio, 0, 0};
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 min = {TimeToX(visibleStart), y0 + 2.0f};
    const ImVec2 max = {std::max(TimeToX(end), min.x + 3.0f), y1 - 2.0f};
    const bool inside = m_RowsHovered && mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
    if (inside)
    {
        HitInfo hit;
        hit.Type = Hit::Clip;
        hit.Row = rowIndex;
        hit.Clip = ref;
        hit.Time = XToTime(mouse.x);
        UpdateHover(hit);
    }
    const size_t slash = timeline.MusicPath.find_last_of("/\\");
    char label[160];
    std::snprintf(label, sizeof(label), "%s%s", timeline.MusicPath.c_str() + (slash == std::string::npos ? 0 : slash + 1),
                  timeline.PlaybackError.empty() ? "" : "  (再生エラー)");
    DrawClipBar(drawList, min, max, CategoryColor(Category::Audio), label, visibleStart, end, IsClipSelected(ref), inside,
                -1, timeline.PlaybackError.empty(), false);
    // 波形（ワーカースレッドで解析したピークを表示。表示範囲の列だけ集計）
    std::shared_ptr<const SequencerWaveform::Peaks> peaks;
    const auto state = SequencerWaveform::Request(timeline.MusicPath, peaks);
    const float waveTop = min.y + ImGui::GetFontSize() + 4.0f;
    const float waveBottom = max.y - 3.0f;
    if (state == SequencerWaveform::State::Ready && peaks && waveBottom - waveTop > 6.0f)
    {
        const float center = (waveTop + waveBottom) * 0.5f;
        const float half = (waveBottom - waveTop) * 0.5f;
        const float x0 = std::max(min.x, m_TimelineMin.x);
        const float x1 = std::min(max.x, m_TimelineMax.x);
        const float gain = std::clamp(timeline.MusicVolume, 0.0f, 2.0f);
        const ImU32 waveColor = IM_COL32(170, 255, 220, 200);
        for (float x = x0; x < x1; x += 1.0f)
        {
            // タイムライン時刻 → ファイル時刻
            const float fileTime0 = XToTime(x) + timeline.MusicOffset;
            const float fileTime1 = XToTime(x + 1.0f) + timeline.MusicOffset;
            float low = 0.0f;
            float high = 0.0f;
            if (SequencerWaveform::Range(*peaks, fileTime0, fileTime1, low, high))
            {
                drawList->AddLine({x + 0.5f, center - std::min(1.0f, high * gain) * half},
                                  {x + 0.5f, center - std::max(-1.0f, low * gain) * half + 1.0f}, waveColor);
            }
        }
    }
    else if (state == SequencerWaveform::State::Loading)
    {
        drawList->AddText({min.x + 8.0f, waveTop}, kTextDim, "波形を解析中…");
    }
    else if (state == SequencerWaveform::State::Failed)
    {
        drawList->AddText({min.x + 8.0f, waveTop}, kTextDim, "波形を表示できません（PCM16 / float32 WAV のみ）");
    }
    // BPM の拍線（小節は太く）
    const float beat = 60.0f / std::max(1.0f, timeline.BeatsPerMinute);
    if (beat * m_Settings.PixelsPerSecond > 5.0f)
    {
        const int perBar = std::max(1, timeline.BeatsPerBar);
        const float viewStart = XToTime(std::max(min.x, m_TimelineMin.x));
        const float viewEnd = XToTime(std::min(max.x, m_TimelineMax.x));
        long long index = static_cast<long long>(std::ceil((viewStart - timeline.BeatOffset) / beat));
        for (float t = timeline.BeatOffset + index * beat; t <= viewEnd; t += beat, ++index)
        {
            const bool bar = index % perBar == 0;
            drawList->AddLine({TimeToX(t), bar ? min.y + 3.0f : waveBottom - 5.0f}, {TimeToX(t), waveBottom},
                              IM_COL32(255, 255, 255, bar ? 70 : 40));
        }
    }
    if (m_Drag == Drag::Clips && !m_ClipOrigins.empty() && m_ClipOrigins.front().Ref.Kind == ClipRef::Type::Audio)
    {
        const float originStart = -m_ClipOrigins.front().Offset;
        drawList->AddRect({TimeToX(std::max(0.0f, originStart)), y0 + 2.0f},
                          {TimeToX(originStart + (end - start)), y1 - 2.0f}, IM_COL32(255, 255, 255, 70), 4.0f);
    }
}

void SequencerWindow::DrawOverlays(TimelineComponent& timeline, ImDrawList* drawList, ImVec2 min, ImVec2 max)
{
    drawList->PushClipRect(min, max, true);
    // タイムライン終了後と範囲再生外を暗く
    const float endX = TimeToX(timeline.Duration);
    if (endX < max.x)
    {
        drawList->AddRectFilled({std::max(min.x, endX), min.y}, max, kOutside);
    }
    if (m_Settings.RangeEnabled)
    {
        const float inX = TimeToX(m_Settings.RangeIn);
        const float outX = TimeToX(m_Settings.RangeOut);
        if (inX > min.x)
        {
            drawList->AddRectFilled(min, {inX, max.y}, IM_COL32(0, 0, 0, 60));
        }
        if (outX < max.x)
        {
            drawList->AddRectFilled({outX, min.y}, max, IM_COL32(0, 0, 0, 60));
        }
        drawList->AddLine({inX, min.y}, {inX, max.y}, WithAlpha(kAccent, 0.5f));
        drawList->AddLine({outX, min.y}, {outX, max.y}, WithAlpha(kAccent, 0.5f));
    }
    // スナップのガイドライン
    if (m_SnapLine >= 0.0f && m_Drag != Drag::None && m_Drag != Drag::Pending && m_Drag != Drag::Marquee)
    {
        const float x = TimeToX(m_SnapLine);
        for (float y = min.y; y < max.y; y += 8.0f)
        {
            drawList->AddLine({x, y}, {x, std::min(max.y, y + 4.0f)}, kAccent, 1.0f);
        }
    }
    // 範囲選択の矩形
    if (m_Drag == Drag::Marquee)
    {
        const ImVec2 a = {std::min(m_MarqueeMin.x, m_MarqueeMax.x), std::min(m_MarqueeMin.y, m_MarqueeMax.y)};
        const ImVec2 b = {std::max(m_MarqueeMin.x, m_MarqueeMax.x), std::max(m_MarqueeMin.y, m_MarqueeMax.y)};
        drawList->AddRectFilled(a, b, IM_COL32(90, 140, 255, 40));
        drawList->AddRect(a, b, IM_COL32(120, 170, 255, 200));
    }
    // 再生ヘッド
    const float headX = TimeToX(timeline.CurrentTime);
    drawList->AddLine({headX, min.y}, {headX, max.y}, kPlayhead, 1.5f);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (m_RowsHovered && std::fabs(mouse.x - headX) < 3.0f && mouse.x >= min.x)
    {
        HitInfo hit;
        hit.Type = Hit::Playhead;
        hit.Time = timeline.CurrentTime;
        hit.Distance = std::fabs(mouse.x - headX);
        UpdateHover(hit);
    }
    drawList->PopClipRect();
}

void SequencerWindow::DrawHorizontalScrollbar(TimelineComponent& timeline, ImVec2 min, ImVec2 max)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled({m_TreeMin.x, min.y}, {max.x + ImGui::GetStyle().ScrollbarSize, max.y}, IM_COL32(18, 19, 23, 255));
    const float width = max.x - min.x;
    const float visible = width / m_Settings.PixelsPerSecond;
    const float total = std::max({timeline.Duration, m_Settings.ViewStart + visible, 0.01f});
    const float grabStart = min.x + m_Settings.ViewStart / total * width;
    const float grabWidth = std::max(20.0f, visible / total * width);
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##HScroll", {std::max(1.0f, width), max.y - min.y});
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActive())
    {
        const float delta = ImGui::GetIO().MouseDelta.x;
        m_Settings.ViewStart = std::max(0.0f, m_Settings.ViewStart + delta / width * total);
    }
    drawList->AddRectFilled({grabStart, min.y + 2.0f}, {std::min(max.x, grabStart + grabWidth), max.y - 2.0f},
                            hovered || ImGui::IsItemActive() ? IM_COL32(130, 135, 150, 255) : IM_COL32(85, 88, 100, 255),
                            4.0f);
}

// ============================================================================
// 曲線エディター
// ============================================================================
float SequencerWindow::ValueToY(float value) const
{
    const float range = std::max(1.0e-6f, m_CurveMax - m_CurveMin);
    return m_CurveMax2.y - 6.0f - (value - m_CurveMin) / range * (m_CurveMax2.y - m_CurveMin2.y - 12.0f);
}

float SequencerWindow::YToValue(float y) const
{
    const float range = std::max(1.0e-6f, m_CurveMax - m_CurveMin);
    return m_CurveMin + (m_CurveMax2.y - 6.0f - y) / std::max(1.0f, m_CurveMax2.y - m_CurveMin2.y - 12.0f) * range;
}

void SequencerWindow::BuildCurveSeries(TimelineComponent& timeline)
{
    m_CurveSeries.clear();
    // 表示するトラック：ツリーで選択した行 → 選択キーのトラック → 表示中の全プロパティ行
    std::vector<std::pair<int, int>> wanted; // (track, channel or -1)
    for (int index : m_VisibleRows)
    {
        const Row& row = m_Rows[index];
        if (m_SelectedRows.count(row.Id) == 0)
        {
            continue;
        }
        if (row.Kind == RowKind::Channel)
        {
            wanted.push_back({row.Track, row.Channel});
        }
        else
        {
            CollectRowTracks(index, m_ScratchTracks);
            for (int track : m_ScratchTracks)
            {
                wanted.push_back({track, -1});
            }
        }
    }
    if (wanted.empty())
    {
        for (const auto& ref : m_SelectedKeys)
        {
            wanted.push_back({ref.Track, -1});
        }
    }
    if (wanted.empty())
    {
        for (int index : m_VisibleRows)
        {
            const Row& row = m_Rows[index];
            if (row.Kind == RowKind::Property)
            {
                wanted.push_back({row.Track, -1});
            }
        }
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    for (const auto& entry : wanted)
    {
        if (entry.first < 0 || entry.first >= static_cast<int>(timeline.Tracks.size()))
        {
            continue;
        }
        const auto& track = timeline.Tracks[entry.first];
        const int dimension = TimelineCurve::ValueDimension(track.Property);
        if (dimension == 0)
        {
            continue;
        }
        const bool color = TimelineCurve::IsColorProperty(track.Property);
        for (int channel = 0; channel < dimension; ++channel)
        {
            if (entry.second >= 0 && entry.second != channel)
            {
                continue;
            }
            const int toggle = dimension == 1 ? 3 : channel;
            if (!m_Settings.CurveChannels[toggle])
            {
                continue;
            }
            // 同じトラックを (track,-1) と (track,channel) で重複させない
            bool exists = false;
            for (const auto& series : m_CurveSeries)
            {
                exists |= series.Track == entry.first && series.Channel == channel;
            }
            if (exists)
            {
                continue;
            }
            CurveSeries series;
            series.Track = entry.first;
            series.Channel = channel;
            series.Scale = DisplayScale(track.Property);
            Category category = Category::Generic;
            if (track.Property == TimelineProperty::CameraFov)
            {
                category = Category::CameraFov;
            }
            else if (track.Property == TimelineProperty::AnimationTime)
            {
                category = Category::Animation;
            }
            else if (track.Property >= TimelineProperty::LightIntensity)
            {
                category = Category::Light;
            }
            series.Color = dimension == 1 ? CategoryColor(category) : ChannelColor(channel, color);
            m_CurveSeries.push_back(series);
        }
    }
}

void SequencerWindow::DrawCurveEditor(TimelineComponent& timeline, ImDrawList* drawList, ImVec2 min, ImVec2 max)
{
    m_CurveMin2 = min;
    m_CurveMax2 = max;
    drawList->PushClipRect(min, max, true);
    drawList->AddRectFilled(min, max, IM_COL32(22, 23, 27, 255));
    BuildCurveSeries(timeline);

    // ---- サンプルのキャッシュ（データ・表示範囲・幅・系列が変わったときだけ再計算）
    const int sampleCount = std::max(2, static_cast<int>((max.x - min.x) / 2.0f));
    uint64_t key = TimelineCurve::ComputeChecksum(timeline);
    auto mix = [&](const void* data, size_t size) { TimelineCurve::HashBytes(key, data, size); };
    mix(&m_Settings.ViewStart, sizeof(float));
    mix(&m_Settings.PixelsPerSecond, sizeof(float));
    mix(&sampleCount, sizeof(int));
    for (const auto& series : m_CurveSeries)
    {
        mix(&series.Track, sizeof(int));
        mix(&series.Channel, sizeof(int));
    }
    if (key != m_CurveCacheKey || m_CurveSampleCount != sampleCount)
    {
        m_CurveCacheKey = key;
        m_CurveSampleCount = sampleCount;
        m_CurveSamples.resize(m_CurveSeries.size() * static_cast<size_t>(sampleCount));
        const float t0 = XToTime(min.x);
        const float dt = (XToTime(max.x) - t0) / (sampleCount - 1);
        for (size_t seriesIndex = 0; seriesIndex < m_CurveSeries.size(); ++seriesIndex)
        {
            auto& series = m_CurveSeries[seriesIndex];
            series.SampleOffset = seriesIndex * sampleCount;
            const auto& track = timeline.Tracks[series.Track];
            for (int sample = 0; sample < sampleCount; ++sample)
            {
                DirectX::XMFLOAT4 value{};
                const auto result = TimelineCurve::EvaluateTrack(track, std::max(0.0f, t0 + dt * sample), value);
                m_CurveSamples[series.SampleOffset + sample] =
                    result == TimelineCurve::TrackSample::None ? NAN
                                                               : TimelineCurve::GetComponent(value, series.Channel) *
                                                                     series.Scale;
            }
        }
    }
    else
    {
        for (size_t seriesIndex = 0; seriesIndex < m_CurveSeries.size(); ++seriesIndex)
        {
            m_CurveSeries[seriesIndex].SampleOffset = seriesIndex * sampleCount;
        }
    }
    // ---- 値の範囲（自動調整）
    if (m_CurveAutoFit && m_Drag != Drag::CurveKeys && m_Drag != Drag::CurveHandle)
    {
        float low = FLT_MAX;
        float high = -FLT_MAX;
        for (float v : m_CurveSamples)
        {
            if (!std::isnan(v))
            {
                low = std::min(low, v);
                high = std::max(high, v);
            }
        }
        if (low == FLT_MAX)
        {
            low = -1.0f;
            high = 1.0f;
        }
        if (high - low < 1.0e-3f)
        {
            low -= 1.0f;
            high += 1.0f;
        }
        const float margin = (high - low) * 0.1f;
        m_CurveMin = low - margin;
        m_CurveMax = high + margin;
    }
    // ---- 水平グリッドと値ラベル
    {
        const float range = m_CurveMax - m_CurveMin;
        const float rough = range / std::max(1.0f, (max.y - min.y) / 40.0f);
        const float magnitude = std::pow(10.0f, std::floor(std::log10(std::max(rough, 1.0e-6f))));
        float step = magnitude;
        for (float factor : {1.0f, 2.0f, 5.0f, 10.0f})
        {
            if (magnitude * factor >= rough)
            {
                step = magnitude * factor;
                break;
            }
        }
        char label[32];
        for (float v = std::ceil(m_CurveMin / step) * step; v <= m_CurveMax; v += step)
        {
            const float y = ValueToY(v);
            drawList->AddLine({min.x, y}, {max.x, y}, std::fabs(v) < step * 0.01f ? kGridMajor : kGridMinor);
            std::snprintf(label, sizeof(label), step < 0.1f ? "%.3f" : (step < 1.0f ? "%.2f" : "%.1f"), v);
            drawList->AddText({min.x + 4.0f, y - ImGui::GetFontSize() - 1.0f}, kTextDim, label);
        }
    }
    // ---- 曲線
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool graphHovered = m_RowsHovered && mouse.x >= min.x && mouse.x < max.x;
    for (const auto& series : m_CurveSeries)
    {
        m_ScratchPoints.clear();
        for (int sample = 0; sample < sampleCount; ++sample)
        {
            const float v = m_CurveSamples[series.SampleOffset + sample];
            if (std::isnan(v))
            {
                if (m_ScratchPoints.size() >= 2)
                {
                    drawList->AddPolyline(m_ScratchPoints.data(), static_cast<int>(m_ScratchPoints.size()), series.Color,
                                          ImDrawFlags_None, 1.6f);
                }
                m_ScratchPoints.clear();
                continue;
            }
            m_ScratchPoints.push_back({min.x + (max.x - min.x) * sample / (sampleCount - 1), ValueToY(v)});
        }
        if (m_ScratchPoints.size() >= 2)
        {
            drawList->AddPolyline(m_ScratchPoints.data(), static_cast<int>(m_ScratchPoints.size()), series.Color,
                                  ImDrawFlags_None, 1.6f);
        }
        // キー点とベジェハンドル
        const auto& track = timeline.Tracks[series.Track];
        for (int clipIndex = 0; clipIndex < static_cast<int>(track.Clips.size()); ++clipIndex)
        {
            const auto& clip = track.Clips[clipIndex];
            for (int keyIndex = 0; keyIndex < static_cast<int>(clip.Keys.size()); ++keyIndex)
            {
                const auto& k = clip.Keys[keyIndex];
                const float time = clip.StartTime + k.Time;
                const float x = TimeToX(time);
                if (x < min.x - 10.0f || x > max.x + 10.0f)
                {
                    continue;
                }
                const KeyRef ref{series.Track, clipIndex, keyIndex};
                const bool selected = IsKeySelected(ref);
                const float value = TimelineCurve::GetComponent(k.Value, series.Channel) * series.Scale;
                const ImVec2 c = {x, ValueToY(value)};
                // 選択キーがベジェ補間なら、次のキーまでの区間に 2 つのハンドルを表示
                if (selected && k.Interpolation == TimelineInterpolation::CubicBezier &&
                    keyIndex + 1 < static_cast<int>(clip.Keys.size()))
                {
                    const auto& next = clip.Keys[keyIndex + 1];
                    const float span = next.Time - k.Time;
                    const float v0 = value;
                    const float v1 = TimelineCurve::GetComponent(next.Value, series.Channel) * series.Scale;
                    const ImVec2 p1 = {TimeToX(time + k.Bezier.x * span), ValueToY(v0 + k.Bezier.y * (v1 - v0))};
                    const ImVec2 p2 = {TimeToX(time + k.Bezier.z * span), ValueToY(v0 + k.Bezier.w * (v1 - v0))};
                    const ImVec2 p3 = {TimeToX(time + span), ValueToY(v1)};
                    drawList->AddLine(c, p1, IM_COL32(255, 255, 255, 120));
                    drawList->AddLine(p3, p2, IM_COL32(255, 255, 255, 120));
                    for (int handle = 0; handle < 2; ++handle)
                    {
                        const ImVec2 h = handle == 0 ? p1 : p2;
                        const float distance = std::fabs(mouse.x - h.x) + std::fabs(mouse.y - h.y);
                        const bool hovered = graphHovered && distance < 8.0f;
                        drawList->AddCircleFilled(h, hovered ? 5.0f : 4.0f, hovered ? kAccent : IM_COL32(230, 230, 240, 255));
                        if (hovered)
                        {
                            HitInfo hit;
                            hit.Type = Hit::CurveHandle;
                            hit.Key = ref;
                            hit.Channel = series.Channel;
                            hit.Handle = handle;
                            hit.Distance = distance;
                            UpdateHover(hit);
                        }
                    }
                }
                const float distance = std::fabs(mouse.x - c.x) + std::fabs(mouse.y - c.y);
                const bool hovered = graphHovered && distance < 8.0f;
                DrawDiamond(drawList, c, hovered ? 6.0f : 4.5f, selected ? kSelection : series.Color,
                            selected ? kAccent : IM_COL32(0, 0, 0, 200), 1.2f);
                if (hovered)
                {
                    HitInfo hit;
                    hit.Type = Hit::CurveKey;
                    hit.Key = ref;
                    hit.Channel = series.Channel;
                    hit.Time = time;
                    hit.Distance = distance;
                    UpdateHover(hit);
                    char tip[64];
                    char now[32];
                    FormatTime(time, now, sizeof(now));
                    std::snprintf(tip, sizeof(tip), "%s  %s = %.4g", now, ChannelName(series.Channel,
                                  TimelineCurve::IsColorProperty(track.Property)), value);
                    ImGui::SetTooltip("%s", tip);
                }
            }
        }
    }
    // 凡例
    float legendY = min.y + 4.0f;
    for (const auto& series : m_CurveSeries)
    {
        const auto& track = timeline.Tracks[series.Track];
        char text[128];
        std::snprintf(text, sizeof(text), "%s %s.%s%s", m_Backend.GetEntityName(track.Target), PropertyLabel(track.Property),
                      ChannelName(series.Channel, TimelineCurve::IsColorProperty(track.Property)),
                      series.Scale != 1.0f ? " (度)" : "");
        const ImVec2 size = ImGui::CalcTextSize(text);
        drawList->AddRectFilled({max.x - size.x - 26.0f, legendY}, {max.x - 4.0f, legendY + size.y + 2.0f},
                                IM_COL32(0, 0, 0, 120), 3.0f);
        drawList->AddRectFilled({max.x - size.x - 22.0f, legendY + 5.0f}, {max.x - size.x - 12.0f, legendY + size.y - 3.0f},
                                series.Color);
        drawList->AddText({max.x - size.x - 8.0f, legendY + 1.0f}, kText, text);
        legendY += size.y + 4.0f;
        if (legendY > max.y - 20.0f)
        {
            break;
        }
    }
    if (m_CurveSeries.empty())
    {
        drawList->AddText({min.x + 12.0f, min.y + 10.0f}, kTextDim,
                          "左のツリーでプロパティを選択すると曲線を表示します（Ctrl+クリックで複数）");
    }
    drawList->PopClipRect();
    if (graphHovered && m_Hover.Type == Hit::None)
    {
        HitInfo hit;
        hit.Type = Hit::CurveEmpty;
        hit.Time = XToTime(mouse.x);
        UpdateHover(hit);
    }
}

// ============================================================================
// 入力
// ============================================================================
bool SequencerWindow::IsKeySelected(const KeyRef& ref) const
{
    return std::binary_search(m_SelectedKeys.begin(), m_SelectedKeys.end(), ref);
}

void SequencerWindow::SelectKey(const KeyRef& ref, bool toggle)
{
    const auto it = std::lower_bound(m_SelectedKeys.begin(), m_SelectedKeys.end(), ref);
    if (it != m_SelectedKeys.end() && *it == ref)
    {
        if (toggle)
        {
            m_SelectedKeys.erase(it);
        }
        return;
    }
    m_SelectedKeys.insert(it, ref);
}

bool SequencerWindow::IsClipSelected(const ClipRef& ref) const
{
    return std::find(m_SelectedClips.begin(), m_SelectedClips.end(), ref) != m_SelectedClips.end();
}

void SequencerWindow::HandleWheel(TimelineComponent& timeline, bool overTimeline, bool overTree)
{
    (void)timeline;
    (void)overTree;
    ImGuiIO& io = ImGui::GetIO();
    float wheelY = io.MouseWheel;
    float wheelX = io.MouseWheelH;
    if (io.KeyShift && wheelX == 0.0f)
    {
        wheelX = wheelY;
        wheelY = 0.0f;
    }
    if (wheelX == 0.0f && wheelY == 0.0f)
    {
        return;
    }
    const ImVec2 mouse = io.MousePos;
    if (io.KeyCtrl && overTimeline && wheelY != 0.0f)
    {
        // Ctrl + ホイール：マウス位置を中心に時間軸ズーム
        const float time = XToTime(mouse.x);
        m_Settings.PixelsPerSecond = std::clamp(m_Settings.PixelsPerSecond * std::pow(1.2f, wheelY), 2.0f, 5000.0f);
        m_Settings.ViewStart = std::max(0.0f, time - (mouse.x - m_TimelineMin.x) / m_Settings.PixelsPerSecond);
        return;
    }
    if (static_cast<Mode>(m_Settings.ModeValue) == Mode::Curve && overTimeline && wheelY != 0.0f)
    {
        // 曲線エディター：ホイールで値方向のズーム
        const float value = YToValue(mouse.y);
        const float factor = std::pow(0.85f, wheelY);
        m_CurveMin = value - (value - m_CurveMin) * factor;
        m_CurveMax = value + (m_CurveMax - value) * factor;
        m_CurveAutoFit = false;
        return;
    }
    if (wheelX != 0.0f && overTimeline)
    {
        m_Settings.ViewStart = std::max(0.0f, m_Settings.ViewStart - wheelX * 60.0f / m_Settings.PixelsPerSecond);
    }
    if (wheelY != 0.0f)
    {
        ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() - wheelY * m_Settings.RowHeight * 3.0f, 0.0f, ImGui::GetScrollMaxY()));
    }
}

void SequencerWindow::HandleRowsInput(TimelineComponent& timeline)
{
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    if (m_RowsHovered)
    {
        HandleWheel(timeline, mouse.x >= m_TimelineMin.x, mouse.x < m_TreeMax.x);
    }
    // 進行中のドラッグ（目盛りから開始したものを含む）
    if (m_Drag != Drag::None)
    {
        if (ImGui::IsMouseDown(m_DragButton))
        {
            if (m_Drag == Drag::Pending)
            {
                const float dx = mouse.x - m_PressMouse.x;
                const float dy = mouse.y - m_PressMouse.y;
                if (dx * dx + dy * dy > kDragThreshold * kDragThreshold)
                {
                    StartDrag(timeline);
                }
            }
            if (m_Drag != Drag::Pending && m_Drag != Drag::None)
            {
                UpdateDrag(timeline);
            }
        }
        else
        {
            if (m_Drag == Drag::Pending)
            {
                HandleClick(timeline);
            }
            else
            {
                EndDrag(timeline);
            }
            m_Drag = Drag::None;
            m_SnapLine = -1.0f;
        }
    }
    if (!m_RowsHovered || m_Drag != Drag::None)
    {
        // ホバー中のキーのツールチップ
        return;
    }
    if (m_Hover.Type == Hit::Key || m_Hover.Type == Hit::SummaryKey)
    {
        char now[32];
        FormatTime(m_Hover.Time, now, sizeof(now));
        if (m_Hover.Type == Hit::Key && TimelineCurve::IsValid(timeline, m_Hover.Key))
        {
            const auto& track = timeline.Tracks[m_Hover.Key.Track];
            const auto& key = track.Clips[m_Hover.Key.Clip].Keys[m_Hover.Key.Key];
            const float scale = DisplayScale(track.Property);
            const int dimension = TimelineCurve::ValueDimension(track.Property);
            if (dimension == 3)
            {
                ImGui::SetTooltip("%s  %s\n(%.3f, %.3f, %.3f)%s\n%s", now, PropertyLabel(track.Property),
                                  key.Value.x * scale, key.Value.y * scale, key.Value.z * scale,
                                  scale != 1.0f ? " 度" : "",
                                  kInterpolationNames[InterpolationIndex(key.Interpolation)]);
            }
            else
            {
                ImGui::SetTooltip("%s  %s\n%.4f%s\n%s", now, PropertyLabel(track.Property), key.Value.x * scale,
                                  scale != 1.0f ? " 度" : "",
                                  kInterpolationNames[InterpolationIndex(key.Interpolation)]);
            }
        }
        else
        {
            ImGui::SetTooltip("%s  （まとめキー：子トラックのキーをまとめて選択・移動）", now);
        }
    }
    else if (m_Hover.Type == Hit::Clip || m_Hover.Type == Hit::ClipLeft || m_Hover.Type == Hit::ClipRight)
    {
        if (m_Hover.Clip.Kind == ClipRef::Type::Audio)
        {
            ImGui::SetTooltip("ドラッグで曲の開始位置（スリップ）を調整\n曲はタイムライン 0 秒から再生されます");
        }
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        const Hit pressed = m_Hover.Type;
        const bool doubleClick = io.MouseClickedCount[ImGuiMouseButton_Left] >= 2;
        if (doubleClick)
        {
            HandleDoubleClick(timeline);
        }
        // クリック直後に掴み直した場合（ダブルクリック扱い）でも、キー・クリップ等はそのままドラッグできる
        if (!doubleClick || pressed == Hit::Key || pressed == Hit::SummaryKey || pressed == Hit::CurveKey ||
            pressed == Hit::CurveHandle || pressed == Hit::ClipLeft || pressed == Hit::ClipRight ||
            pressed == Hit::GroupClip || pressed == Hit::Playhead || pressed == Hit::TreeArrow)
        {
            BeginPendingDrag(timeline);
        }
    }
    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    {
        m_Drag = Drag::Pan;
        m_DragButton = ImGuiMouseButton_Middle;
        m_PressMouse = mouse;
        m_DragStartView = m_Settings.ViewStart;
        m_DragStartScalar = ImGui::GetScrollY();
        m_PressValue = (m_CurveMin + m_CurveMax) * 0.5f;
    }
    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        m_ContextHit = m_Hover;
        if (m_Hover.Type == Hit::Key || m_Hover.Type == Hit::CurveKey)
        {
            if (!IsKeySelected(m_Hover.Key))
            {
                m_SelectedKeys.clear();
                m_SelectedClips.clear();
                SelectKey(m_Hover.Key, false);
            }
        }
        else if (m_Hover.Type == Hit::SummaryKey)
        {
            CollectKeysAt(timeline, m_Hover.Row, m_Hover.Time, m_ScratchRefs);
            m_SelectedKeys.clear();
            for (const auto& ref : m_ScratchRefs)
            {
                SelectKey(ref, false);
            }
        }
        else if (m_Hover.Type == Hit::Clip || m_Hover.Type == Hit::ClipLeft || m_Hover.Type == Hit::ClipRight)
        {
            if (!IsClipSelected(m_Hover.Clip))
            {
                m_SelectedClips = {m_Hover.Clip};
                m_SelectedKeys.clear();
            }
        }
        if (m_Hover.Row >= 0)
        {
            m_ActiveRow = m_Rows[m_Hover.Row].Id;
        }
        if (mouse.x < m_TreeMax.x)
        {
            m_OpenTreeMenu = true;
        }
        else
        {
            m_OpenTimelineMenu = true;
        }
    }
}

void SequencerWindow::BeginPendingDrag(TimelineComponent& timeline)
{
    ImGuiIO& io = ImGui::GetIO();
    m_Press = m_Hover;
    m_Drag = Drag::Pending;
    m_DragButton = ImGuiMouseButton_Left;
    m_PressMouse = io.MousePos;
    m_PressTime = XToTime(io.MousePos.x);
    m_PlayheadGrab = false;
    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;
    switch (m_Press.Type)
    {
        case Hit::TreeArrow:
        {
            const Row& row = m_Rows[m_Press.Row];
            SetRowOpen(row.Id, !row.Open);
            m_Drag = Drag::None;
            break;
        }
        case Hit::TreeKeyButton:
            AddKeyAtRow(timeline, m_Press.Row, timeline.CurrentTime, true);
            m_Drag = Drag::None;
            break;
        case Hit::Key:
        case Hit::CurveKey:
        {
            m_DragStartScalar = IsKeySelected(m_Press.Key) ? 1.0f : 0.0f; // 押す前に選択済みだったか
            if (shift && m_AnchorRow >= 0 && m_Press.Row >= 0)
            {
                SelectKeysInRect(timeline, std::min(m_AnchorTime, m_Press.Time), std::max(m_AnchorTime, m_Press.Time),
                                 std::min(m_AnchorRow, m_Press.Row), std::max(m_AnchorRow, m_Press.Row), true);
                SelectKey(m_Press.Key, false);
            }
            else if (ctrl)
            {
                SelectKey(m_Press.Key, true);
            }
            else if (!IsKeySelected(m_Press.Key))
            {
                m_SelectedKeys.clear();
                SelectKey(m_Press.Key, false);
            }
            if (!ctrl)
            {
                m_SelectedClips.clear();
            }
            if (!shift)
            {
                m_AnchorRow = m_Press.Row;
                m_AnchorTime = m_Press.Time;
            }
            if (m_Press.Row >= 0)
            {
                m_ActiveRow = m_Rows[m_Press.Row].Id;
            }
            break;
        }
        case Hit::SummaryKey:
        {
            CollectKeysAt(timeline, m_Press.Row, m_Press.Time, m_ScratchRefs);
            bool all = !m_ScratchRefs.empty();
            for (const auto& ref : m_ScratchRefs)
            {
                all &= IsKeySelected(ref);
            }
            m_DragStartScalar = all ? 1.0f : 0.0f;
            if (!ctrl && !all)
            {
                m_SelectedKeys.clear();
            }
            for (const auto& ref : m_ScratchRefs)
            {
                if (ctrl && all)
                {
                    SelectKey(ref, true);
                }
                else
                {
                    SelectKey(ref, false);
                }
            }
            if (!ctrl)
            {
                m_SelectedClips.clear();
            }
            m_AnchorRow = m_Press.Row;
            m_AnchorTime = m_Press.Time;
            if (!m_ScratchRefs.empty())
            {
                m_Press.Key = m_ScratchRefs.front();
            }
            break;
        }
        case Hit::Clip:
        case Hit::ClipLeft:
        case Hit::ClipRight:
            m_DragStartScalar = IsClipSelected(m_Press.Clip) ? 1.0f : 0.0f;
            if (ctrl)
            {
                if (IsClipSelected(m_Press.Clip))
                {
                    m_SelectedClips.erase(std::find(m_SelectedClips.begin(), m_SelectedClips.end(), m_Press.Clip));
                }
                else
                {
                    m_SelectedClips.push_back(m_Press.Clip);
                }
            }
            else if (!IsClipSelected(m_Press.Clip))
            {
                m_SelectedClips = {m_Press.Clip};
            }
            if (!ctrl)
            {
                m_SelectedKeys.clear();
            }
            if (m_Press.Row >= 0)
            {
                m_ActiveRow = m_Rows[m_Press.Row].Id;
            }
            break;
        case Hit::Playhead:
            m_PlayheadGrab = true;
            break;
        default:
            break;
    }
}

void SequencerWindow::SnapshotTracks(const TimelineComponent& timeline)
{
    m_TrackSnapshot.clear();
    auto add = [&](int track) {
        if (track < 0 || track >= static_cast<int>(timeline.Tracks.size()))
        {
            return;
        }
        for (const auto& entry : m_TrackSnapshot)
        {
            if (entry.first == track)
            {
                return;
            }
        }
        m_TrackSnapshot.push_back({track, timeline.Tracks[track].Clips});
    };
    for (const auto& origin : m_KeyOrigins)
    {
        add(origin.Ref.Track);
    }
    for (const auto& origin : m_ClipOrigins)
    {
        if (origin.Ref.Kind == ClipRef::Type::Track)
        {
            add(origin.Ref.Track);
        }
    }
}

void SequencerWindow::RestoreTracks(TimelineComponent& timeline)
{
    for (const auto& entry : m_TrackSnapshot)
    {
        if (entry.first < static_cast<int>(timeline.Tracks.size()))
        {
            timeline.Tracks[entry.first].Clips = entry.second;
        }
    }
}

void SequencerWindow::BuildSnapTargets(const TimelineComponent& timeline, bool excludeSelection)
{
    m_SnapTargets.clear();
    m_SnapTargets.push_back(0.0f);
    m_SnapTargets.push_back(timeline.Duration);
    if (m_Drag != Drag::Scrub)
    {
        m_SnapTargets.push_back(timeline.CurrentTime);
    }
    if (m_Settings.RangeEnabled)
    {
        m_SnapTargets.push_back(m_Settings.RangeIn);
        m_SnapTargets.push_back(m_Settings.RangeOut);
    }
    for (int track = 0; track < static_cast<int>(timeline.Tracks.size()); ++track)
    {
        const auto& data = timeline.Tracks[track];
        for (int clip = 0; clip < static_cast<int>(data.Clips.size()); ++clip)
        {
            bool clipMoving = false;
            if (excludeSelection)
            {
                for (const auto& origin : m_ClipOrigins)
                {
                    clipMoving |= origin.Ref.Kind == ClipRef::Type::Track && origin.Ref.Track == track &&
                                  origin.Ref.Clip == clip;
                }
            }
            if (!clipMoving)
            {
                m_SnapTargets.push_back(data.Clips[clip].StartTime);
                m_SnapTargets.push_back(data.Clips[clip].StartTime + data.Clips[clip].Duration);
            }
            for (int key = 0; key < static_cast<int>(data.Clips[clip].Keys.size()); ++key)
            {
                if (excludeSelection && (clipMoving || IsKeySelected({track, clip, key})))
                {
                    continue;
                }
                m_SnapTargets.push_back(data.Clips[clip].StartTime + data.Clips[clip].Keys[key].Time);
            }
        }
    }
    for (int index = 0; index < static_cast<int>(timeline.Monitors.size()); ++index)
    {
        bool moving = false;
        for (const auto& origin : m_ClipOrigins)
        {
            moving |= origin.Ref.Kind == ClipRef::Type::Video && origin.Ref.Track == index;
        }
        if (!moving || !excludeSelection)
        {
            m_SnapTargets.push_back(timeline.Monitors[index].StartTime);
            m_SnapTargets.push_back(TimelineCurve::MonitorEndTime(timeline.Monitors[index], timeline.Duration));
        }
    }
    std::sort(m_SnapTargets.begin(), m_SnapTargets.end());
    m_SnapTargets.erase(std::unique(m_SnapTargets.begin(), m_SnapTargets.end()), m_SnapTargets.end());
}

void SequencerWindow::StartDrag(TimelineComponent& timeline)
{
    m_KeyOrigins.clear();
    m_ClipOrigins.clear();
    const bool curveMode = static_cast<Mode>(m_Settings.ModeValue) == Mode::Curve;
    switch (m_Press.Type)
    {
        case Hit::Key:
        case Hit::SummaryKey:
        case Hit::CurveKey:
        {
            if (m_SelectedKeys.empty())
            {
                m_Drag = Drag::None;
                return;
            }
            for (const auto& ref : m_SelectedKeys)
            {
                m_KeyOrigins.push_back({ref, TimelineCurve::KeyAbsoluteTime(timeline, ref)});
            }
            m_PressTime = m_Press.Time;
            if (TimelineCurve::IsValid(timeline, m_Press.Key))
            {
                m_PressTime = TimelineCurve::KeyAbsoluteTime(timeline, m_Press.Key);
            }
            m_Drag = m_Press.Type == Hit::CurveKey ? Drag::CurveKeys : Drag::Keys;
            BuildSnapTargets(timeline, true);
            BeginEdit();
            SnapshotTracks(timeline);
            break;
        }
        case Hit::Clip:
        case Hit::ClipLeft:
        case Hit::ClipRight:
        {
            const bool single = m_Press.Type != Hit::Clip;
            auto addOrigin = [&](const ClipRef& ref) {
                ClipOrigin origin;
                origin.Ref = ref;
                if (ref.Kind == ClipRef::Type::Track)
                {
                    const auto& clip = timeline.Tracks[ref.Track].Clips[ref.Clip];
                    origin.Start = clip.StartTime;
                    origin.Duration = clip.Duration;
                }
                else if (ref.Kind == ClipRef::Type::Video)
                {
                    const auto& monitor = timeline.Monitors[ref.Track];
                    origin.Start = monitor.StartTime;
                    origin.End = monitor.EndTime;
                    origin.Offset = monitor.Offset;
                    origin.Duration = TimelineCurve::MonitorEndTime(monitor, timeline.Duration) - monitor.StartTime;
                }
                else
                {
                    origin.Start = -timeline.MusicOffset;
                    origin.Offset = timeline.MusicOffset;
                }
                m_ClipOrigins.push_back(origin);
            };
            addOrigin(m_Press.Clip); // 先頭が掴んだクリップ（スナップの基準）
            if (!single)
            {
                for (const auto& ref : m_SelectedClips)
                {
                    if (!(ref == m_Press.Clip))
                    {
                        addOrigin(ref);
                    }
                }
            }
            if (m_Press.Clip.Kind == ClipRef::Type::Audio && single)
            {
                m_Drag = Drag::Clips; // 音声は区間の移動（スリップ）のみ
            }
            else
            {
                m_Drag = m_Press.Type == Hit::ClipLeft ? Drag::ClipLeft
                                                       : (m_Press.Type == Hit::ClipRight ? Drag::ClipRight : Drag::Clips);
            }
            BuildSnapTargets(timeline, true);
            BeginEdit();
            SnapshotTracks(timeline);
            break;
        }
        case Hit::GroupClip:
        {
            CollectRowTracks(m_Press.Row, m_ScratchTracks);
            for (int track : m_ScratchTracks)
            {
                for (int clip = 0; clip < static_cast<int>(timeline.Tracks[track].Clips.size()); ++clip)
                {
                    const auto& data = timeline.Tracks[track].Clips[clip];
                    m_ClipOrigins.push_back({{ClipRef::Type::Track, track, clip}, data.StartTime, data.Duration, 0, 0});
                }
            }
            if (m_ClipOrigins.empty())
            {
                m_Drag = Drag::None;
                return;
            }
            // 最も早いクリップを基準にする
            std::sort(m_ClipOrigins.begin(), m_ClipOrigins.end(),
                      [](const ClipOrigin& a, const ClipOrigin& b) { return a.Start < b.Start; });
            m_Drag = Drag::Clips;
            BuildSnapTargets(timeline, true);
            BeginEdit();
            SnapshotTracks(timeline);
            break;
        }
        case Hit::Playhead:
            m_Drag = Drag::Scrub;
            BuildSnapTargets(timeline, false);
            if (timeline.IsPlaying)
            {
                SetPlaying(timeline, false);
            }
            break;
        case Hit::RowResize:
            m_Drag = Drag::RowResize;
            m_ResizeRow = m_Rows[m_Press.Row].Id;
            m_DragStartScalar = m_Rows[m_Press.Row].Height;
            break;
        case Hit::CurveHandle:
            m_Drag = Drag::CurveHandle;
            BeginEdit();
            break;
        case Hit::RowEmpty:
        case Hit::CurveEmpty:
            m_Drag = Drag::Marquee;
            m_MarqueeMin = m_PressMouse;
            m_MarqueeMax = ImGui::GetIO().MousePos;
            break;
        default:
            m_Drag = Drag::None;
            break;
    }
    (void)curveMode;
}

void SequencerWindow::UpdateDrag(TimelineComponent& timeline)
{
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const float pointerTime = XToTime(mouse.x);
    const float deltaTime = (mouse.x - m_PressMouse.x) / m_Settings.PixelsPerSecond;
    char a[32];
    char b[32];
    char c[32];
    switch (m_Drag)
    {
        case Drag::Scrub:
            SetTime(timeline, SnapTime(pointerTime, true), false);
            FormatTime(timeline.CurrentTime, a, sizeof(a));
            ImGui::SetTooltip("%s", a);
            break;
        case Drag::RangeIn:
            m_Settings.RangeIn = std::clamp(SnapTime(pointerTime, true), 0.0f, m_Settings.RangeOut - 0.01f);
            break;
        case Drag::RangeOut:
            m_Settings.RangeOut = std::max(m_Settings.RangeIn + 0.01f, SnapTime(pointerTime, true));
            break;
        case Drag::DurationEnd:
            timeline.Duration = std::max(0.01f, SnapTime(pointerTime, false));
            FormatTime(timeline.Duration, a, sizeof(a));
            ImGui::SetTooltip("長さ %s", a);
            MarkChanged(timeline);
            break;
        case Drag::Pan:
            m_Settings.ViewStart = std::max(0.0f, m_DragStartView - (mouse.x - m_PressMouse.x) / m_Settings.PixelsPerSecond);
            if (static_cast<Mode>(m_Settings.ModeValue) == Mode::Curve)
            {
                const float dv = (mouse.y - m_PressMouse.y) * (m_CurveMax - m_CurveMin) /
                                 std::max(1.0f, m_CurveMax2.y - m_CurveMin2.y);
                const float half = (m_CurveMax - m_CurveMin) * 0.5f;
                m_CurveMin = m_PressValue + dv - half;
                m_CurveMax = m_PressValue + dv + half;
                m_CurveAutoFit = false;
            }
            else
            {
                ImGui::SetScrollY(std::clamp(m_DragStartScalar - (mouse.y - m_PressMouse.y), 0.0f, ImGui::GetScrollMaxY()));
            }
            break;
        case Drag::RowResize:
            m_RowHeights[m_ResizeRow] = std::clamp(m_DragStartScalar + (mouse.y - m_PressMouse.y), 14.0f, 240.0f);
            break;
        case Drag::Marquee:
            m_MarqueeMax = mouse;
            break;
        case Drag::Keys:
        case Drag::CurveKeys:
        {
            float delta = SnapTime(m_PressTime + deltaTime, true) - m_PressTime;
            float valueDelta = 0.0f;
            const bool curve = m_Drag == Drag::CurveKeys;
            if (curve)
            {
                valueDelta = YToValue(mouse.y) - YToValue(m_PressMouse.y);
                // Shift：大きく動いた軸だけに制限
                if (io.KeyShift)
                {
                    if (std::fabs(mouse.x - m_PressMouse.x) > std::fabs(mouse.y - m_PressMouse.y))
                    {
                        valueDelta = 0.0f;
                    }
                    else
                    {
                        delta = 0.0f;
                        m_SnapLine = -1.0f;
                    }
                }
            }
            RestoreTracks(timeline);
            TimelineCurve::MoveKeys(timeline, m_KeyOrigins, delta, m_SelectedKeys);
            std::sort(m_SelectedKeys.begin(), m_SelectedKeys.end());
            if (curve && valueDelta != 0.0f && m_Press.Channel >= 0)
            {
                for (const auto& ref : m_SelectedKeys)
                {
                    auto& track = timeline.Tracks[ref.Track];
                    if (TimelineCurve::ValueDimension(track.Property) <= m_Press.Channel)
                    {
                        continue;
                    }
                    auto& value = track.Clips[ref.Clip].Keys[ref.Key].Value;
                    TimelineCurve::SetComponent(value, m_Press.Channel,
                                                TimelineCurve::GetComponent(value, m_Press.Channel) +
                                                    valueDelta / DisplayScale(track.Property));
                }
            }
            m_DragDelta = delta;
            MarkChanged(timeline);
            FormatTime(m_PressTime + delta, a, sizeof(a));
            FormatTime(delta, b, sizeof(b));
            if (curve)
            {
                ImGui::SetTooltip("%s  (%s%s)\n値 %+.4g", a, delta >= 0.0f ? "+" : "", b, valueDelta);
            }
            else
            {
                ImGui::SetTooltip("%s  (%s%s)  %zu キー", a, delta >= 0.0f ? "+" : "", b, m_SelectedKeys.size());
            }
            break;
        }
        case Drag::CurveHandle:
        {
            if (!TimelineCurve::IsValid(timeline, m_Press.Key))
            {
                break;
            }
            auto& track = timeline.Tracks[m_Press.Key.Track];
            auto& clip = track.Clips[m_Press.Key.Clip];
            if (m_Press.Key.Key + 1 >= static_cast<int>(clip.Keys.size()))
            {
                break;
            }
            auto& key = clip.Keys[m_Press.Key.Key];
            const auto& next = clip.Keys[m_Press.Key.Key + 1];
            const float scale = DisplayScale(track.Property);
            const float t0 = clip.StartTime + key.Time;
            const float span = std::max(1.0e-5f, next.Time - key.Time);
            const float v0 = TimelineCurve::GetComponent(key.Value, m_Press.Channel) * scale;
            const float v1 = TimelineCurve::GetComponent(next.Value, m_Press.Channel) * scale;
            const float x = std::clamp((pointerTime - t0) / span, 0.0f, 1.0f);
            const bool hasRange = std::fabs(v1 - v0) > 1.0e-6f;
            const float y = hasRange ? (YToValue(mouse.y) - v0) / (v1 - v0) : (m_Press.Handle == 0 ? key.Bezier.y : key.Bezier.w);
            if (m_Press.Handle == 0)
            {
                key.Bezier.x = x;
                key.Bezier.y = std::clamp(y, -4.0f, 5.0f);
            }
            else
            {
                key.Bezier.z = x;
                key.Bezier.w = std::clamp(y, -4.0f, 5.0f);
            }
            MarkChanged(timeline);
            ImGui::SetTooltip("cubic-bezier(%.2f, %.2f, %.2f, %.2f)", key.Bezier.x, key.Bezier.y, key.Bezier.z, key.Bezier.w);
            break;
        }
        case Drag::Clips:
        case Drag::ClipLeft:
        case Drag::ClipRight:
        {
            if (m_ClipOrigins.empty())
            {
                break;
            }
            RestoreTracks(timeline);
            const ClipOrigin& lead = m_ClipOrigins.front();
            constexpr float kMinimum = 1.0f / 240.0f;
            if (m_Drag == Drag::Clips)
            {
                float delta = 0.0f;
                if (lead.Ref.Kind == ClipRef::Type::Audio)
                {
                    delta = SnapTime(lead.Start + deltaTime, true) - lead.Start;
                }
                else
                {
                    // 開始端・終了端のどちらか近い方をスナップ
                    const float startSnap = SnapTime(lead.Start + deltaTime, true) - lead.Start;
                    const float startLine = m_SnapLine;
                    const float endSnap = SnapTime(lead.Start + lead.Duration + deltaTime, true) - lead.Start - lead.Duration;
                    if (std::fabs(startSnap - deltaTime) <= std::fabs(endSnap - deltaTime))
                    {
                        delta = startSnap;
                        m_SnapLine = startLine;
                    }
                    else
                    {
                        delta = endSnap;
                    }
                }
                // どのクリップも 0 秒より前に出ない
                for (const auto& origin : m_ClipOrigins)
                {
                    if (origin.Ref.Kind != ClipRef::Type::Audio)
                    {
                        delta = std::max(delta, -origin.Start);
                    }
                }
                for (const auto& origin : m_ClipOrigins)
                {
                    switch (origin.Ref.Kind)
                    {
                        case ClipRef::Type::Track:
                            timeline.Tracks[origin.Ref.Track].Clips[origin.Ref.Clip].StartTime = origin.Start + delta;
                            break;
                        case ClipRef::Type::Video:
                        {
                            auto& monitor = timeline.Monitors[origin.Ref.Track];
                            monitor.StartTime = origin.Start + delta;
                            monitor.EndTime = origin.End > origin.Start ? origin.End + delta : origin.End;
                            break;
                        }
                        case ClipRef::Type::Audio:
                        {
                            const float limit = timeline.MusicDuration > 0.0f ? timeline.MusicDuration : 36000.0f;
                            timeline.MusicOffset = std::clamp(origin.Offset - delta, 0.0f, limit);
                            timeline.SeekRequested = true;
                            break;
                        }
                    }
                }
                m_DragDelta = delta;
                if (lead.Ref.Kind == ClipRef::Type::Audio)
                {
                    ImGui::SetTooltip("曲の開始位置 %.3f 秒", timeline.MusicOffset);
                }
                else
                {
                    FormatTime(lead.Start + delta, a, sizeof(a));
                    FormatTime(lead.Start + delta + lead.Duration, b, sizeof(b));
                    FormatTime(lead.Duration, c, sizeof(c));
                    ImGui::SetTooltip("%s – %s  (%s)", a, b, c);
                }
            }
            else if (m_Drag == Drag::ClipLeft)
            {
                const float end = lead.Start + lead.Duration;
                if (lead.Ref.Kind == ClipRef::Type::Track)
                {
                    auto& clip = timeline.Tracks[lead.Ref.Track].Clips[lead.Ref.Clip];
                    const float start = std::clamp(SnapTime(lead.Start + deltaTime, true), 0.0f, end - kMinimum);
                    TimelineCurve::SetClipStartKeepContent(clip, start); // 内容（キーの絶対時刻）は動かさない
                    clip.Duration = end - start;
                }
                else if (lead.Ref.Kind == ClipRef::Type::Video)
                {
                    auto& monitor = timeline.Monitors[lead.Ref.Track];
                    const float start =
                        std::clamp(SnapTime(lead.Start + deltaTime, true), std::max(0.0f, lead.Start - lead.Offset),
                                   end - kMinimum);
                    monitor.StartTime = start;
                    monitor.Offset = lead.Offset + (start - lead.Start);
                    monitor.EndTime = lead.End > lead.Start ? lead.End : (monitor.Loop ? lead.End : end);
                }
                FormatTime(lead.Start + deltaTime, a, sizeof(a));
                ImGui::SetTooltip("開始 %s", a);
            }
            else
            {
                if (lead.Ref.Kind == ClipRef::Type::Track)
                {
                    auto& clip = timeline.Tracks[lead.Ref.Track].Clips[lead.Ref.Clip];
                    const float end = std::max(clip.StartTime + kMinimum,
                                               SnapTime(lead.Start + lead.Duration + deltaTime, true));
                    clip.Duration = end - clip.StartTime;
                    FormatTime(end, a, sizeof(a));
                    FormatTime(clip.Duration, b, sizeof(b));
                    ImGui::SetTooltip("終了 %s  (%s)", a, b);
                }
                else if (lead.Ref.Kind == ClipRef::Type::Video)
                {
                    auto& monitor = timeline.Monitors[lead.Ref.Track];
                    monitor.EndTime =
                        std::max(monitor.StartTime + kMinimum, SnapTime(lead.Start + lead.Duration + deltaTime, true));
                    FormatTime(monitor.EndTime, a, sizeof(a));
                    ImGui::SetTooltip("終了 %s", a);
                }
            }
            MarkChanged(timeline);
            break;
        }
        default:
            break;
    }
}

void SequencerWindow::EndDrag(TimelineComponent& timeline)
{
    switch (m_Drag)
    {
        case Drag::Marquee:
        {
            const float x0 = std::min(m_MarqueeMin.x, m_MarqueeMax.x);
            const float x1 = std::max(m_MarqueeMin.x, m_MarqueeMax.x);
            const float y0 = std::min(m_MarqueeMin.y, m_MarqueeMax.y);
            const float y1 = std::max(m_MarqueeMin.y, m_MarqueeMax.y);
            const bool additive = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
            if (!additive)
            {
                m_SelectedKeys.clear();
                m_SelectedClips.clear();
            }
            if (static_cast<Mode>(m_Settings.ModeValue) == Mode::Curve)
            {
                for (const auto& series : m_CurveSeries)
                {
                    const auto& track = timeline.Tracks[series.Track];
                    for (int clip = 0; clip < static_cast<int>(track.Clips.size()); ++clip)
                    {
                        for (int key = 0; key < static_cast<int>(track.Clips[clip].Keys.size()); ++key)
                        {
                            const auto& k = track.Clips[clip].Keys[key];
                            const float x = TimeToX(track.Clips[clip].StartTime + k.Time);
                            const float y = ValueToY(TimelineCurve::GetComponent(k.Value, series.Channel) * series.Scale);
                            if (x >= x0 && x <= x1 && y >= y0 && y <= y1)
                            {
                                SelectKey({series.Track, clip, key}, false);
                            }
                        }
                    }
                }
            }
            else
            {
                int row0 = -1;
                int row1 = -1;
                for (int index : m_VisibleRows)
                {
                    const Row& row = m_Rows[index];
                    if (row.ScreenY + row.Height >= y0 && row.ScreenY <= y1)
                    {
                        row0 = row0 < 0 ? index : row0;
                        row1 = index;
                    }
                }
                if (row0 >= 0)
                {
                    SelectKeysInRect(timeline, XToTime(x0), XToTime(x1), row0, row1, true);
                }
            }
            break;
        }
        case Drag::Keys:
        case Drag::CurveKeys:
        case Drag::Clips:
        case Drag::ClipLeft:
        case Drag::ClipRight:
        case Drag::CurveHandle:
        case Drag::DurationEnd:
            CommitEdit();
            break;
        default:
            break;
    }
    m_KeyOrigins.clear();
    m_ClipOrigins.clear();
    m_TrackSnapshot.clear();
}

void SequencerWindow::SelectKeysInRect(TimelineComponent& timeline, float t0, float t1, int row0, int row1,
                                       bool additive)
{
    if (!additive)
    {
        m_SelectedKeys.clear();
    }
    bool inside = false;
    for (int index : m_VisibleRows)
    {
        if (index == row0)
        {
            inside = true;
        }
        if (inside)
        {
            const Row& row = m_Rows[index];
            if (!RowShowsClips(timeline, row) || row.Track < 0)
            {
                CollectRowTracks(index, m_ScratchTracks);
                for (int track : m_ScratchTracks)
                {
                    const auto& data = timeline.Tracks[track];
                    if (data.Property == TimelineProperty::CameraCut)
                    {
                        continue;
                    }
                    for (int clip = 0; clip < static_cast<int>(data.Clips.size()); ++clip)
                    {
                        for (int key = 0; key < static_cast<int>(data.Clips[clip].Keys.size()); ++key)
                        {
                            const float time = data.Clips[clip].StartTime + data.Clips[clip].Keys[key].Time;
                            if (time >= t0 - 1.0e-5f && time <= t1 + 1.0e-5f)
                            {
                                SelectKey({track, clip, key}, false);
                            }
                        }
                    }
                }
            }
        }
        if (index == row1)
        {
            break;
        }
    }
}

void SequencerWindow::HandleClick(TimelineComponent& timeline)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;
    switch (m_Press.Type)
    {
        case Hit::Key:
        case Hit::CurveKey:
            if (!ctrl && !shift && m_DragStartScalar > 0.5f)
            {
                // 選択済みのキーを（ドラッグせずに）クリック：そのキーだけを選択
                m_SelectedKeys.clear();
                SelectKey(m_Press.Key, false);
            }
            break;
        case Hit::SummaryKey:
            if (!ctrl && !shift && m_DragStartScalar > 0.5f)
            {
                CollectKeysAt(timeline, m_Press.Row, m_Press.Time, m_ScratchRefs);
                m_SelectedKeys.clear();
                for (const auto& ref : m_ScratchRefs)
                {
                    SelectKey(ref, false);
                }
            }
            break;
        case Hit::Clip:
        case Hit::ClipLeft:
        case Hit::ClipRight:
            if (!ctrl && m_DragStartScalar > 0.5f)
            {
                m_SelectedClips = {m_Press.Clip};
            }
            break;
        case Hit::TreeRow:
        case Hit::RowResize:
        {
            const Row& row = m_Rows[m_Press.Row];
            if (ctrl)
            {
                if (!m_SelectedRows.erase(row.Id))
                {
                    m_SelectedRows.insert(row.Id);
                }
            }
            else
            {
                m_SelectedRows.clear();
                m_SelectedRows.insert(row.Id);
                m_SelectedKeys.clear();
                m_SelectedClips.clear();
            }
            m_ActiveRow = row.Id;
            if (row.Kind == RowKind::Video || row.Kind == RowKind::VideoClip)
            {
                m_SelectedClips = {{ClipRef::Type::Video, row.Monitor, 0}};
            }
            else if (row.Kind == RowKind::Audio || row.Kind == RowKind::AudioClip)
            {
                m_SelectedClips = {{ClipRef::Type::Audio, 0, 0}};
            }
            break;
        }
        case Hit::RowEmpty:
            if (shift && m_AnchorRow >= 0 && m_Press.Row >= 0)
            {
                SelectKeysInRect(timeline, std::min(m_AnchorTime, m_Press.Time), std::max(m_AnchorTime, m_Press.Time),
                                 std::min(m_AnchorRow, m_Press.Row), std::max(m_AnchorRow, m_Press.Row), true);
            }
            else if (!ctrl)
            {
                m_SelectedKeys.clear();
                m_SelectedClips.clear();
                if (m_Press.Row >= 0)
                {
                    m_ActiveRow = m_Rows[m_Press.Row].Id;
                    m_SelectedRows.clear();
                    m_SelectedRows.insert(m_ActiveRow);
                }
            }
            break;
        case Hit::CurveEmpty:
            if (!ctrl && !shift)
            {
                m_SelectedKeys.clear();
            }
            break;
        case Hit::Playhead:
            break;
        default:
            break;
    }
}

void SequencerWindow::HandleDoubleClick(TimelineComponent& timeline)
{
    const HitInfo hit = m_Hover;
    switch (hit.Type)
    {
        case Hit::RowEmpty:
        case Hit::Key:
        case Hit::SummaryKey:
        {
            if (hit.Row < 0)
            {
                break;
            }
            const Row& row = m_Rows[hit.Row];
            if (RowShowsClips(timeline, row))
            {
                if (row.Track >= 0 && hit.Type == Hit::RowEmpty)
                {
                    const float saved = timeline.CurrentTime;
                    timeline.CurrentTime = SnapTime(hit.Time, true);
                    BeginEdit();
                    AddClipAtPlayhead(timeline, row.Track);
                    timeline.CurrentTime = saved;
                    MarkChanged(timeline);
                    CommitEdit();
                }
                break;
            }
            if (hit.Type == Hit::RowEmpty)
            {
                AddKeyAtRow(timeline, hit.Row, SnapTime(hit.Time, true), false);
            }
            break;
        }
        case Hit::CurveEmpty:
        {
            BeginEdit();
            m_SelectedKeys.clear();
            const float time = SnapTime(hit.Time, true);
            int lastTrack = -1;
            for (const auto& series : m_CurveSeries)
            {
                if (series.Track == lastTrack)
                {
                    continue;
                }
                lastTrack = series.Track;
                auto& track = timeline.Tracks[series.Track];
                DirectX::XMFLOAT4 value{};
                if (TimelineCurve::EvaluateTrack(track, time, value) == TimelineCurve::TrackSample::None)
                {
                    m_Backend.ReadProperty(track.Target, track.Property, track.FogVolumeIndex, value);
                }
                const auto added = TimelineCurve::AddKeyAt(track, time, value);
                SelectKey({series.Track, added.first, added.second}, false);
            }
            MarkChanged(timeline);
            CommitEdit();
            break;
        }
        case Hit::TreeRow:
        {
            const Row& row = m_Rows[hit.Row];
            if (row.Kind == RowKind::Object && row.Entity != g_kINVALID_ENTITY)
            {
                m_Backend.SetEditorSelection(row.Entity); // シーンで選択
            }
            else if (row.HasChildren)
            {
                SetRowOpen(row.Id, !row.Open);
            }
            break;
        }
        case Hit::Clip:
            m_Settings.ShowInspector = true;
            break;
        default:
            break;
    }
}

// ============================================================================
// 編集操作（すべて Undo 可能）
// ============================================================================
void SequencerWindow::BeginEdit()
{
    if (!m_EditOpen)
    {
        m_Backend.BeginEdit(m_Director);
        m_EditOpen = true;
        m_EditDirty = false;
        m_WidgetEdit = false;
    }
}

void SequencerWindow::CommitEdit()
{
    if (!m_EditOpen)
    {
        return;
    }
    m_Backend.CommitEdit(m_EditDirty);
    m_EditOpen = false;
    m_EditDirty = false;
    m_WidgetEdit = false;
    if (TimelineComponent* timeline = m_Backend.GetTimeline(m_Director))
    {
        m_LastChecksum = TimelineCurve::ComputeChecksum(*timeline);
    }
    m_LastHistoryRevision = m_Backend.GetHistoryRevision();
}

void SequencerWindow::MarkChanged(TimelineComponent& timeline)
{
    m_EditDirty = true;
    if (!m_Backend.IsSimulationRunning())
    {
        m_Backend.Evaluate(m_Director, timeline, false); // 変更をシーンへ即時反映
    }
    m_LastChecksum = TimelineCurve::ComputeChecksum(timeline);
}

void SequencerWindow::WidgetEdited(TimelineComponent& timeline, bool changed)
{
    if (!changed)
    {
        return;
    }
    if (!m_EditOpen)
    {
        m_Backend.BeginEditFromFrameState(); // このフレームの描画前の状態を Undo に使う
        m_EditOpen = true;
        m_WidgetEdit = true;
    }
    MarkChanged(timeline);
}

int SequencerWindow::AddTrack(TimelineComponent& timeline, EntityID entity, TimelineProperty property)
{
    TimelineTrackData track{};
    char name[128];
    std::snprintf(name, sizeof(name), "%s / %s", m_Backend.GetEntityName(entity), PropertyLabel(property));
    track.Name = name;
    track.Target = entity;
    track.Property = property;
    DirectX::XMFLOAT4 value{};
    track.HasDefaultValue = m_Backend.ReadProperty(entity, property, 0, value) && property != TimelineProperty::CameraCut;
    track.DefaultValue = value;
    // キーフレームで編集するトラックは、クリップ外でも最寄りのキー値を保持する
    track.HoldOutside = property != TimelineProperty::AnimationTime && property != TimelineProperty::CameraCut;
    timeline.Tracks.push_back(track);
    m_Collapsed.erase(HashRow(entity, static_cast<int>(RowKind::Object), -64, 0));
    return static_cast<int>(timeline.Tracks.size()) - 1;
}

void SequencerWindow::AddClipAtPlayhead(TimelineComponent& timeline, int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= static_cast<int>(timeline.Tracks.size()))
    {
        return;
    }
    auto& track = timeline.Tracks[trackIndex];
    TimelineClipData clip{};
    clip.StartTime = std::max(0.0f, timeline.CurrentTime);
    clip.Duration = std::clamp(timeline.Duration - clip.StartTime, 1.0f, 5.0f);
    if (track.Property == TimelineProperty::AnimationTime)
    {
        // クリップ開始 = アニメーション 0 秒、等速再生
        clip.Name = track.AnimationPaths.empty() ? "Animation" : "VMD";
        clip.Keys.push_back({0.0f, {0, 0, 0, 0}, TimelineInterpolation::Linear});
        clip.Keys.push_back({clip.Duration, {clip.Duration, 0, 0, 0}, TimelineInterpolation::Linear});
    }
    else if (track.Property == TimelineProperty::CameraCut)
    {
        clip.Name = "Cut";
    }
    else
    {
        clip.Name = "Clip";
        DirectX::XMFLOAT4 value{};
        if (m_Backend.ReadProperty(track.Target, track.Property, track.FogVolumeIndex, value))
        {
            clip.Keys.push_back({0.0f, value, TimelineInterpolation::Linear});
            clip.Keys.push_back({clip.Duration, value, TimelineInterpolation::Linear});
        }
    }
    track.Clips.push_back(clip);
    m_SelectedClips = {{ClipRef::Type::Track, trackIndex, static_cast<int>(track.Clips.size()) - 1}};
    m_SelectedKeys.clear();
}

void SequencerWindow::AddKeyAtRow(TimelineComponent& timeline, int rowIndex, float time, bool useSceneValue)
{
    CollectRowTracks(rowIndex, m_ScratchTracks);
    if (m_ScratchTracks.empty())
    {
        return;
    }
    BeginEdit();
    std::vector<KeyRef> added;
    for (int trackIndex : m_ScratchTracks)
    {
        auto& track = timeline.Tracks[trackIndex];
        if (track.Property == TimelineProperty::CameraCut)
        {
            continue;
        }
        DirectX::XMFLOAT4 value{};
        bool valid = false;
        if (useSceneValue)
        {
            valid = m_Backend.ReadProperty(track.Target, track.Property, track.FogVolumeIndex, value);
        }
        if (!valid)
        {
            // 既存の曲線上の値でキーを打つ（形を変えない）
            valid = TimelineCurve::EvaluateTrack(track, time, value) != TimelineCurve::TrackSample::None;
        }
        if (!valid)
        {
            valid = m_Backend.ReadProperty(track.Target, track.Property, track.FogVolumeIndex, value);
        }
        if (!valid)
        {
            continue;
        }
        if (track.Clips.empty() && track.Property != TimelineProperty::AnimationTime)
        {
            track.HoldOutside = true;
        }
        const auto result = TimelineCurve::AddKeyAt(track, time, value);
        added.push_back({trackIndex, result.first, result.second});
    }
    MarkChanged(timeline);
    CommitEdit();
    if (!added.empty())
    {
        m_SelectedKeys = added;
        std::sort(m_SelectedKeys.begin(), m_SelectedKeys.end());
        m_SelectedClips.clear();
    }
}

void SequencerWindow::DeleteSelection(TimelineComponent& timeline)
{
    if (!m_SelectedKeys.empty())
    {
        BeginEdit();
        TimelineCurve::DeleteKeys(timeline, m_SelectedKeys);
        m_SelectedKeys.clear();
        MarkChanged(timeline);
        CommitEdit();
        return;
    }
    bool removed = false;
    std::vector<ClipRef> clips = m_SelectedClips;
    std::sort(clips.begin(), clips.end(), [](const ClipRef& a, const ClipRef& b) {
        return a.Track != b.Track ? a.Track > b.Track : a.Clip > b.Clip;
    });
    for (const auto& ref : clips)
    {
        if (ref.Kind == ClipRef::Type::Track && ref.Track < static_cast<int>(timeline.Tracks.size()) &&
            ref.Clip < static_cast<int>(timeline.Tracks[ref.Track].Clips.size()))
        {
            if (!removed)
            {
                BeginEdit();
            }
            auto& list = timeline.Tracks[ref.Track].Clips;
            list.erase(list.begin() + ref.Clip);
            removed = true;
        }
    }
    if (removed)
    {
        m_SelectedClips.clear();
        MarkChanged(timeline);
        CommitEdit();
    }
    else if (!m_SelectedClips.empty())
    {
        m_Backend.Log("Sequencer: 映像・音声はインスペクターの「削除」から外してください。");
    }
}

void SequencerWindow::CopySelection(TimelineComponent& timeline)
{
    if (m_SelectedKeys.empty())
    {
        return;
    }
    m_Clipboard.clear();
    float first = FLT_MAX;
    for (const auto& ref : m_SelectedKeys)
    {
        first = std::min(first, TimelineCurve::KeyAbsoluteTime(timeline, ref));
    }
    for (const auto& ref : m_SelectedKeys)
    {
        ClipboardKey entry;
        entry.Track = ref.Track;
        entry.Property = timeline.Tracks[ref.Track].Property;
        entry.RelativeTime = TimelineCurve::KeyAbsoluteTime(timeline, ref) - first;
        entry.Key = timeline.Tracks[ref.Track].Clips[ref.Clip].Keys[ref.Key];
        m_Clipboard.push_back(entry);
    }
    std::snprintf(m_StatusText, sizeof(m_StatusText), "%zu キーをコピーしました", m_Clipboard.size());
}

void SequencerWindow::Paste(TimelineComponent& timeline)
{
    if (m_Clipboard.empty())
    {
        return;
    }
    // コピー元が 1 トラックだけなら、選択中の行（同じ次元のトラック）へ貼り付ける
    int target = -1;
    bool single = true;
    for (const auto& entry : m_Clipboard)
    {
        single &= entry.Track == m_Clipboard.front().Track;
    }
    if (single)
    {
        for (const auto& row : m_Rows)
        {
            if (row.Id == m_ActiveRow && row.Track >= 0 &&
                TimelineCurve::ValueDimension(timeline.Tracks[row.Track].Property) ==
                    TimelineCurve::ValueDimension(m_Clipboard.front().Property) &&
                timeline.Tracks[row.Track].Property != TimelineProperty::CameraCut)
            {
                target = row.Track;
            }
        }
    }
    BeginEdit();
    std::vector<KeyRef> pasted;
    for (const auto& entry : m_Clipboard)
    {
        const int trackIndex = target >= 0 ? target : entry.Track;
        if (trackIndex < 0 || trackIndex >= static_cast<int>(timeline.Tracks.size()) ||
            (target < 0 && timeline.Tracks[trackIndex].Property != entry.Property))
        {
            continue;
        }
        auto& track = timeline.Tracks[trackIndex];
        const auto result =
            TimelineCurve::AddKeyAt(track, timeline.CurrentTime + entry.RelativeTime, entry.Key.Value, entry.Key.Interpolation);
        auto& key = track.Clips[result.first].Keys[result.second];
        key.Interpolation = entry.Key.Interpolation;
        key.Bezier = entry.Key.Bezier;
        pasted.push_back({trackIndex, result.first, result.second});
    }
    MarkChanged(timeline);
    CommitEdit();
    // 同じクリップに挿入すると index がずれるため、時刻で選択し直す
    m_SelectedKeys.clear();
    for (const auto& entry : m_Clipboard)
    {
        const int trackIndex = target >= 0 ? target : entry.Track;
        if (trackIndex < 0 || trackIndex >= static_cast<int>(timeline.Tracks.size()))
        {
            continue;
        }
        const auto& track = timeline.Tracks[trackIndex];
        for (int clip = 0; clip < static_cast<int>(track.Clips.size()); ++clip)
        {
            const int key = TimelineCurve::FindKey(track.Clips[clip].Keys,
                                                   timeline.CurrentTime + entry.RelativeTime - track.Clips[clip].StartTime);
            if (key >= 0)
            {
                SelectKey({trackIndex, clip, key}, false);
            }
        }
    }
    (void)pasted;
    std::snprintf(m_StatusText, sizeof(m_StatusText), "%zu キーを貼り付けました", m_SelectedKeys.size());
}

void SequencerWindow::SetInterpolation(TimelineComponent& timeline, TimelineInterpolation interpolation)
{
    if (m_SelectedKeys.empty())
    {
        return;
    }
    BeginEdit();
    for (const auto& ref : m_SelectedKeys)
    {
        timeline.Tracks[ref.Track].Clips[ref.Clip].Keys[ref.Key].Interpolation = interpolation;
    }
    MarkChanged(timeline);
    CommitEdit();
}

void SequencerWindow::SplitClipAtPlayhead(TimelineComponent& timeline, const ClipRef& ref)
{
    if (ref.Kind != ClipRef::Type::Track || ref.Track < 0 || ref.Track >= static_cast<int>(timeline.Tracks.size()))
    {
        return;
    }
    auto& clips = timeline.Tracks[ref.Track].Clips;
    if (ref.Clip < 0 || ref.Clip >= static_cast<int>(clips.size()))
    {
        return;
    }
    const float time = timeline.CurrentTime;
    TimelineClipData& clip = clips[ref.Clip];
    const float end = clip.StartTime + clip.Duration;
    if (time <= clip.StartTime + TimelineCurve::kMinClipDuration || time >= end - TimelineCurve::kMinClipDuration)
    {
        return;
    }
    BeginEdit();
    TimelineClipData second = clip;
    clip.Duration = time - clip.StartTime;
    second.Name = clip.Name + " (2)";
    TimelineCurve::SetClipStartKeepContent(second, time); // 内容はそのまま、区間だけ分ける
    second.Duration = end - time;
    clips.insert(clips.begin() + ref.Clip + 1, second);
    MarkChanged(timeline);
    CommitEdit();
    m_SelectedClips = {{ClipRef::Type::Track, ref.Track, ref.Clip + 1}};
}

void SequencerWindow::FrameAll(TimelineComponent& timeline)
{
    float end = timeline.Duration;
    for (const auto& track : timeline.Tracks)
    {
        for (const auto& clip : track.Clips)
        {
            end = std::max(end, clip.StartTime + clip.Duration);
        }
    }
    const float width = std::max(50.0f, m_TimelineMax.x - m_TimelineMin.x);
    m_Settings.ViewStart = 0.0f;
    m_Settings.PixelsPerSecond = std::clamp(width / std::max(0.1f, end * 1.04f), 2.0f, 5000.0f);
}

// ============================================================================
// インスペクター（選択に応じて段階的に表示）
// ============================================================================
void SequencerWindow::DrawInspector(TimelineComponent& timeline)
{
    m_Backend.CaptureFrameState(m_Director);
    ImGui::PushItemWidth(-90.0f);
    if (!m_SelectedKeys.empty())
    {
        DrawInspectorKeys(timeline);
    }
    else if (!m_SelectedClips.empty())
    {
        DrawInspectorClip(timeline);
    }
    else
    {
        int track = -1;
        for (const auto& row : m_Rows)
        {
            if (row.Id == m_ActiveRow)
            {
                track = row.Track;
            }
        }
        if (track >= 0 && track < static_cast<int>(timeline.Tracks.size()))
        {
            DrawInspectorTrack(timeline, track);
        }
        else
        {
            DrawInspectorTimeline(timeline);
        }
    }
    ImGui::PopItemWidth();
}

void SequencerWindow::DrawInspectorKeys(TimelineComponent& timeline)
{
    const KeyRef first = m_SelectedKeys.front();
    if (!TimelineCurve::IsValid(timeline, first))
    {
        return;
    }
    auto& track = timeline.Tracks[first.Track];
    auto& clip = track.Clips[first.Clip];
    auto& key = clip.Keys[first.Key];
    const float scale = DisplayScale(track.Property);
    if (m_SelectedKeys.size() == 1)
    {
        ImGui::SeparatorText("キーフレーム");
        ImGui::TextDisabled("%s", track.Name.c_str());
        // 時刻（絶対時刻で編集、確定時に並べ替え・同時刻の規則を適用）
        float time = clip.StartTime + key.Time;
        const float before = time;
        ImGui::DragFloat("時刻 (秒)", &time, 0.01f, 0.0f, 36000.0f, "%.4f");
        const bool timeActive = ImGui::IsItemActive();
        if (time != before)
        {
            WidgetEdited(timeline, true);
            const std::vector<TimelineCurve::KeyOrigin> origins = {{first, before}};
            TimelineCurve::MoveKeys(timeline, origins, time - before, m_SelectedKeys);
            std::sort(m_SelectedKeys.begin(), m_SelectedKeys.end());
            (void)timeActive;
            return; // 参照が変わるため次のフレームで描画し直す
        }
        int frame = static_cast<int>(std::lround(before * GetFrameRate()));
        if (ImGui::InputInt("フレーム", &frame, 1, 10))
        {
            WidgetEdited(timeline, true);
            const std::vector<TimelineCurve::KeyOrigin> origins = {{first, before}};
            TimelineCurve::MoveKeys(timeline, origins, std::max(0.0f, frame / GetFrameRate()) - before, m_SelectedKeys);
            std::sort(m_SelectedKeys.begin(), m_SelectedKeys.end());
            return;
        }
        ImGui::SeparatorText("値");
        const int dimension = TimelineCurve::ValueDimension(track.Property);
        if (TimelineCurve::IsColorProperty(track.Property))
        {
            WidgetEdited(timeline, ImGui::ColorEdit3("色", &key.Value.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float));
        }
        else if (dimension == 3)
        {
            for (int channel = 0; channel < 3; ++channel)
            {
                float value = TimelineCurve::GetComponent(key.Value, channel) * scale;
                ImGui::PushID(channel);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ChannelColor(channel, false)));
                ImGui::TextUnformatted(ChannelName(channel, false));
                ImGui::PopStyleColor();
                ImGui::SameLine(24.0f);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat("##v", &value, scale != 1.0f ? 0.5f : 0.01f, 0.0f, 0.0f,
                                     scale != 1.0f ? "%.2f°" : "%.4f"))
                {
                    TimelineCurve::SetComponent(key.Value, channel, value / scale);
                    WidgetEdited(timeline, true);
                }
                ImGui::PopID();
            }
        }
        else if (dimension == 1)
        {
            float value = key.Value.x * scale;
            if (ImGui::DragFloat("値", &value, scale != 1.0f ? 0.25f : 0.01f, 0.0f, 0.0f, scale != 1.0f ? "%.2f°" : "%.4f"))
            {
                key.Value.x = value / scale;
                WidgetEdited(timeline, true);
            }
        }
        if (ImGui::Button("シーンの現在値を取り込む"))
        {
            DirectX::XMFLOAT4 value{};
            if (m_Backend.ReadProperty(track.Target, track.Property, track.FogVolumeIndex, value))
            {
                BeginEdit();
                key.Value = value;
                MarkChanged(timeline);
                CommitEdit();
            }
        }
        ImGui::SetItemTooltip("ギズモ等で動かした現在の値をこのキーに設定します");
    }
    else
    {
        ImGui::SeparatorText("キーフレーム");
        ImGui::Text("%zu 個のキーを選択中", m_SelectedKeys.size());
        float minimum = FLT_MAX;
        float maximum = -FLT_MAX;
        for (const auto& ref : m_SelectedKeys)
        {
            const float time = TimelineCurve::KeyAbsoluteTime(timeline, ref);
            minimum = std::min(minimum, time);
            maximum = std::max(maximum, time);
        }
        char a[32];
        char b[32];
        FormatTime(minimum, a, sizeof(a));
        FormatTime(maximum, b, sizeof(b));
        ImGui::TextDisabled("%s – %s", a, b);
        const float frame = 1.0f / GetFrameRate();
        auto shift = [&](float delta) {
            std::vector<TimelineCurve::KeyOrigin> origins;
            for (const auto& ref : m_SelectedKeys)
            {
                origins.push_back({ref, TimelineCurve::KeyAbsoluteTime(timeline, ref)});
            }
            BeginEdit();
            TimelineCurve::MoveKeys(timeline, origins, delta, m_SelectedKeys);
            std::sort(m_SelectedKeys.begin(), m_SelectedKeys.end());
            MarkChanged(timeline);
            CommitEdit();
        };
        if (ImGui::Button("-1 フレーム"))
        {
            shift(-frame);
        }
        ImGui::SameLine();
        if (ImGui::Button("+1 フレーム"))
        {
            shift(frame);
        }
        ImGui::SameLine();
        if (ImGui::Button("先頭を再生ヘッドへ"))
        {
            shift(timeline.CurrentTime - minimum);
        }
    }

    ImGui::SeparatorText("補間（次のキーまで）");
    int interpolation = InterpolationIndex(key.Interpolation);
    if (ImGui::Combo("補間", &interpolation, kInterpolationNames, IM_ARRAYSIZE(kInterpolationNames)))
    {
        SetInterpolation(timeline, static_cast<TimelineInterpolation>(interpolation));
    }
    if (key.Interpolation == TimelineInterpolation::CubicBezier)
    {
        // ベジェのプレビューと編集（選択キーすべてに適用）
        DirectX::XMFLOAT4 bezier = key.Bezier;
        bool changed = ImGui::DragFloat2("出 (x1, y1)", &bezier.x, 0.01f);
        changed |= ImGui::DragFloat2("入 (x2, y2)", &bezier.z, 0.01f);
        bezier.x = std::clamp(bezier.x, 0.0f, 1.0f);
        bezier.z = std::clamp(bezier.z, 0.0f, 1.0f);
        const char* presets[] = {"ease", "ease-in", "ease-out", "ease-in-out", "back"};
        const DirectX::XMFLOAT4 values[] = {{0.25f, 0.1f, 0.25f, 1.0f}, {0.42f, 0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.58f, 1.0f},
                                            {0.42f, 0.0f, 0.58f, 1.0f}, {0.34f, 1.56f, 0.64f, 1.0f}};
        for (int preset = 0; preset < 5; ++preset)
        {
            if (preset > 0)
            {
                ImGui::SameLine();
            }
            if (ImGui::SmallButton(presets[preset]))
            {
                bezier = values[preset];
                changed = true;
            }
        }
        if (changed)
        {
            WidgetEdited(timeline, true);
            for (const auto& ref : m_SelectedKeys)
            {
                timeline.Tracks[ref.Track].Clips[ref.Clip].Keys[ref.Key].Bezier = bezier;
            }
        }
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const float size = std::min(ImGui::GetContentRegionAvail().x, 150.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(position, {position.x + size, position.y + size}, IM_COL32(18, 19, 23, 255), 4.0f);
        const float pad = 14.0f;
        auto toScreen = [&](float x, float y) {
            return ImVec2(position.x + pad + x * (size - pad * 2.0f), position.y + size - pad - y * (size - pad * 2.0f) * 0.7f -
                                                                          (size - pad * 2.0f) * 0.15f);
        };
        m_ScratchPoints.clear();
        for (int step = 0; step <= 40; ++step)
        {
            const float x = step / 40.0f;
            m_ScratchPoints.push_back(toScreen(x, TimelineCurve::CubicBezierEase(x, bezier.x, bezier.y, bezier.z, bezier.w)));
        }
        drawList->AddLine(toScreen(0, 0), toScreen(bezier.x, bezier.y), IM_COL32(255, 255, 255, 90));
        drawList->AddLine(toScreen(1, 1), toScreen(bezier.z, bezier.w), IM_COL32(255, 255, 255, 90));
        drawList->AddPolyline(m_ScratchPoints.data(), static_cast<int>(m_ScratchPoints.size()), kAccent, 0, 2.0f);
        drawList->AddCircleFilled(toScreen(bezier.x, bezier.y), 3.5f, kText);
        drawList->AddCircleFilled(toScreen(bezier.z, bezier.w), 3.5f, kText);
        ImGui::Dummy({size, size});
        ImGui::TextDisabled("曲線エディターでハンドルをドラッグして編集できます");
    }
    if (track.Property == TimelineProperty::TransformRotation)
    {
        ImGui::SeparatorText("回転");
        ImGui::TextWrapped("回転は Euler 角（XYZ, ラジアン保存・度表示）を成分ごとに補間します。360° を超える値で複数回転も表現できます。");
        if (ImGui::Button("最短回転に補正"))
        {
            BeginEdit();
            if (TimelineCurve::UnwrapRotationKeys(track))
            {
                MarkChanged(timeline);
            }
            CommitEdit();
        }
        ImGui::SetItemTooltip("隣り合うキーの差が ±180° 以内になるよう 360° 単位で調整します（0°/360° 境界の逆回転を防止）");
    }
}

void SequencerWindow::DrawInspectorClip(TimelineComponent& timeline)
{
    const ClipRef ref = m_SelectedClips.front();
    char a[32];
    if (ref.Kind == ClipRef::Type::Track)
    {
        auto& track = timeline.Tracks[ref.Track];
        auto& clip = track.Clips[ref.Clip];
        ImGui::SeparatorText(track.Property == TimelineProperty::CameraCut ? "カメラカット" : "クリップ");
        ImGui::TextDisabled("%s", track.Name.c_str());
        char name[128];
        std::snprintf(name, sizeof(name), "%s", clip.Name.c_str());
        if (ImGui::InputText("名前", name, sizeof(name)))
        {
            clip.Name = name;
            WidgetEdited(timeline, true);
        }
        WidgetEdited(timeline, ImGui::Checkbox("有効", &clip.Enabled));
        float start = clip.StartTime;
        float end = clip.StartTime + clip.Duration;
        if (ImGui::DragFloat("開始 (秒)", &start, 0.01f, 0.0f, end - TimelineCurve::kMinClipDuration, "%.3f"))
        {
            TimelineCurve::SetClipStartKeepContent(clip, std::clamp(start, 0.0f, end - TimelineCurve::kMinClipDuration));
            clip.Duration = end - clip.StartTime;
            WidgetEdited(timeline, true);
        }
        if (ImGui::DragFloat("終了 (秒)", &end, 0.01f, clip.StartTime + TimelineCurve::kMinClipDuration, 36000.0f, "%.3f"))
        {
            clip.Duration = std::max(TimelineCurve::kMinClipDuration, end - clip.StartTime);
            WidgetEdited(timeline, true);
        }
        FormatTime(clip.Duration, a, sizeof(a));
        ImGui::Text("長さ  %s", a);
        if (track.Property != TimelineProperty::CameraCut)
        {
            WidgetEdited(timeline, ImGui::DragFloat("Blend In", &clip.BlendIn, 0.01f, 0.0f, clip.Duration));
            WidgetEdited(timeline, ImGui::DragFloat("Blend Out", &clip.BlendOut, 0.01f, 0.0f, clip.Duration));
            ImGui::Text("キー %zu 個", clip.Keys.size());
        }
        else
        {
            std::vector<EntityID> cameras;
            m_Backend.CollectCameras(cameras);
            if (ImGui::BeginCombo("カメラ", m_Backend.GetEntityName(track.Target)))
            {
                for (EntityID camera : cameras)
                {
                    ImGui::PushID(static_cast<int>(camera));
                    if (ImGui::Selectable(m_Backend.GetEntityName(camera), camera == track.Target))
                    {
                        BeginEdit();
                        track.Target = camera;
                        MarkChanged(timeline);
                        CommitEdit();
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::TextWrapped("区間内はこのカメラがメインのゲームカメラになります（Play / 一時停止中に適用）。"
                               "重なった区間は後から始まるクリップが優先です。");
        }
        if (track.Property == TimelineProperty::AnimationTime)
        {
            ImGui::TextWrapped("キーの値 = アニメーションの再生時刻（秒）。開始・終了のキーで再生速度とオフセットが決まります。");
        }
        if (ImGui::Button("再生ヘッドで分割"))
        {
            SplitClipAtPlayhead(timeline, ref);
        }
        ImGui::SameLine();
        if (ImGui::Button("削除"))
        {
            DeleteSelection(timeline);
        }
        if (track.Property == TimelineProperty::AnimationTime)
        {
            ImGui::SeparatorText("VMD レイヤー");
            WidgetEdited(timeline, m_Backend.DrawAnimationLayers(m_Director, ref.Track, track));
        }
    }
    else if (ref.Kind == ClipRef::Type::Video && ref.Track < static_cast<int>(timeline.Monitors.size()))
    {
        auto& monitor = timeline.Monitors[ref.Track];
        ImGui::SeparatorText("映像クリップ");
        ImGui::TextDisabled("%s", m_Backend.GetEntityName(monitor.Target));
        WidgetEdited(timeline, ImGui::Checkbox("有効（再生する）", &monitor.Enabled));
        WidgetEdited(timeline, ImGui::DragFloat("開始 (秒)", &monitor.StartTime, 0.01f, 0.0f, 36000.0f, "%.3f"));
        float end = monitor.EndTime;
        if (ImGui::DragFloat("終了 (秒)", &end, 0.01f, 0.0f, 36000.0f, end > monitor.StartTime ? "%.3f" : "制限なし"))
        {
            monitor.EndTime = end;
            WidgetEdited(timeline, true);
        }
        WidgetEdited(timeline, ImGui::DragFloat("オフセット (秒)", &monitor.Offset, 0.01f, 0.0f, 36000.0f, "%.3f"));
        WidgetEdited(timeline, ImGui::DragFloat("FPS", &monitor.FramesPerSecond, 0.1f, 0.01f, 240.0f));
        WidgetEdited(timeline, ImGui::Checkbox("連番ループ", &monitor.Loop));
        WidgetEdited(timeline, ImGui::DragFloat("発光強度", &monitor.EmissiveIntensity, 0.1f, 0.0f, 10000.0f));
        WidgetEdited(timeline, ImGui::DragFloat("周辺照明の強度", &monitor.LightIntensity, 0.1f, 0.0f, 100000.0f));
        size_t frame = 0;
        if (TimelineCurve::MonitorFrame(monitor, timeline.CurrentTime, frame))
        {
            ImGui::Text("現在のフレーム  %zu / %zu", frame + 1, monitor.Frames.size());
        }
        else
        {
            ImGui::TextDisabled("現在は更新しません（無効 / 終了後 / 連番なし）");
        }
        if (ImGui::TreeNode("連番画像・対象"))
        {
            WidgetEdited(timeline, m_Backend.DrawMonitorFrames(m_Director, ref.Track, monitor));
            ImGui::TreePop();
        }
        if (ImGui::Button("映像を削除"))
        {
            BeginEdit();
            timeline.Monitors.erase(timeline.Monitors.begin() + ref.Track);
            m_SelectedClips.clear();
            MarkChanged(timeline);
            CommitEdit();
        }
    }
    else if (ref.Kind == ClipRef::Type::Audio)
    {
        ImGui::SeparatorText("音声クリップ");
        char path[512];
        std::snprintf(path, sizeof(path), "%s", timeline.MusicPath.c_str());
        if (ImGui::InputText("曲 (WAV)", path, sizeof(path), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            BeginEdit();
            timeline.MusicPath = path;
            timeline.MusicDuration = 0.0f;
            timeline.SeekRequested = true;
            MarkChanged(timeline);
            CommitEdit();
        }
        ImGui::SetItemTooltip("PCM16 / float32 WAV。Enter で確定。アセットブラウザから WAV をタイムラインへドロップしても設定できます");
        if (ImGui::DragFloat("曲の開始位置 (秒)", &timeline.MusicOffset, 0.01f, 0.0f, 36000.0f, "%.3f"))
        {
            timeline.SeekRequested = true;
            WidgetEdited(timeline, true);
        }
        WidgetEdited(timeline, ImGui::SliderFloat("音量", &timeline.MusicVolume, 0.0f, 2.0f, "%.2f"));
        WidgetEdited(timeline, ImGui::DragFloat("BPM", &timeline.BeatsPerMinute, 0.1f, 1.0f, 1000.0f, "%.2f"));
        WidgetEdited(timeline, ImGui::DragFloat("先頭ビート (秒)", &timeline.BeatOffset, 0.01f, -60.0f, 600.0f, "%.3f"));
        WidgetEdited(timeline, ImGui::SliderInt("拍子（拍 / 小節）", &timeline.BeatsPerBar, 1, 12));
        if (timeline.MusicDuration > 0.0f)
        {
            FormatTime(timeline.MusicDuration, a, sizeof(a));
            ImGui::Text("曲の長さ  %s", a);
        }
        else
        {
            ImGui::TextDisabled("曲の長さは再生開始時に取得されます");
        }
        if (!timeline.PlaybackError.empty())
        {
            ImGui::TextColored({1.0f, 0.45f, 0.4f, 1.0f}, "%s", timeline.PlaybackError.c_str());
        }
        ImGui::TextWrapped("曲は LiveDirector の時計です（タイムライン 0 秒 = 曲の開始位置）。曲の区間はドラッグでスリップのみ可能です。");
        if (!timeline.MusicPath.empty() && ImGui::Button("音声を外す"))
        {
            BeginEdit();
            timeline.MusicPath.clear();
            timeline.MusicDuration = 0.0f;
            m_SelectedClips.clear();
            MarkChanged(timeline);
            CommitEdit();
        }
    }
}

void SequencerWindow::DrawInspectorTrack(TimelineComponent& timeline, int trackIndex)
{
    auto& track = timeline.Tracks[trackIndex];
    ImGui::SeparatorText("トラック");
    char name[128];
    std::snprintf(name, sizeof(name), "%s", track.Name.c_str());
    if (ImGui::InputText("名前", name, sizeof(name)))
    {
        track.Name = name;
        WidgetEdited(timeline, true);
    }
    WidgetEdited(timeline, ImGui::Checkbox("有効", &track.Enabled));
    ImGui::Text("対象  %s", m_Backend.GetEntityName(track.Target));
    ImGui::Text("プロパティ  %s", PropertyLabel(track.Property));
    if (track.Property != TimelineProperty::CameraCut)
    {
        WidgetEdited(timeline, ImGui::Checkbox("クリップ外は最寄りのキー値を保持", &track.HoldOutside));
        ImGui::SetItemTooltip("オフ：クリップ外は既定値（トラック作成時の値）に戻ります（従来の動作）");
    }
    size_t keys = 0;
    for (const auto& clip : track.Clips)
    {
        keys += clip.Keys.size();
    }
    ImGui::Text("クリップ %zu / キー %zu", track.Clips.size(), keys);
    if (track.Property != TimelineProperty::CameraCut && track.Property != TimelineProperty::AnimationTime)
    {
        if (ImGui::Button("再生ヘッドに現在値でキー (I)"))
        {
            const int row = FindRow(RowKind::Property, trackIndex);
            if (row >= 0)
            {
                AddKeyAtRow(timeline, row, timeline.CurrentTime, true);
            }
        }
    }
    else if (ImGui::Button("再生ヘッドにクリップを追加"))
    {
        BeginEdit();
        AddClipAtPlayhead(timeline, trackIndex);
        MarkChanged(timeline);
        CommitEdit();
    }
    if (track.Property == TimelineProperty::TransformRotation && ImGui::Button("最短回転に補正"))
    {
        BeginEdit();
        if (TimelineCurve::UnwrapRotationKeys(track))
        {
            MarkChanged(timeline);
        }
        CommitEdit();
    }
    if (track.Property >= TimelineProperty::LightIntensity && track.Property <= TimelineProperty::LightBeamDensity)
    {
        ImGui::TextDisabled(m_Backend.IsPhysicallyBasedLight(track.Target) ? "ライト：物理ベース（強度は物理単位）"
                                                                            : "ライト：演出用（非物理）");
        ImGui::TextWrapped("ステージライトグループの設定より、このトラックの値が後から適用されます。");
    }
    if (track.Property == TimelineProperty::AnimationTime)
    {
        ImGui::SeparatorText("VMD レイヤー");
        WidgetEdited(timeline, m_Backend.DrawAnimationLayers(m_Director, trackIndex, track));
        if (!track.AnimationError.empty())
        {
            ImGui::TextColored({1.0f, 0.45f, 0.4f, 1.0f}, "%s", track.AnimationError.c_str());
        }
    }
    ImGui::Separator();
    if (ImGui::Button("トラックを削除"))
    {
        BeginEdit();
        timeline.Tracks.erase(timeline.Tracks.begin() + trackIndex);
        m_SelectedKeys.clear();
        m_SelectedClips.clear();
        m_ActiveRow = 0;
        MarkChanged(timeline);
        CommitEdit();
    }
}

void SequencerWindow::DrawInspectorTimeline(TimelineComponent& timeline)
{
    ImGui::SeparatorText("タイムライン");
    ImGui::TextDisabled("%s", m_Backend.GetEntityName(m_Director));
    WidgetEdited(timeline, ImGui::DragFloat("長さ (秒)", &timeline.Duration, 0.1f, 0.01f, 36000.0f, "%.2f"));
    timeline.Duration = std::max(0.01f, timeline.Duration);
    WidgetEdited(timeline, ImGui::DragFloat("FPS (0=Project)", &timeline.FrameRate, 1.0f, 0.0f, 240.0f, "%.0f"));
    WidgetEdited(timeline, ImGui::DragFloat("BPM", &timeline.BeatsPerMinute, 0.1f, 1.0f, 1000.0f, "%.2f"));
    WidgetEdited(timeline, ImGui::DragFloat("先頭ビート (秒)", &timeline.BeatOffset, 0.01f, -60.0f, 600.0f, "%.3f"));
    WidgetEdited(timeline, ImGui::SliderInt("拍 / 小節", &timeline.BeatsPerBar, 1, 12));
    ImGui::BeginDisabled(!timeline.MusicPath.empty());
    WidgetEdited(timeline, ImGui::DragFloat("再生速度", &timeline.Speed, 0.01f, -100.0f, 100.0f));
    ImGui::EndDisabled();
    WidgetEdited(timeline, ImGui::Checkbox("Play On Awake", &timeline.PlayOnAwake));
    WidgetEdited(timeline, ImGui::Checkbox("Loop", &timeline.Loop));
    ImGui::SeparatorText("音声");
    if (timeline.MusicPath.empty())
    {
        ImGui::TextWrapped("WAV をタイムラインへドロップするか、パスを入力してください。");
    }
    char path[512];
    std::snprintf(path, sizeof(path), "%s", timeline.MusicPath.c_str());
    if (ImGui::InputText("曲 (WAV)", path, sizeof(path), ImGuiInputTextFlags_EnterReturnsTrue))
    {
        BeginEdit();
        timeline.MusicPath = path;
        timeline.MusicDuration = 0.0f;
        timeline.SeekRequested = true;
        MarkChanged(timeline);
        CommitEdit();
    }
    ImGui::SeparatorText("操作");
    ImGui::TextWrapped("ダブルクリック：キー追加 / Ctrl・Shift：複数・範囲選択 / ドラッグ：移動（Alt でスナップ無効）\n"
                       "Delete：削除 / Ctrl+C・V：コピー・貼り付け / Ctrl+Z・Y：元に戻す・やり直し\n"
                       "Ctrl+ホイール：ズーム / Shift+ホイール：横スクロール / 中ボタン：パン / F：全体表示\n"
                       "Space：再生 / ↑↓：前後のキー / ←→：1 フレーム / I：現在値でキー");
}

// ============================================================================
// 下部バー・コンテキストメニュー
// ============================================================================
void SequencerWindow::DrawBottomBar(TimelineComponent& timeline)
{
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.0f, 2.0f});
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("ズーム");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("##Zoom", &m_Settings.PixelsPerSecond, 2.0f, 2000.0f, "%.0f px/秒", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::TextDisabled("表示範囲");
    ImGui::SameLine();
    const float visible = (m_TimelineMax.x - m_TimelineMin.x) / m_Settings.PixelsPerSecond;
    float range[2] = {m_Settings.ViewStart, m_Settings.ViewStart + visible};
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::DragFloat2("##ViewRange", range, 0.05f, 0.0f, 36000.0f, "%.2fs"))
    {
        range[1] = std::max(range[0] + 0.05f, range[1]);
        m_Settings.ViewStart = std::max(0.0f, range[0]);
        m_Settings.PixelsPerSecond =
            std::clamp((m_TimelineMax.x - m_TimelineMin.x) / (range[1] - range[0]), 2.0f, 5000.0f);
    }
    ImGui::SameLine();
    if (ImGui::Button("全体表示 (F)"))
    {
        FrameAll(timeline);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("トラック高さ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::SliderFloat("##RowHeight", &m_Settings.RowHeight, 16.0f, 48.0f, "%.0f"))
    {
        m_RowHeights.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("曲線を表示", &m_Settings.InlineCurves);
    ImGui::SetItemTooltip("Dope Sheet の行内に値の変化を薄く表示");
    ImGui::SameLine();
    char status[200];
    if (!m_SelectedKeys.empty())
    {
        std::snprintf(status, sizeof(status), "キー %zu 個を選択", m_SelectedKeys.size());
    }
    else if (!m_SelectedClips.empty())
    {
        std::snprintf(status, sizeof(status), "クリップ %zu 個を選択", m_SelectedClips.size());
    }
    else
    {
        std::snprintf(status, sizeof(status), "%s", m_StatusText);
    }
    ImGui::TextDisabled("%s%s", status, m_Backend.IsSimulationRunning() ? "   ● Play 中（TimeLineSystem が評価）" : "");
    ImGui::PopStyleVar();
}

void SequencerWindow::DrawContextMenus(TimelineComponent& timeline)
{
    if (m_OpenTreeMenu)
    {
        ImGui::OpenPopup("##SeqTreeMenu");
        m_OpenTreeMenu = false;
    }
    if (m_OpenTimelineMenu)
    {
        ImGui::OpenPopup("##SeqTimelineMenu");
        m_OpenTimelineMenu = false;
    }
    if (ImGui::BeginPopup("##SeqTreeMenu"))
    {
        const int rowIndex = m_ContextHit.Row;
        if (rowIndex >= 0 && rowIndex < static_cast<int>(m_Rows.size()))
        {
            const Row row = m_Rows[rowIndex];
            ImGui::TextDisabled("%s", row.Label);
            ImGui::Separator();
            if (row.Kind == RowKind::Object && row.Entity != g_kINVALID_ENTITY)
            {
                if (ImGui::BeginMenu("トラックを追加"))
                {
                    DrawAddTrackMenu(timeline, row.Entity);
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("シーンで選択"))
                {
                    m_Backend.SetEditorSelection(row.Entity);
                }
                if (ImGui::MenuItem("このオブジェクトのトラックをすべて削除"))
                {
                    BeginEdit();
                    timeline.Tracks.erase(std::remove_if(timeline.Tracks.begin(), timeline.Tracks.end(),
                                                         [&](const TimelineTrackData& t) { return t.Target == row.Entity; }),
                                          timeline.Tracks.end());
                    m_SelectedKeys.clear();
                    m_SelectedClips.clear();
                    MarkChanged(timeline);
                    CommitEdit();
                }
            }
            if (row.Track >= 0 && row.Track < static_cast<int>(timeline.Tracks.size()))
            {
                auto& track = timeline.Tracks[row.Track];
                bool enabled = track.Enabled;
                if (ImGui::MenuItem("有効", nullptr, &enabled))
                {
                    BeginEdit();
                    track.Enabled = enabled;
                    MarkChanged(timeline);
                    CommitEdit();
                }
                if (track.Property != TimelineProperty::CameraCut && track.Property != TimelineProperty::AnimationTime &&
                    ImGui::MenuItem("再生ヘッドに現在値でキー", "I"))
                {
                    AddKeyAtRow(timeline, rowIndex, timeline.CurrentTime, true);
                }
                if ((track.Property == TimelineProperty::CameraCut || track.Property == TimelineProperty::AnimationTime ||
                     static_cast<Mode>(m_Settings.ModeValue) == Mode::Clip) &&
                    ImGui::MenuItem("再生ヘッドにクリップを追加"))
                {
                    BeginEdit();
                    AddClipAtPlayhead(timeline, row.Track);
                    MarkChanged(timeline);
                    CommitEdit();
                }
                if (track.Property == TimelineProperty::TransformRotation && ImGui::MenuItem("最短回転に補正"))
                {
                    BeginEdit();
                    if (TimelineCurve::UnwrapRotationKeys(track))
                    {
                        MarkChanged(timeline);
                    }
                    CommitEdit();
                }
                if (ImGui::MenuItem("トラックを削除"))
                {
                    BeginEdit();
                    timeline.Tracks.erase(timeline.Tracks.begin() + row.Track);
                    m_SelectedKeys.clear();
                    m_SelectedClips.clear();
                    MarkChanged(timeline);
                    CommitEdit();
                }
            }
            if (row.HasChildren)
            {
                if (ImGui::MenuItem(row.Open ? "折りたたむ" : "展開する"))
                {
                    SetRowOpen(row.Id, !row.Open);
                }
            }
            if (ImGui::MenuItem("すべて展開"))
            {
                m_Collapsed.clear();
            }
            if (ImGui::MenuItem("チャンネル（X/Y/Z）をすべて折りたたむ"))
            {
                for (const auto& other : m_Rows)
                {
                    if (other.Kind == RowKind::Property && other.HasChildren)
                    {
                        m_Collapsed.insert(other.Id);
                    }
                }
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##SeqTimelineMenu"))
    {
        const HitInfo& hit = m_ContextHit;
        if (!m_SelectedKeys.empty() && (hit.Type == Hit::Key || hit.Type == Hit::SummaryKey || hit.Type == Hit::CurveKey))
        {
            ImGui::TextDisabled("%zu キー", m_SelectedKeys.size());
            ImGui::Separator();
            if (ImGui::BeginMenu("補間"))
            {
                for (int index = 0; index < 6; ++index)
                {
                    if (ImGui::MenuItem(kInterpolationNames[index]))
                    {
                        SetInterpolation(timeline, static_cast<TimelineInterpolation>(index));
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("コピー", "Ctrl+C"))
            {
                CopySelection(timeline);
            }
            if (ImGui::MenuItem("貼り付け（再生ヘッド）", "Ctrl+V", false, !m_Clipboard.empty()))
            {
                Paste(timeline);
            }
            if (ImGui::MenuItem("削除", "Delete"))
            {
                DeleteSelection(timeline);
            }
        }
        else if (!m_SelectedClips.empty() &&
                 (hit.Type == Hit::Clip || hit.Type == Hit::ClipLeft || hit.Type == Hit::ClipRight))
        {
            const ClipRef ref = m_SelectedClips.front();
            if (ref.Kind == ClipRef::Type::Track)
            {
                auto& clip = timeline.Tracks[ref.Track].Clips[ref.Clip];
                bool enabled = clip.Enabled;
                if (ImGui::MenuItem("有効", nullptr, &enabled))
                {
                    BeginEdit();
                    clip.Enabled = enabled;
                    MarkChanged(timeline);
                    CommitEdit();
                }
                if (ImGui::MenuItem("再生ヘッドで分割"))
                {
                    SplitClipAtPlayhead(timeline, ref);
                }
                if (ImGui::MenuItem("クリップを削除", "Delete"))
                {
                    DeleteSelection(timeline);
                }
            }
            else
            {
                ImGui::TextDisabled("インスペクターで編集");
            }
        }
        else
        {
            if (hit.Row >= 0 && hit.Row < static_cast<int>(m_Rows.size()))
            {
                const Row& row = m_Rows[hit.Row];
                if (!RowShowsClips(timeline, row) && ImGui::MenuItem("ここにキーを追加"))
                {
                    AddKeyAtRow(timeline, hit.Row, SnapTime(hit.Time, false), false);
                }
                if (RowShowsClips(timeline, row) && row.Track >= 0 && ImGui::MenuItem("ここにクリップを追加"))
                {
                    const float saved = timeline.CurrentTime;
                    timeline.CurrentTime = SnapTime(hit.Time, false);
                    BeginEdit();
                    AddClipAtPlayhead(timeline, row.Track);
                    timeline.CurrentTime = saved;
                    MarkChanged(timeline);
                    CommitEdit();
                }
            }
            if (ImGui::MenuItem("貼り付け（再生ヘッド）", "Ctrl+V", false, !m_Clipboard.empty()))
            {
                Paste(timeline);
            }
            if (ImGui::MenuItem("再生ヘッドをここへ"))
            {
                SetTime(timeline, hit.Time, true);
            }
        }
        ImGui::EndPopup();
    }
}
