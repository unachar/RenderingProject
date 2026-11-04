#include "pch.h"
#include "materialgraphexteditor.h"
#include "materialgraphcustomhlsl.h"
#include "materialgraphevaluator.h"
#include "materialgraphlower.h"
#include "materialgraphmath.h"
#include "materialgraphnodes.h"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace MaterialGraphExtEditor
{
    namespace
    {
        using namespace MaterialGraphNodes;
        using EditKind = MaterialNodeKind;

        // ---------------------------------------------------------------- 共通
        // 値の変更を 1 回の Undo にまとめる共通処理
        struct Edit
        {
            EditHost& Host;
            MaterialGraphNode& Node;
            void Begin()
            {
                Host.BeginEdit(Node.Id);
            }
            void Done()
            {
                Host.MarkChanged();
            }
            void End()
            {
                Host.EndEdit(Node.Id);
            }
        };

        ImU32 DisplayColor(const float* rgb)
        {
            const auto encode = [](float v) {
                return static_cast<int>(MaterialGraphEval::LinearToSrgb(std::isfinite(v) ? v : 0.0f) * 255.0f + 0.5f);
            };
            return IM_COL32(encode(rgb[0]), encode(rgb[1]), encode(rgb[2]), 255);
        }

        const char* SamplerName(int i)
        {
            static const char* names[] = {"Sampler: Color", "Sampler: Normal", "Sampler: Linear"};
            return names[std::clamp(i, 0, 2)];
        }
        const char* InterpName(int i)
        {
            static const char* names[] = {"補間: Linear", "補間: Smooth", "補間: Constant"};
            return names[std::clamp(i, 0, 2)];
        }
        const char* LoopName(int i)
        {
            static const char* names[] = {"ループ: なし", "ループ: Repeat", "ループ: PingPong"};
            return names[std::clamp(i, 0, 2)];
        }
        const char* ToonClassName(int i)
        {
            static const char* names[] = {"素材: 衣装", "素材: 髪", "素材: 肌"};
            return names[std::clamp(i, 0, 2)];
        }
        const char* CurvatureModeName(int i)
        {
            static const char* names[] = {"凸（エッジ）", "凹（キャビティ）", "両方"};
            return names[std::clamp(i, 0, 2)];
        }

        // 数値で切り替えるスライダー（ポップアップを使わない：ノードの中でも安全）
        bool ValueSelector(Edit& e, size_t index, int count, const char* (*name)(int), float width, const char* id)
        {
            int v = std::clamp(static_cast<int>(e.Node.Value[index]), 0, count - 1);
            ImGui::SetNextItemWidth(width);
            if (ImGui::SliderInt(id, &v, 0, count - 1, name(v)))
            {
                e.Begin();
                e.Node.Value[index] = static_cast<float>(v);
                e.Done();
                return true;
            }
            if (ImGui::IsItemDeactivated())
            {
                e.End();
            }
            return false;
        }

        bool ValueToggle(Edit& e, size_t index, const char* label, const char* tooltip = nullptr)
        {
            bool on = e.Node.Value[index] > 0.5f;
            if (ImGui::Checkbox(label, &on))
            {
                e.Begin();
                e.Node.Value[index] = on ? 1.0f : 0.0f;
                e.Done();
                e.End();
                return true;
            }
            if (tooltip && ImGui::IsItemHovered())
            {
                e.Host.Tooltip(tooltip);
            }
            return false;
        }

        bool ValueDrag(Edit& e, size_t index, const char* id, float speed, const char* format, float width, float minimum = 0.0f, float maximum = 0.0f)
        {
            float v = e.Node.Value[index];
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat(id, &v, speed, minimum, maximum, format) && std::isfinite(v))
            {
                e.Begin();
                e.Node.Value[index] = v;
                e.Done();
            }
            if (ImGui::IsItemDeactivated())
            {
                e.End();
            }
            return false;
        }

        // テクスチャのドロップ先（Content Browser からのドラッグ）
        void TextureDrop(Edit& e, float width, const char* tooltip, const char* emptyLabel = "Textureをドロップ")
        {
            std::string label = e.Node.Texture != 0 ? e.Host.AssetFileName(e.Node.Texture) : std::string();
            if (label.empty())
            {
                label = emptyLabel;
            }
            label += "##TextureDrop";
            ImGui::Button(label.c_str(), ImVec2(width, 0));
            if (tooltip && ImGui::IsItemHovered())
            {
                e.Host.Tooltip(tooltip);
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetPayloadType))
                {
                    if (payload->DataSize == sizeof(AssetDragPayload))
                    {
                        const auto& asset = *static_cast<const AssetDragPayload*>(payload->Data);
                        if (asset.Type == AssetType::Texture && e.Host.IsAssetType(asset.Handle, AssetType::Texture))
                        {
                            e.Begin();
                            e.Node.Texture = asset.Handle;
                            e.Done();
                            e.End();
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (e.Node.Texture != 0)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("×##ClearTexture"))
                {
                    e.Begin();
                    e.Node.Texture = 0;
                    e.Done();
                    e.End();
                }
            }
        }

        // 文字列の編集（Text / PinNames）
        bool TextField(Edit& e, std::string& value, const char* id, float width, size_t maxLength, bool multiline = false, float height = 0.0f)
        {
            char buffer[4096];
            const size_t length = (std::min)(value.size(), (std::min)(maxLength, sizeof(buffer) - 1));
            std::memcpy(buffer, value.data(), length);
            buffer[length] = '\0';
            ImGui::SetNextItemWidth(width);
            bool changed = false;
            if (multiline)
            {
                changed = ImGui::InputTextMultiline(id, buffer, (std::min)(maxLength + 1, sizeof(buffer)), ImVec2(width, height));
            }
            else
            {
                changed = ImGui::InputText(id, buffer, (std::min)(maxLength + 1, sizeof(buffer)));
            }
            if (changed)
            {
                e.Begin();
                value = buffer;
                e.Done();
            }
            if (ImGui::IsItemDeactivated())
            {
                e.End();
            }
            return changed;
        }

        // ---------------------------------------------------------------- ランプ（Multi Color Ramp）
        void DrawRampEditor(Edit& e, float width)
        {
            auto keys = RampKeys(e.Node);
            Interp mode = RampMode(e.Node);
            // グラデーションのプレビュー
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float height = 18.0f;
            ImGui::InvisibleButton("##RampBar", ImVec2(width, height + 10.0f));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            constexpr int kSegments = 48;
            for (int s = 0; s < kSegments; ++s)
            {
                float a[3], b[3];
                EvalRamp(keys, mode, static_cast<float>(s) / kSegments, a);
                EvalRamp(keys, mode, static_cast<float>(s + 1) / kSegments, b);
                const float x0 = origin.x + width * static_cast<float>(s) / kSegments;
                const float x1 = origin.x + width * static_cast<float>(s + 1) / kSegments;
                draw->AddRectFilledMultiColor(ImVec2(x0, origin.y), ImVec2(x1, origin.y + height), DisplayColor(a), DisplayColor(b), DisplayColor(b), DisplayColor(a));
            }
            for (const auto& key : keys)
            {
                const float x = origin.x + width * std::clamp(key.X, 0.0f, 1.0f);
                draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height + 6.0f), IM_COL32(255, 255, 255, 220), 1.5f);
                draw->AddCircleFilled(ImVec2(x, origin.y + height + 6.0f), 3.5f, DisplayColor(key.Y));
            }
            // 補間 / キー数
            Edit modeEdit = e;
            int interp = static_cast<int>(mode);
            ImGui::SetNextItemWidth(width * 0.6f);
            if (ImGui::SliderInt("##RampMode", &interp, 0, 2, InterpName(interp)))
            {
                e.Begin();
                SetRampKeys(e.Node, keys, static_cast<Interp>(interp));
                e.Done();
            }
            if (ImGui::IsItemDeactivated())
            {
                e.End();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(static_cast<int>(keys.size()) >= kMaxRampKeys);
            if (ImGui::SmallButton("+##AddKey"))
            {
                e.Begin();
                // 最も広い隙間の中央に、両隣の色を混ぜたキーを足す
                size_t gap = 0;
                float widest = -1.0f;
                for (size_t i = 0; i + 1 < keys.size(); ++i)
                {
                    const float w = keys[i + 1].X - keys[i].X;
                    if (w > widest)
                    {
                        widest = w;
                        gap = i;
                    }
                }
                Key added;
                if (keys.size() >= 2)
                {
                    added.X = (keys[gap].X + keys[gap + 1].X) * 0.5f;
                    for (int c = 0; c < 3; ++c)
                    {
                        added.Y[c] = (keys[gap].Y[c] + keys[gap + 1].Y[c]) * 0.5f;
                    }
                    keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(gap) + 1, added);
                }
                else
                {
                    added.X = 1.0f;
                    keys.push_back(added);
                }
                SetRampKeys(e.Node, keys, mode);
                e.Done();
                e.End();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Text("%d / %d", static_cast<int>(keys.size()), kMaxRampKeys);
            // キーの一覧（位置 / 色）
            int removeIndex = -1;
            for (size_t i = 0; i < keys.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                float position = keys[i].X;
                float color[3] = {keys[i].Y[0], keys[i].Y[1], keys[i].Y[2]};
                ImGui::ColorButton("##Swatch", ImVec4(std::clamp(MaterialGraphEval::LinearToSrgb(color[0]), 0.0f, 1.0f),
                                                      std::clamp(MaterialGraphEval::LinearToSrgb(color[1]), 0.0f, 1.0f),
                                                      std::clamp(MaterialGraphEval::LinearToSrgb(color[2]), 0.0f, 1.0f), 1.0f),
                                   ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
                ImGui::SameLine();
                bool changed = false;
                ImGui::SetNextItemWidth(width * 0.22f);
                changed |= ImGui::DragFloat("##Pos", &position, 0.005f, 0.0f, 1.0f, "%.2f");
                bool endEdit = ImGui::IsItemDeactivated();
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::SetNextItemWidth(width * 0.56f);
                changed |= ImGui::DragFloat3("##Rgb", color, 0.005f, 0.0f, 16.0f, "%.2f");
                endEdit |= ImGui::IsItemDeactivated();
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::BeginDisabled(static_cast<int>(keys.size()) <= kMinRampKeys);
                if (ImGui::SmallButton("×"))
                {
                    removeIndex = static_cast<int>(i);
                }
                ImGui::EndDisabled();
                if (changed && std::isfinite(position) && std::isfinite(color[0]) && std::isfinite(color[1]) && std::isfinite(color[2]))
                {
                    e.Begin();
                    keys[i].X = std::clamp(position, 0.0f, 1.0f);
                    for (int c = 0; c < 3; ++c)
                    {
                        keys[i].Y[c] = color[c];
                    }
                    SetRampKeys(e.Node, keys, mode);
                    e.Done();
                }
                if (endEdit)
                {
                    e.End();
                }
                ImGui::PopID();
            }
            if (removeIndex >= 0)
            {
                e.Begin();
                keys.erase(keys.begin() + removeIndex);
                SetRampKeys(e.Node, keys, mode);
                e.Done();
                e.End();
            }
            (void)modeEdit;
        }

        // ---------------------------------------------------------------- カーブ（RGB Curves / Curve Atlas）
        // keys は x, y0 のみを使う。xRange = x の最大。戻り値 = 変更があった
        bool DrawCurveWidget(Edit& e, std::vector<Key>& keys, Interp mode, float width, float size, const char* id, int maxPoints, ImU32 lineColor)
        {
            static const void* dragOwner = nullptr;
            static int dragIndex = -1;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(id, ImVec2(width, size));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 max(origin.x + width, origin.y + size);
            draw->AddRectFilled(origin, max, IM_COL32(24, 24, 28, 255));
            for (int g = 1; g < 4; ++g)
            {
                const float t = static_cast<float>(g) / 4.0f;
                draw->AddLine(ImVec2(origin.x + width * t, origin.y), ImVec2(origin.x + width * t, max.y), IM_COL32(60, 60, 68, 255));
                draw->AddLine(ImVec2(origin.x, origin.y + size * t), ImVec2(max.x, origin.y + size * t), IM_COL32(60, 60, 68, 255));
            }
            draw->AddLine(ImVec2(origin.x, max.y), ImVec2(max.x, origin.y), IM_COL32(80, 80, 90, 255)); // y = x
            const auto toScreen = [&](float x, float y) { return ImVec2(origin.x + width * std::clamp(x, 0.0f, 1.0f), max.y - size * std::clamp(y, 0.0f, 1.0f)); };
            ImVec2 previous = toScreen(0.0f, EvalCurve(keys, 0.0f, mode));
            for (int s = 1; s <= 64; ++s)
            {
                const float x = static_cast<float>(s) / 64.0f;
                const ImVec2 next = toScreen(x, EvalCurve(keys, x, mode));
                draw->AddLine(previous, next, lineColor, 2.0f);
                previous = next;
            }
            for (size_t i = 0; i < keys.size(); ++i)
            {
                draw->AddCircleFilled(toScreen(keys[i].X, keys[i].Y[0]), 4.0f, IM_COL32(255, 255, 255, 255));
            }
            // 操作：ドラッグで点を動かす / ダブルクリックで追加 / 右クリックで削除
            const float mouseX = e.Host.ScreenToCanvasX(ImGui::GetIO().MousePos.x);
            const float mouseY = e.Host.ScreenToCanvasY(ImGui::GetIO().MousePos.y);
            const float nx = std::clamp((mouseX - origin.x) / width, 0.0f, 1.0f);
            const float ny = std::clamp(1.0f - (mouseY - origin.y) / size, 0.0f, 1.0f);
            const auto nearest = [&]() {
                int best = -1;
                float bestDistance = 12.0f / (std::min)(width, size);
                for (size_t i = 0; i < keys.size(); ++i)
                {
                    const float d = std::hypot(keys[i].X - nx, keys[i].Y[0] - ny);
                    if (d < bestDistance)
                    {
                        bestDistance = d;
                        best = static_cast<int>(i);
                    }
                }
                return best;
            };
            bool changed = false;
            const void* owner = &keys;
            (void)owner;
            const void* ownerKey = static_cast<const void*>(&e.Node);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && static_cast<int>(keys.size()) < maxPoints && nearest() < 0)
            {
                e.Begin();
                Key key;
                key.X = nx;
                key.Y[0] = ny;
                keys.push_back(key);
                std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.X < b.X; });
                changed = true;
                e.Done();
                e.End();
            }
            else if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1) && keys.size() > 2)
            {
                const int index = nearest();
                if (index > 0 && index + 1 < static_cast<int>(keys.size())) // 端の点は消さない
                {
                    e.Begin();
                    keys.erase(keys.begin() + index);
                    changed = true;
                    e.Done();
                    e.End();
                }
            }
            else if (ImGui::IsItemActivated())
            {
                dragOwner = ownerKey;
                dragIndex = nearest();
            }
            if (ImGui::IsItemActive() && dragOwner == ownerKey && dragIndex >= 0 && dragIndex < static_cast<int>(keys.size()))
            {
                const int last = static_cast<int>(keys.size()) - 1;
                const float lo = dragIndex == 0 ? 0.0f : keys[static_cast<size_t>(dragIndex) - 1].X + 0.005f;
                const float hi = dragIndex == last ? 1.0f : keys[static_cast<size_t>(dragIndex) + 1].X - 0.005f;
                e.Begin();
                // 両端の点は x を固定する（0 と 1）
                keys[static_cast<size_t>(dragIndex)].X = (dragIndex == 0 || dragIndex == last) ? keys[static_cast<size_t>(dragIndex)].X : std::clamp(nx, lo, (std::max)(lo, hi));
                keys[static_cast<size_t>(dragIndex)].Y[0] = ny;
                changed = true;
                e.Done();
            }
            if (ImGui::IsItemDeactivated())
            {
                dragIndex = -1;
                dragOwner = nullptr;
                e.End();
            }
            return changed;
        }

        void DrawCurvesEditor(Edit& e, float width)
        {
            static int selectedCurve = 0;
            static uint64_t selectedNode = 0;
            if (selectedNode != e.Node.Id)
            {
                selectedNode = e.Node.Id;
                selectedCurve = 0;
            }
            static const char* labels[] = {"RGB", "R", "G", "B"};
            static const ImU32 colors[] = {IM_COL32(230, 230, 230, 255), IM_COL32(240, 90, 90, 255), IM_COL32(90, 220, 110, 255), IM_COL32(100, 140, 255, 255)};
            for (int c = 0; c < 4; ++c)
            {
                if (c)
                {
                    ImGui::SameLine(0.0f, 2.0f);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(colors[c]));
                if (ImGui::Selectable(labels[c], selectedCurve == c, 0, ImVec2(width / 4.0f - 2.0f, 0)))
                {
                    selectedCurve = c;
                }
                ImGui::PopStyleColor();
            }
            if (e.Node.Params.size() < static_cast<size_t>(4 * kCurveStride))
            {
                e.Begin();
                e.Node.Params = DefaultCurveParams();
                e.Done();
            }
            auto keys = CurveKeys(e.Node.Params, selectedCurve);
            const Interp mode = e.Node.Value[0] > 0.5f ? Interp::Smooth : Interp::Linear;
            if (DrawCurveWidget(e, keys, mode, width, width * 0.8f, "##Curve", kMaxCurvePoints, colors[selectedCurve]))
            {
                SetCurveKeys(e.Node.Params, selectedCurve, keys);
            }
            ValueToggle(e, 0, "スムーズ", "オン：単調 3 次（なめらか）\nオフ：折れ線");
            ImGui::SameLine();
            if (ImGui::SmallButton("リセット"))
            {
                e.Begin();
                auto reset = std::vector<Key>{Key{0.0f, {0.0f, 0, 0}}, Key{1.0f, {1.0f, 0, 0}}};
                SetCurveKeys(e.Node.Params, selectedCurve, reset);
                e.Done();
                e.End();
            }
            ImGui::TextDisabled("ダブルクリック：点を追加 / 右クリック：削除");
        }

        void DrawAtlasEditor(Edit& e, float width)
        {
            static int selectedRow = 0;
            auto rows = AtlasRows(e.Node);
            if (rows.empty())
            {
                e.Begin();
                e.Node.Params = DefaultAtlasParams();
                e.Done();
                e.End();
                rows = AtlasRows(e.Node);
            }
            selectedRow = std::clamp(selectedRow, 0, static_cast<int>(rows.size()) - 1);
            ImGui::SetNextItemWidth(width * 0.5f);
            ImGui::SliderInt("##AtlasRow", &selectedRow, 0, static_cast<int>(rows.size()) - 1, "行 %d");
            ImGui::SameLine();
            ImGui::BeginDisabled(static_cast<int>(rows.size()) >= kMaxCurveRows);
            if (ImGui::SmallButton("+行"))
            {
                e.Begin();
                rows.push_back({Key{0.0f, {0.0f, 0, 0}}, Key{1.0f, {1.0f, 0, 0}}});
                SetAtlasRows(e.Node, rows);
                selectedRow = static_cast<int>(rows.size()) - 1;
                e.Done();
                e.End();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(rows.size() <= 1);
            if (ImGui::SmallButton("-行"))
            {
                e.Begin();
                rows.erase(rows.begin() + selectedRow);
                SetAtlasRows(e.Node, rows);
                e.Done();
                e.End();
                selectedRow = (std::max)(0, selectedRow - 1);
            }
            ImGui::EndDisabled();
            auto& keys = rows[static_cast<size_t>(selectedRow)];
            const Interp mode = static_cast<Interp>(std::clamp(static_cast<int>(e.Node.Value[0]), 0, 2));
            if (DrawCurveWidget(e, keys, mode, width, width * 0.7f, "##AtlasCurve", kMaxCurveKeys, IM_COL32(120, 200, 255, 255)))
            {
                SetAtlasRows(e.Node, rows);
            }
            ImGui::TextDisabled("ダブルクリック：点を追加 / 右クリック：削除（x は 0〜1。Time はこの範囲）");
        }

        // ---------------------------------------------------------------- Custom HLSL
        void DrawCustomHlsl(Edit& e, float width)
        {
            ValueSelector(e, 0, kMaxCustomInputs + 1, [](int i) -> const char* {
                static const char* names[] = {"入力: 0", "入力: 1", "入力: 2", "入力: 3", "入力: 4"};
                return names[std::clamp(i, 0, 4)];
            }, width * 0.48f, "##CustomInputs");
            ImGui::SameLine(0.0f, 4.0f);
            ValueSelector(e, 1, 4, [](int i) -> const char* {
                static const char* names[] = {"出力: float", "出力: float2", "出力: float3", "出力: float4"};
                return names[std::clamp(i, 0, 3)];
            }, width * 0.48f, "##CustomOutput");
            const int inputCount = std::clamp(static_cast<int>(e.Node.Value[0]), 0, kMaxCustomInputs);
            if (e.Node.PinNames.size() < static_cast<size_t>(kMaxCustomInputs))
            {
                e.Node.PinNames.resize(kMaxCustomInputs);
                for (int i = 0; i < kMaxCustomInputs; ++i)
                {
                    if (e.Node.PinNames[static_cast<size_t>(i)].empty())
                    {
                        e.Node.PinNames[static_cast<size_t>(i)] = std::string(1, static_cast<char>('a' + i));
                    }
                }
            }
            for (int i = 0; i < inputCount; ++i)
            {
                ImGui::PushID(i);
                TextField(e, e.Node.PinNames[static_cast<size_t>(i)], "##PinName", width * 0.3f, 24);
                if (i + 1 < inputCount && (i % 3) != 2)
                {
                    ImGui::SameLine(0.0f, 2.0f);
                }
                ImGui::PopID();
            }
            TextField(e, e.Node.Text, "##Expr", width, 4000, true, 70.0f);
            if (ImGui::IsItemHovered())
            {
                e.Host.Tooltip("HLSL の式（1 つ）。使える：数値 / 入力名 / + - * / / 比較 / ?: / .xyz / float2〜4() /\n"
                               "abs ceil floor frac round trunc sign saturate sqrt sin cos tan asin acos atan exp log2\n"
                               "pow min max fmod step atan2 lerp clamp smoothstep dot cross length distance normalize\n"
                               "テクスチャ / バッファ / 代入 / ループ / #include は使えません（位置付きのエラーになります）");
            }
            // 型検査（入力の型は接続元から決まる）
            std::vector<std::string> names;
            std::vector<int> dims;
            for (int i = 0; i < inputCount; ++i)
            {
                names.push_back(static_cast<size_t>(i) < e.Node.PinNames.size() ? e.Node.PinNames[static_cast<size_t>(i)] : std::string(1, static_cast<char>('a' + i)));
                const auto* link = static_cast<size_t>(i) < e.Node.InputPins.size() ? e.Host.Graph().InputLink(e.Node.InputPins[static_cast<size_t>(i)]) : nullptr;
                dims.push_back(link ? (std::clamp)(static_cast<int>(e.Host.Graph().PinOutputType(link->SourcePin)), 1, 4) : 1);
            }
            const auto compiled = MaterialGraphCustomHlsl::Compile(e.Node.Text, names, dims, static_cast<int>(e.Node.Value[1]));
            if (!compiled.Ok())
            {
                for (const auto& error : compiled.Errors)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.38f, 0.34f, 1.0f));
                    ImGui::TextUnformatted(("⚠ " + error.Format()).c_str());
                    ImGui::PopStyleColor();
                    // ソース位置：該当行を示す
                    int line = 1;
                    size_t begin = 0;
                    for (size_t i = 0; i < e.Node.Text.size() && line < error.Line; ++i)
                    {
                        if (e.Node.Text[i] == '\n')
                        {
                            ++line;
                            begin = i + 1;
                        }
                    }
                    const size_t end = e.Node.Text.find('\n', begin);
                    const std::string source = e.Node.Text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                    ImGui::TextDisabled("%s", source.substr(0, 60).c_str());
                    ImGui::TextDisabled("%*s^", (std::max)(error.Column - 1, 0), "");
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(0.45f, 0.82f, 0.50f, 1.0f), "✓ 型 OK（float%d）", compiled.Result->OutputDims);
            }
        }

        // ---------------------------------------------------------------- Material Function
        void DrawFunctionCall(Edit& e, float width)
        {
            const auto functions = e.Host.FunctionAssets();
            std::vector<std::string> names;
            int current = -1;
            for (size_t i = 0; i < functions.size(); ++i)
            {
                names.push_back(e.Host.AssetFileName(functions[i]));
                if (functions[i] == e.Node.Function)
                {
                    current = static_cast<int>(i);
                }
            }
            // 関数の選択（ポップアップを使わない：クリックで次の関数へ）
            std::string label = current >= 0 ? names[static_cast<size_t>(current)] : std::string("（関数を選択）");
            label += "##FunctionPick";
            if (ImGui::Button(label.c_str(), ImVec2(width, 0)) && !functions.empty())
            {
                e.Begin();
                e.Node.Function = functions[static_cast<size_t>((current + 1) % static_cast<int>(functions.size()))];
                std::string error;
                MaterialFunctionLibrary::RefreshCall(e.Node, error);
                e.Done();
                e.End();
            }
            if (ImGui::IsItemHovered())
            {
                e.Host.Tooltip("クリックで次の関数へ切り替え / Content Browser の .matfunc をドロップでも設定できます");
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetPayloadType))
                {
                    if (payload->DataSize == sizeof(AssetDragPayload))
                    {
                        const auto& asset = *static_cast<const AssetDragPayload*>(payload->Data);
                        if (e.Host.IsAssetType(asset.Handle, AssetType::MaterialFunction))
                        {
                            e.Begin();
                            e.Node.Function = asset.Handle;
                            std::string error;
                            MaterialFunctionLibrary::RefreshCall(e.Node, error);
                            e.Done();
                            e.End();
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::SmallButton("新規…"))
            {
                if (const AssetHandle created = e.Host.CreateFunctionAsset())
                {
                    e.Begin();
                    e.Node.Function = created;
                    std::string error;
                    MaterialFunctionLibrary::RefreshCall(e.Node, error);
                    e.Done();
                    e.End();
                    e.Host.OpenFunctionAsset(created);
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(e.Node.Function == 0);
            if (ImGui::SmallButton("編集"))
            {
                e.Host.OpenFunctionAsset(e.Node.Function);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("更新"))
            {
                MaterialFunctionLibrary::Invalidate(e.Node.Function);
                e.Begin();
                std::string error;
                MaterialFunctionLibrary::RefreshCall(e.Node, error);
                e.Done();
                e.End();
            }
            ImGui::EndDisabled();
            if (e.Node.Function != 0)
            {
                std::string error;
                if (!MaterialFunctionLibrary::ValidateReferences(e.Host.Graph(), 0, error) || !MaterialFunctionLibrary::ValidateCall(e.Host.Graph(), e.Node, error))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.38f, 0.34f, 1.0f));
                    ImGui::TextWrapped("⚠ %s", error.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }

        void DrawFunctionPort(Edit& e, float width, bool input)
        {
            ImGui::TextDisabled("%s", input ? "関数の入力（呼び出しノードのピンになります）" : "関数の出力");
            TextField(e, e.Node.Text, "##PortName", width, 40);
            int dims = std::clamp(static_cast<int>(e.Node.Value[0]), 1, 4);
            ImGui::SetNextItemWidth(width * 0.48f);
            if (ImGui::SliderInt("##PortDims", &dims, 1, 4, dims == 1 ? "float" : (dims == 2 ? "float2" : (dims == 3 ? "float3" : "float4"))))
            {
                e.Begin();
                e.Node.Value[0] = static_cast<float>(dims);
                e.Done();
            }
            if (ImGui::IsItemDeactivated())
            {
                e.End();
            }
            ImGui::SameLine(0.0f, 4.0f);
            int index = std::clamp(static_cast<int>(e.Node.Value[1]), 0, (input ? kMaxFunctionInputs : kMaxFunctionOutputs) - 1);
            ImGui::SetNextItemWidth(width * 0.48f);
            if (ImGui::SliderInt("##PortIndex", &index, 0, (input ? kMaxFunctionInputs : kMaxFunctionOutputs) - 1, input ? "入力 %d" : "出力 %d"))
            {
                e.Begin();
                e.Node.Value[1] = static_cast<float>(index);
                e.Done();
            }
            if (ImGui::IsItemDeactivated())
            {
                e.End();
            }
            if (input)
            {
                // 既定値（呼び出しで未接続のときの値 / 関数内のプレビュー）
                e.Node.Params.resize(4, 0.0f);
                float v[4] = {e.Node.Params[0], e.Node.Params[1], e.Node.Params[2], e.Node.Params[3]};
                ImGui::SetNextItemWidth(width);
                if (ImGui::DragFloat4("##PortDefault", v, 0.01f, 0.0f, 0.0f, "%.2f"))
                {
                    e.Begin();
                    for (int i = 0; i < 4; ++i)
                    {
                        if (std::isfinite(v[i]))
                        {
                            e.Node.Params[static_cast<size_t>(i)] = v[i];
                        }
                    }
                    e.Done();
                }
                if (ImGui::IsItemDeactivated())
                {
                    e.End();
                }
            }
        }
    } // namespace

    bool DrawNodeEditor(EditHost& host, MaterialGraphNode& node, float width)
    {
        if (!IsExtKind(node.Kind))
        {
            return false;
        }
        Edit e{host, node};
        switch (node.Kind)
        {
            case EditKind::ToonLightRamp: {
                ValueToggle(e, 0, "ランプテクスチャ", "オフ：下の 3 色 / オン：テクスチャの横軸 = 帯の位置（0 = 影 → 1 = 光）");
                if (node.Value[0] > 0.5f)
                {
                    TextureDrop(e, width, "ランプ画像（横方向のグラデーション）");
                }
                ValueSelector(e, 1, 3, &ToonClassName, width, "##ToonClass");
                host.Tooltip("素材によって陰影の回り込み（wrap）が変わります（肌 0.38 / それ以外 0.12。既存のトゥーンと同じ）");
                break;
            }
            case EditKind::FaceSdfShadow:
                TextureDrop(e, width, "顔の SDF テクスチャ（R）。左右反転した値は同じ画像を U 反転して読みます", "SDF をドロップ");
                break;
            case EditKind::MatCap:
                TextureDrop(e, width, "MatCap テクスチャ（未設定のときは標準の球面シェーディング）");
                ValueSelector(e, 0, static_cast<int>(MaterialBlendMode::Count), &MaterialGraphMath::BlendModeName, width, "##MatCapBlend");
                break;
            case EditKind::MultiColorRamp:
                DrawRampEditor(e, width);
                break;
            case EditKind::TextureParameter2D: {
                TextField(e, node.Text, "##ParamName", width, 48);
                if (ImGui::IsItemHovered())
                {
                    host.Tooltip("パラメーター名。マテリアルインスタンスの「テクスチャ上書き」はこの名前で差し替えます");
                }
                TextureDrop(e, width, "既定のテクスチャ（マテリアルで上書きされなければ使われます）", "既定のテクスチャ");
                ValueSelector(e, 0, 3, &SamplerName, width, "##ParamSampler");
                node.Params.resize(4, 1.0f);
                float fallback[4] = {node.Params[0], node.Params[1], node.Params[2], node.Params[3]};
                ImGui::SetNextItemWidth(width);
                if (ImGui::DragFloat4("##Fallback", fallback, 0.01f, 0.0f, 1.0f, "%.2f"))
                {
                    e.Begin();
                    for (int i = 0; i < 4; ++i)
                    {
                        if (std::isfinite(fallback[i]))
                        {
                            node.Params[static_cast<size_t>(i)] = fallback[i];
                        }
                    }
                    e.Done();
                }
                if (ImGui::IsItemDeactivated())
                {
                    e.End();
                }
                if (ImGui::IsItemHovered())
                {
                    host.Tooltip("テクスチャが未設定のときの色（RGBA）");
                }
                break;
            }
            case EditKind::PackedTextureChannels:
                TextureDrop(e, width, "ORM などのチャンネルパックされたテクスチャ");
                ValueSelector(e, 0, 3, &SamplerName, width, "##PackedSampler");
                break;
            case EditKind::FlowMap:
                TextureDrop(e, width, "フローマップ（RG = 方向。0.5 が無移動）。Flow ピンに繋ぐとテクスチャより優先します", "Flow テクスチャをドロップ");
                break;
            case EditKind::TriplanarNormalCorrected:
                TextureDrop(e, width, "法線マップ（3 方向から投影します）");
                ValueToggle(e, 0, "ワールド空間で出力", "オフ：接空間（Material Output の Normal に繋ぐ）/ オン：ワールド空間");
                break;
            case EditKind::ThicknessMap:
                TextureDrop(e, width, "厚みマップ（白 = 厚い）");
                ValueToggle(e, 0, "反転");
                ValueDrag(e, 1, "##ThicknessChannel", 0.1f, "チャンネル %.0f", width, 0.0f, static_cast<float>(MaterialGraphMath::kConvertChannelCount - 1));
                break;
            case EditKind::CurveAtlas: {
                TextureDrop(e, width, "カーブのアトラス画像（横 = 時間、行 = カーブ）。未設定なら下の内蔵カーブを使います", "内蔵カーブ（画像で置き換え可）");
                ValueSelector(e, 0, 3, &InterpName, width, "##AtlasInterp");
                ValueSelector(e, 1, 3, &LoopName, width, "##AtlasLoop");
                if (node.Texture != 0)
                {
                    ValueDrag(e, 2, "##AtlasRows", 0.1f, "行数 %.0f", width, 1.0f, 64.0f);
                    ValueDrag(e, 3, "##AtlasChannel", 0.1f, "チャンネル %.0f", width, 0.0f, 3.0f);
                }
                else
                {
                    DrawAtlasEditor(e, width);
                }
                break;
            }
            case EditKind::RgbCurves:
                DrawCurvesEditor(e, width);
                break;
            case EditKind::FlipbookUV:
                ValueSelector(e, 0, 3, &LoopName, width, "##FlipLoop");
                break;
            case EditKind::TwoSidedSign:
                ValueToggle(e, 0, "0 / 1 で出力", "オフ：表 +1 / 裏 -1　オン：表 1 / 裏 0");
                break;
            case EditKind::CurvatureMask:
                ValueSelector(e, 0, 3, &CurvatureModeName, width, "##CurvatureMode");
                ImGui::TextDisabled("曲率はメッシュのベイク / 画素の微分から");
                break;
            case EditKind::MaterialLayerBlend:
                ValueToggle(e, 0, "高さでブレンド", "オン：Height の高い方が先に残る（Height Lerp と同じ考え方）");
                break;
            case EditKind::AudioSpectrum:
                ValueToggle(e, 0, "周波数 Hz で指定", "オフ：バンド番号 0〜31 / オン：周波数（20Hz〜20kHz の対数）");
                break;
            case EditKind::DynamicSwitch:
                ValueDrag(e, 0, "##SwitchThreshold", 0.01f, "しきい値 %.2f", width);
                host.Tooltip("Condition > しきい値 で True 側（実行時に GPU が分岐。Static Switch はコンパイル時に分岐）");
                break;
            case EditKind::CustomHlsl:
                DrawCustomHlsl(e, width);
                break;
            case EditKind::MaterialFunctionCall:
                DrawFunctionCall(e, width);
                break;
            case EditKind::FunctionInput:
                DrawFunctionPort(e, width, true);
                break;
            case EditKind::FunctionOutput:
                DrawFunctionPort(e, width, false);
                break;
            default:
                break; // Directional Rim / Hair / Eye Highlight / HSV Adjust / Thin Film など：ピンの横の数値だけ
        }
        return true;
    }

    const char* PinLabel(const MaterialGraphNode& node, size_t input, std::string& storage)
    {
        if (node.Kind == EditKind::CustomHlsl && input < node.PinNames.size() && !node.PinNames[input].empty())
        {
            storage = node.PinNames[input];
            return storage.c_str();
        }
        if (node.Kind == EditKind::MaterialFunctionCall && input < node.PinNames.size() && !node.PinNames[input].empty())
        {
            storage = node.PinNames[input];
            return storage.c_str();
        }
        return nullptr;
    }

    bool IsPinUnused(const MaterialGraphNode& node, size_t input)
    {
        if (node.Kind == EditKind::CustomHlsl)
        {
            return static_cast<int>(input) >= std::clamp(static_cast<int>(node.Value[0]), 0, kMaxCustomInputs);
        }
        if (node.Kind == EditKind::MaterialFunctionCall)
        {
            return node.Params.size() > 5 && static_cast<int>(input) >= static_cast<int>(node.Params[5]);
        }
        return false;
    }

    const char* SlotLabel(const MaterialGraphNode& node, size_t slot, std::string& storage)
    {
        if (node.Kind == EditKind::MaterialFunctionCall)
        {
            if (slot < 4 && node.PinNames.size() > 8 + slot && !node.PinNames[8 + slot].empty())
            {
                storage = node.PinNames[8 + slot];
                return storage.c_str();
            }
            return nullptr;
        }
        const auto slots = MaterialGraph::FixedOutputSlots(node.Kind);
        return slot < slots.size() ? slots[slot].Name : nullptr;
    }

    bool ShowInAddMenu(MaterialNodeKind kind, const MaterialGraph& graph)
    {
        if (kind == MaterialNodeKind::FunctionInput || kind == MaterialNodeKind::FunctionOutput)
        {
            return graph.Settings.IsFunction;
        }
        if (kind == MaterialNodeKind::Output)
        {
            return !graph.Settings.IsFunction;
        }
        return true;
    }
} // namespace MaterialGraphExtEditor
