#pragma once
#include <imgui.h>
#include <cmath>

namespace MaterialOpticsUI
{
    // プリセットの適用や、無関係なマテリアルプロパティの上書きは行わない。
    inline bool Draw(float& ior, float& transmission, float& reflectionRoughness, float& transmissionRoughness,
                     float& thickness, float* absorption)
    {
        if (!ImGui::CollapsingHeader("光学 / 反射・屈折・吸収", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return false;
        }
        bool changed = false;
        changed |= ImGui::DragFloat("屈折率 (IOR)", &ior, 0.005f, 1.0f, 10.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("屈折方向とFresnel反射率を同時に変更します。1.0で空気と同じ屈折率。");
        const float ratio = (ior - 1.0f) / (ior + 1.0f);
        ImGui::Text("垂直入射反射率 F0: %.2f %%", ratio * ratio * 100.0f);
        changed |= ImGui::SliderFloat("光学透過率", &transmission, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("0 = 不透明の反射・拡散、1 = 透過。アルファとは別の物理パラメータです。");
        changed |=
            ImGui::SliderFloat("反射ラフネス", &reflectionRoughness, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        changed |= ImGui::SliderFloat("透過ラフネス", &transmissionRoughness, 0.0f, 1.0f, "%.3f",
                                      ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("透過背景のぼけ。反射のRoughnessと独立して編集できます。");
        changed |= ImGui::DragFloat("基準厚み / world unit", &thickness, 0.001f, 0.0f, 65504.0f, "%.4f",
                                    ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip(
            "DXRでは実形状から光路長を求めます。この値はBase Colorの透過色の基準距離と、画面空間近似の厚みです。");
        changed |= ImGui::DragFloat3("吸収係数 RGB / world unit", absorption, 0.01f, 0.0f, 65504.0f, "%.4f",
                                     ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Beer-Lambert吸収。赤の係数を上げると赤い光が減衰します。発光色ではありません。");
        // 保存する吸収係数は同じまま、色として編集できるようにする。
        const float referenceDistance = thickness > 0.0001f ? thickness : 0.0001f;
        float attenuation[3];
        for (int i = 0; i < 3; ++i)
        {
            attenuation[i] = std::exp(-absorption[i] * referenceDistance);
        }
        if (ImGui::ColorEdit3("吸収後の透過色（基準厚み）", attenuation, ImGuiColorEditFlags_Float))
        {
            for (int i = 0; i < 3; ++i)
            {
                const float channel =
                    attenuation[i] < 0.0001f ? 0.0001f : (attenuation[i] > 1.0f ? 1.0f : attenuation[i]);
                const float coefficient = -std::log(channel) / referenceDistance;
                absorption[i] = coefficient < 65504.0f ? coefficient : 65504.0f;
            }
            changed = true;
        }
        ImGui::SetItemTooltip("吸収係数と同じ値を色で編集します。白は吸収なし。Base Colorの透過色とは乗算されます。");
        ImGui::TextWrapped(
            "ガラスでもカバレッジは1のままです。Base Colorで透過色、Roughnessで反射のぼけを調整します。");
        return changed;
    }
} // 名前空間 MaterialOpticsUI の終わり。
