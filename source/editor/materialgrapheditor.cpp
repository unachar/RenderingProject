#include "pch.h"
#include "materialgrapheditor.h"
#include "materialgraph.h"
#include "materialgraphcompiler.h"
#include "materialgraphshader.h"
#include "materialmanager.h"
#include "../../External/imgui-node-editor/imgui_node_editor.h"
#include <json/json.h>
#include <fstream>
#include <optional>
#include <future>
#include <chrono>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
    namespace Nodes = ax::NodeEditor;
    using Kind = MaterialNodeKind;

    struct GraphDocument
    {
        MaterialGraph Graph;
        MaterialGraph SavedGraph;
        std::vector<MaterialGraph> Undo;
        std::vector<MaterialGraph> Redo;
        std::optional<MaterialGraph> PendingEdit;
        std::optional<MaterialGraph> PendingMove;
        std::filesystem::path Path;
        std::filesystem::path RecoveryPath;
        Nodes::EditorContext* Context = nullptr;
        std::string Layout;
        std::string Error;
        std::string Status;
        bool Dirty = false;
        bool RecoveryDirty = false;
        bool LoadFailed = false;
        bool RestorePositions = true;
        int FramesUntilFit = 2;
        bool ValueChanged = false;
        uint64_t PendingValueNode = 0;
        char Search[128]{};
        char Rename[256]{};
        uint64_t ContextNode = 0;
        MaterialGraphProgram Program;
        MaterialGraphShader Shader;
        std::future<MaterialGraphShader> ShaderJob;
        uint64_t CompilingRevision = 0;
        std::vector<uint64_t> PendingSelection;
        ImVec2 AddPosition{};

        ~GraphDocument()
        {
            if (Context)
            {
                Nodes::DestroyEditor(Context);
            }
        }
    };

    std::unordered_map<AssetHandle, std::unique_ptr<GraphDocument>> documents;
    AssetHandle focusedMaterial = 0;
    std::vector<MaterialGraphNode> copiedNodes;
    std::vector<MaterialGraphLink> copiedLinks;
    double nextRecoverySave = 0;

    void ContinueToolbar()
    {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + 120.0f < right)
        {
            ImGui::SameLine();
        }
    }

    void AppendHistory(GraphDocument& document, const MaterialGraph& before)
    {
        if (document.Undo.size() >= 64)
        {
            document.Undo.erase(document.Undo.begin());
        }
        document.Undo.push_back(before);
        document.Redo.clear();
        document.Dirty = true;
        document.RecoveryDirty = true;
    }

    void FinishValueEdit(GraphDocument& document)
    {
        if (!document.PendingEdit)
        {
            return;
        }
        auto before = std::move(*document.PendingEdit);
        document.PendingEdit.reset();
        document.PendingValueNode = 0;
        const bool changed = document.ValueChanged;
        document.ValueChanged = false;
        if (changed)
        {
            AppendHistory(document, before);
        }
    }

    void FinishMove(GraphDocument& document)
    {
        if (!document.PendingMove)
        {
            return;
        }
        auto before = std::move(*document.PendingMove);
        document.PendingMove.reset();
        AppendHistory(document, before);
    }

    void FinishEdits(GraphDocument& document)
    {
        FinishValueEdit(document);
        FinishMove(document);
    }

    void PushHistory(GraphDocument& document, const MaterialGraph& before)
    {
        FinishEdits(document);
        AppendHistory(document, before);
    }

    bool WriteDocument(GraphDocument& document, const std::filesystem::path& path)
    {
        if (!document.Graph.Validate(document.Error))
        {
            return false;
        }
        Json::Value value;
        Json::CharReaderBuilder reader;
        std::istringstream graph(document.Graph.Serialize());
        if (!Json::parseFromStream(reader, graph, &value, &document.Error))
        {
            return false;
        }
        value["editor"] = document.Layout;
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            document.Error = error.message();
            return false;
        }
        return WriteAssetFile(path, Json::writeString(writer, value), document.Error);
    }

    bool ReadDocument(const std::filesystem::path& path, MaterialGraph& graph, std::string& layout, std::string& error)
    {
        if (!MaterialGraph::Load(path, graph, error))
        {
            return false;
        }
        std::ifstream input(path, std::ios::binary);
        Json::Value value;
        Json::CharReaderBuilder reader;
        if (!Json::parseFromStream(reader, input, &value, &error))
        {
            return false;
        }
        if (value["editor"].isString())
        {
            layout = value["editor"].asString();
        }
        return true;
    }

    GraphDocument& Document(AssetHandle material)
    {
        auto& slot = documents[material];
        if (slot)
        {
            return *slot;
        }
        slot = std::make_unique<GraphDocument>();
        auto& document = *slot;
        document.Path = MaterialManager::Registry().Resolve(material);
        document.Path += ".graph.json";
        document.RecoveryPath =
            std::filesystem::path("EditorCache/MaterialGraphs") / (std::to_string(material) + ".recovery.json");
        document.Graph = MaterialGraph::FromMaterial(MaterialManager::GetRuntime(material).Editing);
        std::error_code error;
        if (std::filesystem::exists(document.Path, error))
        {
            document.LoadFailed = !ReadDocument(document.Path, document.Graph, document.Layout, document.Error);
        }
        document.SavedGraph = document.Graph;
        if (!document.LoadFailed && std::filesystem::exists(document.RecoveryPath, error))
        {
            MaterialGraph recovered;
            std::string recoveredLayout;
            if (ReadDocument(document.RecoveryPath, recovered, recoveredLayout, document.Error))
            {
                document.Graph = std::move(recovered);
                document.Layout = std::move(recoveredLayout);
                document.Dirty = document.Graph.Serialize() != document.SavedGraph.Serialize();
                document.Status = "前回の未保存グラフを復元しました。";
            }
        }
        Nodes::Config config;
        config.SettingsFile = nullptr;
        config.UserPointer = &document;
        config.SaveSettings = [](const char* data, size_t size, Nodes::SaveReasonFlags, void* user) {
            auto& target = *static_cast<GraphDocument*>(user);
            target.Layout.assign(data, size);
            target.RecoveryDirty = true;
            return true;
        };
        config.LoadSettings = [](char* data, void* user) {
            const auto& layout = static_cast<GraphDocument*>(user)->Layout;
            if (data)
            {
                memcpy(data, layout.data(), layout.size());
            }
            return layout.size();
        };
        config.CanvasSizeMode = Nodes::CanvasSizeMode::CenterOnly;
        document.Context = Nodes::CreateEditor(&config);
        document.FramesUntilFit = document.Layout.empty() ? 2 : 0;
        return document;
    }

    void RestorePositions(GraphDocument& document)
    {
        for (const auto& node : document.Graph.Nodes)
        {
            Nodes::SetNodePosition(Nodes::NodeId(node.Id), ImVec2(node.Position[0], node.Position[1]));
            if (node.Kind == Kind::Group && document.Layout.empty())
            {
                Nodes::SetGroupSize(Nodes::NodeId(node.Id), ImVec2(node.GroupSize[0], node.GroupSize[1]));
            }
        }
        document.RestorePositions = false;
    }

    void TrackValueEdit(GraphDocument& document, MaterialGraphNode& node, const std::array<float, 4>& before,
                        bool changed)
    {
        if (ImGui::IsItemActivated())
        {
            FinishValueEdit(document);
            document.PendingEdit = document.Graph;
            document.PendingEdit->FindNode(node.Id)->Value = before;
            document.PendingValueNode = node.Id;
            document.ValueChanged = false;
        }
        if (changed)
        {
            // Some pop-up widgets report activation on their child rather than the enclosing item.
            if (!document.PendingEdit)
            {
                document.PendingEdit = document.Graph;
                document.PendingEdit->FindNode(node.Id)->Value = before;
                document.PendingValueNode = node.Id;
            }
            document.ValueChanged = true;
            document.Dirty = true;
            document.RecoveryDirty = true;
            ++document.Graph.ParameterRevision;
        }
        if (document.PendingEdit && document.PendingValueNode == node.Id &&
            (ImGui::IsItemDeactivatedAfterEdit() ||
             (!ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))))
        {
            FinishValueEdit(document);
        }
    }

    void DrawNode(GraphDocument& document, MaterialGraphNode& node)
    {
        const auto& definition = MaterialGraph::Definition(node.Kind);
        Nodes::BeginNode(Nodes::NodeId(node.Id));
        ImGui::PushID(std::to_string(node.Id).c_str());
        ImGui::TextUnformatted(node.Name.c_str());
        if (node.Kind == Kind::Group)
        {
            Nodes::Group(ImVec2(node.GroupSize[0], node.GroupSize[1]));
        }
        else
        {
            ImGui::Separator();
            const auto before = node.Value;
            bool changed = false;
            ImGui::SetNextItemWidth(160.0f);
            if (node.Kind == Kind::Constant || node.Kind == Kind::ScalarParameter)
            {
                changed = ImGui::DragFloat("##Value", node.Value.data(), 0.01f);
                TrackValueEdit(document, node, before, changed);
            }
            else if (node.Kind == Kind::VectorParameter)
            {
                changed = ImGui::DragFloat3("##Vector", node.Value.data(), 0.01f);
                TrackValueEdit(document, node, before, changed);
            }
            else if (node.Kind == Kind::ColorParameter)
            {
                changed = ImGui::ColorEdit4("##Color", node.Value.data(), ImGuiColorEditFlags_Float);
                TrackValueEdit(document, node, before, changed);
            }
            else if (node.Kind == Kind::TextureSample)
            {
                auto path = MaterialManager::Registry().Resolve(node.Texture).filename().string();
                if (path.empty())
                {
                    path = "Textureをドロップ";
                }
                ImGui::Button(path.c_str(), ImVec2(170, 0));
                if (ImGui::BeginDragDropTarget())
                {
                    if (const auto* payload = ImGui::AcceptDragDropPayload(AssetPayloadType))
                    {
                        if (payload->DataSize == sizeof(AssetDragPayload))
                        {
                            const auto& asset = *static_cast<const AssetDragPayload*>(payload->Data);
                            if (asset.Type == AssetType::Texture &&
                                MaterialManager::Registry().TypeOf(asset.Handle) == AssetType::Texture)
                            {
                                PushHistory(document, document.Graph);
                                node.Texture = asset.Handle;
                                ++document.Graph.Revision;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
            }
            for (size_t i = 0; i < node.InputPins.size(); ++i)
            {
                Nodes::BeginPin(Nodes::PinId(node.InputPins[i]), Nodes::PinKind::Input);
                ImGui::Text("o %s", definition.Inputs[i].Name);
                Nodes::EndPin();
            }
            if (node.OutputPin)
            {
                ImGui::Dummy(ImVec2(160, 2));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 160.0f - ImGui::CalcTextSize("Result o").x);
                Nodes::BeginPin(Nodes::PinId(node.OutputPin), Nodes::PinKind::Output);
                ImGui::TextUnformatted("Result o");
                Nodes::EndPin();
            }
        }
        ImGui::PopID();
        Nodes::EndNode();
    }

    void CopySelection(GraphDocument& document)
    {
        copiedNodes.clear();
        copiedLinks.clear();
        std::unordered_set<uint64_t> pins;
        for (const auto& node : document.Graph.Nodes)
        {
            if (node.Kind != Kind::Output && Nodes::IsNodeSelected(Nodes::NodeId(node.Id)))
            {
                copiedNodes.push_back(node);
                pins.insert(node.OutputPin);
                pins.insert(node.InputPins.begin(), node.InputPins.end());
            }
        }
        for (const auto& link : document.Graph.Links)
        {
            if (pins.contains(link.SourcePin) && pins.contains(link.TargetPin))
            {
                copiedLinks.push_back(link);
            }
        }
    }

    void PasteSelection(GraphDocument& document)
    {
        if (copiedNodes.empty())
        {
            return;
        }
        MaterialGraph candidate = document.Graph;
        std::unordered_map<uint64_t, uint64_t> remappedPins;
        std::vector<uint64_t> selection;
        for (const auto& source : copiedNodes)
        {
            const auto id = candidate.AddNode(source.Kind, source.Position[0] + 40, source.Position[1] + 40);
            auto* destination = candidate.FindNode(id);
            if (!destination)
            {
                document.Error = "ノードの追加上限に達しました。";
                return;
            }
            destination->Name = source.Name;
            destination->Value = source.Value;
            destination->Texture = source.Texture;
            destination->GroupSize = source.GroupSize;
            remappedPins[source.OutputPin] = destination->OutputPin;
            for (size_t i = 0; i < source.InputPins.size(); ++i)
            {
                remappedPins[source.InputPins[i]] = destination->InputPins[i];
            }
            selection.push_back(id);
        }
        for (const auto& link : copiedLinks)
        {
            if (!candidate.Connect(remappedPins.at(link.SourcePin), remappedPins.at(link.TargetPin), document.Error))
            {
                return;
            }
        }
        PushHistory(document, document.Graph);
        document.Graph = std::move(candidate);
        document.RestorePositions = true;
        document.PendingSelection = std::move(selection);
    }

    void TrackPositions(GraphDocument& document)
    {
        bool moved = false;
        for (const auto& node : document.Graph.Nodes)
        {
            const auto position = Nodes::GetNodePosition(Nodes::NodeId(node.Id));
            moved |= std::abs(position.x - node.Position[0]) > 0.1f || std::abs(position.y - node.Position[1]) > 0.1f;
        }
        if (moved && !document.PendingMove)
        {
            document.PendingMove = document.Graph;
        }
        if (moved)
        {
            for (auto& node : document.Graph.Nodes)
            {
                const auto position = Nodes::GetNodePosition(Nodes::NodeId(node.Id));
                node.Position = {position.x, position.y};
            }
            document.Dirty = true;
            document.RecoveryDirty = true;
        }
        if (document.PendingMove && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            FinishMove(document);
        }
    }

    void DrawCanvas(GraphDocument& document)
    {
        std::optional<std::pair<uint64_t, uint64_t>> connection;
        Nodes::Begin("MaterialGraphCanvas");
        if (document.RestorePositions)
        {
            RestorePositions(document);
        }
        for (auto& node : document.Graph.Nodes)
        {
            DrawNode(document, node);
        }
        if (!document.PendingSelection.empty())
        {
            Nodes::ClearSelection();
            for (const auto id : document.PendingSelection)
            {
                Nodes::SelectNode(Nodes::NodeId(id), true);
            }
            document.PendingSelection.clear();
        }
        for (const auto& link : document.Graph.Links)
        {
            Nodes::Link(Nodes::LinkId(link.Id), Nodes::PinId(link.SourcePin), Nodes::PinId(link.TargetPin));
        }
        if (Nodes::BeginCreate())
        {
            Nodes::PinId first;
            Nodes::PinId second;
            if (Nodes::QueryNewLink(&first, &second) && first && second)
            {
                uint64_t source = first.Get();
                uint64_t target = second.Get();
                bool input = false;
                document.Graph.FindPinNode(source, &input);
                if (input)
                {
                    std::swap(source, target);
                }
                std::string error;
                if (document.Graph.CanConnect(source, target, error))
                {
                    if (Nodes::AcceptNewItem())
                    {
                        const auto before = document.Graph;
                        if (document.Graph.Connect(source, target, document.Error))
                        {
                            PushHistory(document, before);
                        }
                    }
                }
                else
                {
                    Nodes::RejectNewItem(ImVec4(0.9f, 0.25f, 0.2f, 1));
                    document.Error = error;
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
                    const auto before = document.Graph;
                    if (document.Graph.RemoveLink(link.Get()))
                    {
                        PushHistory(document, before);
                    }
                }
            }
            Nodes::NodeId node;
            while (Nodes::QueryDeletedNode(&node))
            {
                const auto* value = document.Graph.FindNode(node.Get());
                if (value && value->Kind != Kind::Output)
                {
                    if (Nodes::AcceptDeletedItem())
                    {
                        const auto before = document.Graph;
                        document.Graph.RemoveNode(node.Get());
                        PushHistory(document, before);
                    }
                }
                else
                {
                    Nodes::RejectDeletedItem();
                }
            }
        }
        Nodes::EndDelete();
        if (Nodes::BeginShortcut())
        {
            if (Nodes::AcceptCopy())
            {
                CopySelection(document);
            }
            if (Nodes::AcceptPaste())
            {
                PasteSelection(document);
            }
            if (Nodes::AcceptDuplicate())
            {
                CopySelection(document);
                PasteSelection(document);
            }
        }
        Nodes::EndShortcut();
        Nodes::NodeId context;
        const bool nodeMenu = Nodes::ShowNodeContextMenu(&context);
        const bool backgroundMenu = Nodes::ShowBackgroundContextMenu();
        Nodes::Suspend();
        if (backgroundMenu)
        {
            document.AddPosition = Nodes::ScreenToCanvas(ImGui::GetMousePos());
            document.Search[0] = '\0';
            ImGui::OpenPopup("AddMaterialNode");
        }
        if (nodeMenu)
        {
            document.ContextNode = context.Get();
            if (const auto* node = document.Graph.FindNode(document.ContextNode))
            {
                snprintf(document.Rename, sizeof(document.Rename), "%s", node->Name.c_str());
                ImGui::OpenPopup("MaterialNodeMenu");
            }
        }
        if (ImGui::BeginPopup("AddMaterialNode"))
        {
            ImGui::InputTextWithHint("##Search", "ノードを検索", document.Search, sizeof(document.Search));
            for (const auto& definition : MaterialGraph::Definitions())
            {
                ImGuiTextFilter filter(document.Search);
                if (definition.Kind == Kind::Output || !filter.PassFilter(definition.Name))
                {
                    continue;
                }
                if (ImGui::MenuItem(definition.Name))
                {
                    const auto before = document.Graph;
                    const auto id =
                        document.Graph.AddNode(definition.Kind, document.AddPosition.x, document.AddPosition.y);
                    if (id)
                    {
                        PushHistory(document, before);
                        document.RestorePositions = true;
                    }
                }
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("MaterialNodeMenu"))
        {
            if (ImGui::InputText("名前", document.Rename, sizeof(document.Rename),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (auto* node = document.Graph.FindNode(document.ContextNode))
                {
                    PushHistory(document, document.Graph);
                    node->Name = document.Rename;
                }
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("コピー"))
            {
                Nodes::SelectNode(Nodes::NodeId(document.ContextNode));
                CopySelection(document);
            }
            const auto* sourceNode = document.Graph.FindNode(document.ContextNode);
            if (sourceNode && sourceNode->OutputPin && ImGui::BeginMenu("出力の接続先"))
            {
                const auto sourcePin = sourceNode->OutputPin;
                for (const auto& target : document.Graph.Nodes)
                {
                    const auto& definition = MaterialGraph::Definition(target.Kind);
                    for (size_t i = 0; i < target.InputPins.size(); ++i)
                    {
                        const auto pin = target.InputPins[i];
                        std::string error;
                        const bool compatible = document.Graph.CanConnect(sourcePin, pin, error);
                        const auto label = target.Name + " / " + definition.Inputs[i].Name;
                        ImGui::PushID(std::to_string(pin).c_str());
                        if (ImGui::MenuItem(label.c_str(), nullptr, false, compatible))
                        {
                            connection = std::make_pair(sourcePin, pin);
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("削除"))
            {
                const auto before = document.Graph;
                if (document.Graph.RemoveNode(document.ContextNode))
                {
                    PushHistory(document, before);
                }
            }
            ImGui::EndPopup();
        }
        if (connection)
        {
            const auto before = document.Graph;
            if (document.Graph.Connect(connection->first, connection->second, document.Error))
            {
                PushHistory(document, before);
            }
        }
        Nodes::Resume();
        Nodes::End();
        if (document.FramesUntilFit > 0 && --document.FramesUntilFit == 0)
        {
            Nodes::NavigateToContent(0);
        }
        if (!document.RestorePositions)
        {
            TrackPositions(document);
        }
    }
}

void MaterialGraphEditor::BeginFrame()
{
    focusedMaterial = 0;
    for (auto& [handle, document] : documents)
    {
        if (document->ShaderJob.valid() &&
            document->ShaderJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto result = document->ShaderJob.get();
            if (result.Success())
            {
                document->Shader = std::move(result);
                document->Error.clear();
                if (document->CompilingRevision == document->Graph.Revision)
                {
                    document->Status = "DXCでシェーダーをコンパイルしました。";
                }
                else
                {
                    document->Status = "コンパイル中にグラフが変更されました。再コンパイルしてください。";
                }
            }
            else
            {
                document->Error = result.Error;
            }
        }
    }
}

void MaterialGraphEditor::Draw(AssetHandle material)
{
    auto& document = Document(material);
    ImGui::PushID(FormatAssetHandle(material).c_str());
    Nodes::SetCurrentEditor(document.Context);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    {
        focusedMaterial = material;
    }
    if (ImGui::Button("Save Graph"))
    {
        Save(material);
    }
    ContinueToolbar();
    ImGui::BeginDisabled(document.Undo.empty());
    if (ImGui::Button("Undo"))
    {
        focusedMaterial = material;
        Undo(false);
    }
    ImGui::EndDisabled();
    ContinueToolbar();
    ImGui::BeginDisabled(document.LoadFailed || document.ShaderJob.valid());
    if (ImGui::Button("Compile Graph"))
    {
        document.Program = MaterialGraphCompiler::Generate(document.Graph);
        document.Error = document.Program.Error;
        if (document.Program.Success)
        {
            document.CompilingRevision = document.Graph.Revision;
            document.ShaderJob = std::async(std::launch::async, [program = document.Program] {
                return MaterialGraphShaderCompiler::Compile(program);
            });
            document.Status = "シェーダーをコンパイル中...";
        }
    }
    ImGui::EndDisabled();
    ContinueToolbar();
    ImGui::BeginDisabled(document.Redo.empty());
    if (ImGui::Button("Redo"))
    {
        focusedMaterial = material;
        Undo(true);
    }
    ImGui::EndDisabled();
    ContinueToolbar();
    if (ImGui::Button("Frame All"))
    {
        Nodes::NavigateToContent();
    }
    ContinueToolbar();
    ImGui::TextUnformatted(document.Dirty ? "未保存" : "保存済み");
    ContinueToolbar();
    ImGui::BeginDisabled(document.LoadFailed);
    if (ImGui::Button("Generate HLSL"))
    {
        document.Program = MaterialGraphCompiler::Generate(document.Graph);
        document.Error = document.Program.Error;
        if (document.Program.Success)
        {
            auto path = document.RecoveryPath;
            path.replace_extension("hlsl");
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                document.Error = error.message();
            }
            else if (WriteAssetFile(path, document.Program.Hlsl, document.Error))
            {
                document.Status = "HLSLを生成しました: " + path.generic_string();
            }
        }
    }
    ImGui::EndDisabled();
    if (!document.Program.Hlsl.empty())
    {
        ContinueToolbar();
        if (ImGui::SmallButton("HLSL"))
        {
            ImGui::OpenPopup("Generated HLSL");
        }
    }
    ImGui::SetNextWindowSize(ImVec2(800, 480), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Generated HLSL", nullptr))
    {
        ImGui::InputTextMultiline("##HlslSource", document.Program.Hlsl.data(), document.Program.Hlsl.size() + 1,
                                  ImVec2(-1, -ImGui::GetFrameHeightWithSpacing()), ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("閉じる"))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!document.Error.empty())
    {
        ImGui::TextWrapped("%s", document.Error.c_str());
    }
    if (!document.Status.empty())
    {
        ImGui::TextWrapped("%s", document.Status.c_str());
    }
    if (!document.LoadFailed)
    {
        DrawCanvas(document);
    }
    else
    {
        ImGui::TextWrapped("グラフを読み込めませんでした。既存ファイルを保持しています: %s",
                           document.Path.string().c_str());
    }
    Nodes::SetCurrentEditor(nullptr);
    ImGui::PopID();
}

bool MaterialGraphEditor::HasFocus()
{
    return focusedMaterial != 0;
}

bool MaterialGraphEditor::HasUnsavedChanges(AssetHandle material)
{
    const auto found = documents.find(material);
    return found != documents.end() && found->second->Dirty;
}

bool MaterialGraphEditor::Save(AssetHandle material)
{
    const auto found = documents.find(material);
    if (found == documents.end())
    {
        return true;
    }
    auto& document = *found->second;
    FinishEdits(document);
    if (document.LoadFailed || !WriteDocument(document, document.Path))
    {
        return false;
    }
    document.SavedGraph = document.Graph;
    document.Dirty = false;
    document.RecoveryDirty = false;
    document.Status = "グラフを保存しました。";
    std::error_code ignored;
    std::filesystem::remove(document.RecoveryPath, ignored);
    return true;
}

const char* MaterialGraphEditor::LastError(AssetHandle material)
{
    const auto found = documents.find(material);
    if (found == documents.end())
    {
        return "";
    }
    return found->second->Error.c_str();
}

void MaterialGraphEditor::Revert(AssetHandle material)
{
    const auto found = documents.find(material);
    if (found == documents.end())
    {
        return;
    }
    auto& document = *found->second;
    FinishEdits(document);
    PushHistory(document, document.Graph);
    const auto revision = document.Graph.Revision;
    const auto parameterRevision = document.Graph.ParameterRevision;
    document.Graph = document.SavedGraph;
    document.Graph.Revision = revision + 1;
    document.Graph.ParameterRevision = parameterRevision + 1;
    document.RestorePositions = true;
    document.Dirty = false;
    document.RecoveryDirty = false;
    std::error_code ignored;
    std::filesystem::remove(document.RecoveryPath, ignored);
}

void MaterialGraphEditor::Undo(bool redo)
{
    const auto found = documents.find(focusedMaterial);
    if (found == documents.end())
    {
        return;
    }
    auto& document = *found->second;
    FinishEdits(document);
    auto* source = &document.Undo;
    auto* target = &document.Redo;
    if (redo)
    {
        std::swap(source, target);
    }
    if (source->empty())
    {
        return;
    }
    const auto revision = document.Graph.Revision;
    target->push_back(document.Graph);
    document.Graph = std::move(source->back());
    source->pop_back();
    document.Graph.Revision = revision + 1;
    document.RestorePositions = true;
    document.Dirty = document.Graph.Serialize() != document.SavedGraph.Serialize();
    document.RecoveryDirty = true;
    document.PendingEdit.reset();
    document.PendingMove.reset();
}

void MaterialGraphEditor::SaveRecovery()
{
    if (ImGui::GetTime() < nextRecoverySave || ImGui::IsAnyItemActive())
    {
        return;
    }
    nextRecoverySave = ImGui::GetTime() + 2.0;
    for (auto& [handle, document] : documents)
    {
        if (document->RecoveryDirty && !document->LoadFailed && WriteDocument(*document, document->RecoveryPath))
        {
            document->RecoveryDirty = false;
        }
    }
}

void MaterialGraphEditor::Shutdown()
{
    for (auto& [handle, document] : documents)
    {
        FinishEdits(*document);
        if (document->Dirty && !document->LoadFailed)
        {
            WriteDocument(*document, document->RecoveryPath);
        }
    }
    documents.clear();
    focusedMaterial = 0;
    nextRecoverySave = 0;
}
