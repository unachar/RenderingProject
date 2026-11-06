#include "pch.h"
#include "niagaragraph.h"
#include "graphvisuals.h"
#include <unordered_set>
#include "../../External/imgui-node-editor/imgui_node_editor.h"
#include <imgui.h>
#include <cmath>

namespace NiagaraGraph
{
    namespace Nodes = ax::NodeEditor;

    bool Draw(Niagara::Module& module, Nodes::EditorContext* context, std::string& diagnostic)
    {
        bool changed = false;
        Nodes::SetCurrentEditor(context);
        if (ImGui::Button("ノードを追加"))
        {
            ImGui::OpenPopup("NiagaraNodePalette");
        }
        if (ImGui::BeginPopup("NiagaraNodePalette"))
        {
            for (const auto& definition : MaterialGraph::Definitions())
            {
                if (!Niagara::SupportsGraphNode(definition.Kind))
                {
                    continue;
                }
                if (ImGui::MenuItem(definition.Name))
                {
                    const auto id = module.Graph.AddNode(definition.Kind, 40.0f, 40.0f);
                    Nodes::SetNodePosition(Nodes::NodeId(id), {40.0f, 40.0f});
                    changed = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("全体を表示"))
        {
            Nodes::NavigateToContent();
        }
        ImGui::SameLine();
        if (ImGui::Button("選択ノードを出力に"))
        {
            Nodes::NodeId selected;
            if (Nodes::GetSelectedNodes(&selected, 1) == 1)
            {
                if (const auto* node = module.Graph.FindNode(selected.Get()); node && node->OutputPin != 0)
                {
                    module.OutputPin = node->OutputPin;
                    changed = true;
                }
            }
        }
        ImGui::TextDisabled(
            "World Position: 粒子位置 / Time: システム時刻 / Particles.NormalizedAge: 寿命比率 / パラメーター: 名前で参照");
        std::unordered_set<uint64_t> connected;
        for (const auto& link : module.Graph.Links)
        {
            connected.insert(link.SourcePin);
            connected.insert(link.TargetPin);
        }
        GraphVisuals::PushCanvasStyle();
        Nodes::Begin("NiagaraModuleCanvas", {0, (std::max)(120.0f, ImGui::GetContentRegionAvail().y)});
        for (auto& node : module.Graph.Nodes)
        {
            if (node.Kind == MaterialNodeKind::Output)
            {
                continue;
            }
            const auto& definition = MaterialGraph::Definition(node.Kind);
            ImGui::PushID(static_cast<int>(node.Id));
            Nodes::BeginNode(Nodes::NodeId(node.Id));
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float width = (std::max)(220.0f, ImGui::CalcTextSize(definition.Name).x + 90.0f);
            ImGui::TextUnformatted(definition.Name);
            const float headerBottom = ImGui::GetItemRectMax().y;
            if (node.OutputPin && node.SplitPins.empty())
            {
                ImGui::SameLine();
                ImGui::SetCursorScreenPos({start.x + width - ImGui::GetTextLineHeight() * 0.72f, start.y});
                Nodes::BeginPin(Nodes::PinId(node.OutputPin), Nodes::PinKind::Output);
                Nodes::PinPivotAlignment({1, 0.5f});
                Nodes::PinPivotSize({0, 0});
                GraphVisuals::PinIcon(GraphVisuals::TypeColor(module.Graph.ResolveOutputType(node.Id)),
                                      connected.contains(node.OutputPin));
                Nodes::EndPin();
            }
            ImGui::Dummy({width, 4});
            if (module.OutputPin == node.OutputPin ||
                std::find(node.SplitPins.begin(), node.SplitPins.end(), module.OutputPin) != node.SplitPins.end())
            {
                ImGui::TextColored({0.95f, 0.72f, 0.32f, 1}, "シミュレーション出力");
            }
            const bool parameter =
                node.Kind == MaterialNodeKind::ScalarParameter || node.Kind == MaterialNodeKind::VectorParameter ||
                node.Kind == MaterialNodeKind::Vector2Parameter || node.Kind == MaterialNodeKind::ColorParameter;
            if (parameter)
            {
                char name[256]{};
                snprintf(name, sizeof(name), "%s", node.Name.c_str());
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::InputText("名前", name, sizeof(name)))
                {
                    node.Name = name;
                    changed = true;
                }
            }
            if (MaterialGraph::IsComponentConstant(node.Kind) || parameter || node.Kind == MaterialNodeKind::Constant)
            {
                const int components = (std::clamp)(static_cast<int>(definition.OutputType), 1, 4);
                ImGui::SetNextItemWidth(180.0f);
                changed |= ImGui::DragScalarN("値", ImGuiDataType_Float, node.Value.data(), components, 0.01f);
            }
            const size_t rows = (std::max)(node.InputPins.size(), node.SplitPins.size());
            for (size_t index = 0; index < rows; ++index)
            {
                const float rowY = ImGui::GetCursorScreenPos().y;
                if (index < node.InputPins.size())
                {
                    const auto pin = node.InputPins[index];
                    const auto* link = module.Graph.RawInputLink(pin);
                    const bool component =
                        MaterialGraph::IsComponentConstant(node.Kind) && node.Kind != MaterialNodeKind::Constant;
                    const auto type =
                        link ? module.Graph.PinOutputType(link->SourcePin) : definition.Inputs[index].Type;
                    Nodes::BeginPin(Nodes::PinId(pin), Nodes::PinKind::Input);
                    Nodes::PinPivotAlignment({0, 0.5f});
                    Nodes::PinPivotSize({0, 0});
                    const auto color = component ? ImGui::ColorConvertFloat4ToU32(GraphVisuals::ComponentColor(index))
                                                 : GraphVisuals::TypeColor(type);
                    GraphVisuals::PinIcon(color, link != nullptr);
                    ImGui::SameLine(0, 4);
                    ImGui::TextUnformatted(component ? GraphVisuals::ComponentName(index)
                                                     : definition.Inputs[index].Name);
                    Nodes::EndPin();
                    if (!link && (component || MaterialGraph::IsPinValueEditable(node.Kind, index)))
                    {
                        float value =
                            component ? node.Value[index] : MaterialGraph::UnconnectedInputValue(node, index)[0];
                        ImGui::PushID(static_cast<int>(index));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(64);
                        if (ImGui::DragFloat("##InputValue", &value, 0.01f))
                        {
                            if (component)
                            {
                                node.Value[index] = value;
                            }
                            else
                            {
                                node.PinValues.resize(node.InputPins.size(), std::nanf(""));
                                node.PinValues[index] = value;
                            }
                            changed = true;
                        }
                        ImGui::PopID();
                    }
                }
                if (index < node.SplitPins.size())
                {
                    if (index < node.InputPins.size())
                    {
                        ImGui::SameLine();
                    }
                    ImGui::SetCursorScreenPos({(std::max)(start.x + width - 76, ImGui::GetCursorScreenPos().x), rowY});
                    Nodes::BeginPin(Nodes::PinId(node.SplitPins[index]), Nodes::PinKind::Output);
                    Nodes::PinPivotAlignment({1, 0.5f});
                    Nodes::PinPivotSize({0, 0});
                    ImGui::TextColored(GraphVisuals::ComponentColor(index), "%s", GraphVisuals::ComponentName(index));
                    ImGui::SameLine(0, 4);
                    GraphVisuals::PinIcon(ImGui::ColorConvertFloat4ToU32(GraphVisuals::ComponentColor(index)),
                                          connected.contains(node.SplitPins[index]));
                    Nodes::EndPin();
                }
            }
            ImGui::Dummy({width, 6});
            Nodes::EndNode();
            GraphVisuals::DrawHeader(Nodes::NodeId(node.Id), headerBottom, GraphVisuals::Category(node.Kind).Color);
            const auto position = Nodes::GetNodePosition(Nodes::NodeId(node.Id));
            if (position.x != node.Position[0] || position.y != node.Position[1])
            {
                node.Position = {position.x, position.y};
                changed = true;
            }
            ImGui::PopID();
        }
        for (const auto& link : module.Graph.Links)
        {
            const auto color =
                ImGui::ColorConvertU32ToFloat4(GraphVisuals::TypeColor(module.Graph.PinOutputType(link.SourcePin)));
            Nodes::Link(Nodes::LinkId(link.Id), Nodes::PinId(link.SourcePin), Nodes::PinId(link.TargetPin), color,
                        2.0f);
        }
        if (Nodes::BeginCreate())
        {
            Nodes::PinId first, second;
            if (Nodes::QueryNewLink(&first, &second) && first && second)
            {
                uint64_t source = first.Get();
                uint64_t target = second.Get();
                bool input = false;
                module.Graph.FindPinNode(source, &input);
                if (input)
                {
                    std::swap(source, target);
                }
                std::string error;
                if (module.Graph.CanConnect(source, target, error))
                {
                    if (Nodes::AcceptNewItem())
                    {
                        changed |= module.Graph.Connect(source, target, diagnostic);
                    }
                }
                else
                {
                    Nodes::RejectNewItem();
                    diagnostic = error;
                }
            }
        }
        Nodes::EndCreate();
        if (Nodes::BeginDelete())
        {
            Nodes::LinkId link;
            while (Nodes::QueryDeletedLink(&link))
            {
                if (Nodes::AcceptDeletedItem())
                {
                    changed |= module.Graph.RemoveLink(link.Get());
                }
            }
            Nodes::NodeId node;
            while (Nodes::QueryDeletedNode(&node))
            {
                const auto* model = module.Graph.FindNode(node.Get());
                if (model && Nodes::AcceptDeletedItem())
                {
                    if (module.OutputPin == model->OutputPin ||
                        std::find(model->SplitPins.begin(), model->SplitPins.end(), module.OutputPin) !=
                            model->SplitPins.end())
                    {
                        module.OutputPin = 0;
                    }
                    changed |= module.Graph.RemoveNode(node.Get());
                }
            }
        }
        Nodes::EndDelete();
        static Nodes::PinId contextPin;
        Nodes::Suspend();
        if (Nodes::ShowPinContextMenu(&contextPin))
        {
            ImGui::OpenPopup("NiagaraPinMenu");
        }
        if (ImGui::BeginPopup("NiagaraPinMenu"))
        {
            bool input = false;
            size_t inputIndex = 0;
            const auto* node = module.Graph.FindPinNode(contextPin.Get(), &input, &inputIndex);
            if (node)
            {
                if (input && inputIndex < node->PinValues.size() && ImGui::MenuItem("既定値に戻す"))
                {
                    module.Graph.FindNode(node->Id)->PinValues[inputIndex] = std::nanf("");
                    changed = true;
                }
                if (!input)
                {
                    if (ImGui::MenuItem("このピンを出力に"))
                    {
                        module.OutputPin = contextPin.Get();
                        changed = true;
                    }
                    if (node->SplitPins.empty() && module.Graph.CanSplitOutput(node->Id) &&
                        ImGui::MenuItem("ピンを分割"))
                    {
                        changed |= module.Graph.SplitOutput(node->Id, diagnostic);
                    }
                    if (!node->SplitPins.empty() && ImGui::MenuItem("ピンを統合"))
                    {
                        if (std::find(node->SplitPins.begin(), node->SplitPins.end(), module.OutputPin) !=
                            node->SplitPins.end())
                        {
                            module.OutputPin = node->OutputPin;
                        }
                        changed |= module.Graph.RecombineOutput(node->Id);
                    }
                }
                if (connected.contains(contextPin.Get()) && ImGui::MenuItem("接続を解除"))
                {
                    std::vector<uint64_t> links;
                    for (const auto& link : module.Graph.Links)
                    {
                        if (link.SourcePin == contextPin.Get() || link.TargetPin == contextPin.Get())
                        {
                            links.push_back(link.Id);
                        }
                    }
                    for (const auto id : links)
                    {
                        changed |= module.Graph.RemoveLink(id);
                    }
                }
            }
            ImGui::EndPopup();
        }
        Nodes::Resume();
        Nodes::End();
        Nodes::PopStyleColor(6);
        Nodes::SetCurrentEditor(nullptr);
        return changed;
    }
}
