#pragma once
#include "../assets/materialgraph.h"
#include "../../External/imgui-node-editor/imgui_node_editor.h"
#include <imgui.h>
#include <algorithm>

// マテリアルとNiagaraでノード・ピンの視覚表現を共有する。
namespace GraphVisuals
{
    struct CategoryInfo
    {
        const char* Name;
        ImU32 Color;
    };

    inline CategoryInfo Category(MaterialNodeKind kind)
    {
        switch (kind)
        {
            case MaterialNodeKind::Output:
                return {"Output", IM_COL32(72, 72, 78, 255)};
            case MaterialNodeKind::Constant:
            case MaterialNodeKind::Constant2Vector:
            case MaterialNodeKind::Constant3Vector:
            case MaterialNodeKind::Constant4Vector:
            case MaterialNodeKind::Time:
                return {"Constants", IM_COL32(56, 118, 70, 255)};
            case MaterialNodeKind::ScalarParameter:
            case MaterialNodeKind::Vector2Parameter:
            case MaterialNodeKind::VectorParameter:
            case MaterialNodeKind::ColorParameter:
                return {"Parameters", IM_COL32(42, 140, 96, 255)};
            case MaterialNodeKind::TextureSample:
            case MaterialNodeKind::TriplanarTexture:
            case MaterialNodeKind::ConvertToTexture:
            case MaterialNodeKind::ConvertToHeight:
            case MaterialNodeKind::ConvertToNormal:
                return {"Texture", IM_COL32(48, 92, 168, 255)};
            case MaterialNodeKind::ConvertToNormalHeight:
                return {"Legacy", IM_COL32(48, 92, 168, 255)}; // 追加メニューには出さない
            case MaterialNodeKind::UV:
            case MaterialNodeKind::TextureCoordinate:
            case MaterialNodeKind::Panner:
            case MaterialNodeKind::Rotator:
            case MaterialNodeKind::WorldPosition:
            case MaterialNodeKind::WorldNormal:
            case MaterialNodeKind::CameraVector:
            case MaterialNodeKind::ReflectionVector:
            case MaterialNodeKind::SolidCoordinate:
            case MaterialNodeKind::LocalPosition:
            case MaterialNodeKind::VertexColor:
            case MaterialNodeKind::CameraDistance:
            case MaterialNodeKind::CameraPosition:
            case MaterialNodeKind::PixelDepth:
            case MaterialNodeKind::ScreenPosition:
                return {"Coordinates", IM_COL32(150, 62, 62, 255)};
            case MaterialNodeKind::ObjectPosition:
            case MaterialNodeKind::ObjectScale:
            case MaterialNodeKind::ObjectOrientation:
            case MaterialNodeKind::ObjectRadius:
            case MaterialNodeKind::ObjectBounds:
            case MaterialNodeKind::BoundingBoxUVW:
            case MaterialNodeKind::PerInstanceRandom:
            case MaterialNodeKind::TransformVector:
            case MaterialNodeKind::TransformPosition:
                return {"Object", IM_COL32(170, 110, 50, 255)};
            case MaterialNodeKind::Normalize:
            case MaterialNodeKind::Dot:
            case MaterialNodeKind::CrossProduct:
            case MaterialNodeKind::Distance:
            case MaterialNodeKind::Length:
            case MaterialNodeKind::AppendVector:
            case MaterialNodeKind::ComponentMask:
            case MaterialNodeKind::BreakVector:
                return {"Vector", IM_COL32(108, 78, 146, 255)};
            case MaterialNodeKind::Fresnel:
            case MaterialNodeKind::Desaturation:
            case MaterialNodeKind::If:
            case MaterialNodeKind::SphereMask:
            case MaterialNodeKind::CheapContrast:
            case MaterialNodeKind::Noise:
            case MaterialNodeKind::Reroute:
            case MaterialNodeKind::StaticSwitch:
            case MaterialNodeKind::SlopeMask:
            case MaterialNodeKind::HeightLerp:
            case MaterialNodeKind::DitherOpacity:
            case MaterialNodeKind::DistanceFade:
            case MaterialNodeKind::HeightGradient:
                return {"Utility", IM_COL32(140, 104, 40, 255)};
            case MaterialNodeKind::Pi:
                return {"Constants", IM_COL32(56, 118, 70, 255)};
            case MaterialNodeKind::HueShift:
            case MaterialNodeKind::Blend:
            case MaterialNodeKind::ColorAdjust:
            case MaterialNodeKind::BlackBody:
                return {"Color", IM_COL32(170, 70, 120, 255)};
            case MaterialNodeKind::LinearGradient:
            case MaterialNodeKind::RadialGradient:
            case MaterialNodeKind::Voronoi:
            case MaterialNodeKind::ProceduralNoise:
            case MaterialNodeKind::FractalNoise:
            case MaterialNodeKind::NoiseCombine:
            case MaterialNodeKind::NoiseLayers:
            case MaterialNodeKind::SolidTexture:
            case MaterialNodeKind::SolidPattern:
            case MaterialNodeKind::ThreeColorRamp:
            case MaterialNodeKind::ConvertToSolid:
                return {"Procedural", IM_COL32(60, 128, 140, 255)};
            case MaterialNodeKind::RenderSoftBand:
            case MaterialNodeKind::RenderRadialPulse:
            case MaterialNodeKind::RenderFlowUV:
                return {"Render / Original", IM_COL32(42, 128, 126, 255)};
            case MaterialNodeKind::NormalFromHeight:
            case MaterialNodeKind::BlendAngleCorrectedNormals:
            case MaterialNodeKind::FlattenNormal:
            case MaterialNodeKind::BumpOffset:
                return {"Normal / Height", IM_COL32(86, 96, 170, 255)};
            case MaterialNodeKind::ReflectVector:
            case MaterialNodeKind::RotateAboutAxis:
                return {"Vector", IM_COL32(108, 78, 146, 255)};
            case MaterialNodeKind::Group:
                return {"Comment", IM_COL32(80, 100, 130, 255)};
            default:
                return {"Math", IM_COL32(64, 70, 80, 255)};
        }
    }

    inline ImU32 TypeColor(MaterialGraphType type, float alpha = 1.0f)
    {
        ImVec4 color;
        switch (type)
        {
            case MaterialGraphType::Scalar:
                color = ImVec4(0.55f, 0.85f, 0.55f, 1.0f); // 緑
                break;
            case MaterialGraphType::Vector2:
                color = ImVec4(0.95f, 0.60f, 0.30f, 1.0f); // 橙
                break;
            case MaterialGraphType::Vector3:
                color = ImVec4(0.95f, 0.88f, 0.35f, 1.0f); // 黄
                break;
            case MaterialGraphType::Vector4:
                color = ImVec4(0.95f, 0.45f, 0.75f, 1.0f); // 桃
                break;
            default:
                color = ImVec4(0.78f, 0.78f, 0.80f, 1.0f); // 型未定
                break;
        }
        color.w = alpha;
        return ImGui::GetColorU32(color);
    }

    inline const char* TypeName(MaterialGraphType type)
    {
        switch (type)
        {
            case MaterialGraphType::Scalar:
                return "float";
            case MaterialGraphType::Vector2:
                return "float2";
            case MaterialGraphType::Vector3:
                return "float3";
            case MaterialGraphType::Vector4:
                return "float4";
            default:
                return "auto";
        }
    }

    inline const char* ComponentName(size_t component)
    {
        static const char* names[] = {"X (R)", "Y (G)", "Z (B)", "W (A)"};
        return names[(std::min)(component, size_t{3})];
    }

    inline ImVec4 ComponentColor(size_t component)
    {
        static const ImVec4 colors[] = {{0.96f, 0.32f, 0.28f, 1.0f},
                                        {0.35f, 0.85f, 0.38f, 1.0f},
                                        {0.35f, 0.60f, 1.0f, 1.0f},
                                        {0.85f, 0.85f, 0.88f, 1.0f}};
        return colors[(std::min)(component, size_t{3})];
    }

    inline void PinIcon(ImU32 color, bool connected)
    {
        const float size = ImGui::GetTextLineHeight() * 0.72f;
        ImGui::Dummy(ImVec2(size, ImGui::GetTextLineHeight()));
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 center(min.x + size * 0.5f, min.y + ImGui::GetTextLineHeight() * 0.5f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (connected)
        {
            draw->AddCircleFilled(center, size * 0.5f, color, 16);
        }
        else
        {
            draw->AddCircle(center, size * 0.5f - 0.75f, color, 16, 1.5f);
        }
    }

    inline void PushCanvasStyle()
    {
        namespace Nodes = ax::NodeEditor;
        Nodes::PushStyleColor(Nodes::StyleColor_Bg, {0.09f, 0.09f, 0.10f, 1.0f});
        Nodes::PushStyleColor(Nodes::StyleColor_Grid, {1, 1, 1, 0.05f});
        Nodes::PushStyleColor(Nodes::StyleColor_NodeBg, {0.11f, 0.11f, 0.12f, 0.96f});
        Nodes::PushStyleColor(Nodes::StyleColor_NodeBorder, {0, 0, 0, 0.9f});
        Nodes::PushStyleColor(Nodes::StyleColor_HovNodeBorder, {0.85f, 0.85f, 0.85f, 0.8f});
        Nodes::PushStyleColor(Nodes::StyleColor_SelNodeBorder, {1, 0.66f, 0.15f, 1});
    }

    inline void DrawHeader(ax::NodeEditor::NodeId id, float bottom, ImU32 color)
    {
        namespace Nodes = ax::NodeEditor;
        const auto position = Nodes::GetNodePosition(id);
        const auto size = Nodes::GetNodeSize(id);
        if (size.x <= 0.0f)
        {
            return;
        }
        const auto& style = Nodes::GetStyle();
        const float inset = style.NodeBorderWidth * 0.5f;
        const ImVec2 min(position.x + inset, position.y + inset);
        const ImVec2 max(position.x + size.x - inset, bottom + 3.0f);
        auto* draw = Nodes::GetNodeBackgroundDrawList(id);
        draw->AddRectFilled(min, max, color, style.NodeRounding, ImDrawFlags_RoundCornersTop);
        draw->AddRectFilledMultiColor({min.x, (min.y + max.y) * 0.5f}, max, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                      IM_COL32(0, 0, 0, 70), IM_COL32(0, 0, 0, 70));
        draw->AddLine({min.x, max.y}, max, IM_COL32(0, 0, 0, 140));
    }
}
