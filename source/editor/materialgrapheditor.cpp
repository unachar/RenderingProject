#include "pch.h"
#include "materialgrapheditor.h"
#include "materialgraph.h"
#include "materialgraphcompiler.h"
#include "materialgraphshader.h"
#include "materialmanager.h"
#include "materialgraphpreview.h"
#include "materialgraphtextures.h"
#include "materialpreview.h"
#include "../../External/imgui-node-editor/imgui_node_editor.h"
#include <json/json.h>
#include <fstream>
#include <optional>
#include <future>
#include <chrono>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

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
        // プレビュー
        uint64_t SelectedNode = 0;
        // ノード整理 / コメント / 追加メニュー
        std::unordered_map<uint64_t, float> PinY; // ピン → キャンバス上の高さ（このフレーム）
        int PendingArrange = 0;
        uint64_t PendingConnectPin = 0;
        bool OpenAddMenu = false;
        uint64_t EditingComment = 0;
        char CommentBuffer[512]{};
        std::vector<uint64_t> NewGroups;
        // マテリアルアセットへの反映
        bool AutoApply = true;
        bool ApplyInitialized = false;
        uint64_t AppliedSignature = 0;
        uint32_t ManagedSlots = 0;
        std::vector<std::string> ApplyNotes;

        ~GraphDocument()
        {
            MaterialGraphPreview::Forget(Graph);
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
    bool showNodePreviews = true; // ノードプレビューの表示（ツールバーで切り替え）

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

    // ============================================================ 見た目（UE のマテリアルエディター風）
    struct CategoryInfo
    {
        const char* Name;
        ImU32 Color;
    };

    CategoryInfo Category(Kind kind)
    {
        switch (kind)
        {
            case Kind::Output:
                return {"Output", IM_COL32(72, 72, 78, 255)};
            case Kind::Constant:
            case Kind::Constant2Vector:
            case Kind::Constant3Vector:
            case Kind::Constant4Vector:
            case Kind::Time:
                return {"Constants", IM_COL32(56, 118, 70, 255)};
            case Kind::ScalarParameter:
            case Kind::VectorParameter:
            case Kind::ColorParameter:
                return {"Parameters", IM_COL32(42, 140, 96, 255)};
            case Kind::TextureSample:
                return {"Texture", IM_COL32(48, 92, 168, 255)};
            case Kind::UV:
            case Kind::TextureCoordinate:
            case Kind::Panner:
            case Kind::Rotator:
            case Kind::WorldPosition:
            case Kind::WorldNormal:
            case Kind::CameraVector:
            case Kind::ReflectionVector:
                return {"Coordinates", IM_COL32(150, 62, 62, 255)};
            case Kind::Normalize:
            case Kind::Dot:
            case Kind::CrossProduct:
            case Kind::Distance:
            case Kind::Length:
            case Kind::AppendVector:
            case Kind::ComponentMask:
                return {"Vector", IM_COL32(108, 78, 146, 255)};
            case Kind::Fresnel:
            case Kind::Desaturation:
            case Kind::If:
            case Kind::SphereMask:
            case Kind::CheapContrast:
            case Kind::Noise:
            case Kind::Reroute:
                return {"Utility", IM_COL32(140, 104, 40, 255)};
            case Kind::Group:
                return {"Comment", IM_COL32(80, 100, 130, 255)};
            default:
                return {"Math", IM_COL32(64, 70, 80, 255)};
        }
    }

    ImU32 TypeColor(MaterialGraphType type, float alpha = 1.0f)
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

    const char* TypeName(MaterialGraphType type)
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

    float NodeWidth(const MaterialGraphNode& node)
    {
        if (node.Width > 0.0f)
        {
            return std::clamp(node.Width, 96.0f, 512.0f);
        }
        return node.Kind == Kind::Output ? 168.0f : 128.0f;
    }

    ImVec4 CommentColor(const MaterialGraphNode& node)
    {
        const bool unset = node.Value[0] == 0.0f && node.Value[1] == 0.0f && node.Value[2] == 0.0f;
        return unset ? ImVec4(0.30f, 0.42f, 0.60f, 1.0f) : ImVec4(node.Value[0], node.Value[1], node.Value[2], 1.0f);
    }

    // 1 フレーム分の描画情報（接続状態・型・強調）
    struct FrameInfo
    {
        std::unordered_map<uint64_t, const MaterialGraphNode*> PinOwner;
        std::unordered_map<uint64_t, uint64_t> InputSource; // 入力ピン → 接続元のピン
        std::unordered_set<uint64_t> ConnectedPins;
        std::unordered_map<uint64_t, MaterialGraphType> OutputTypes;
        std::unordered_set<uint64_t> Selected;
        uint64_t Hovered = 0;
    };

    FrameInfo BuildFrameInfo(const MaterialGraph& graph)
    {
        FrameInfo info;
        info.PinOwner.reserve(graph.Nodes.size() * 3);
        for (const auto& node : graph.Nodes)
        {
            if (node.OutputPin)
            {
                info.PinOwner[node.OutputPin] = &node;
                info.OutputTypes[node.Id] = graph.ResolveOutputType(node.Id);
            }
            for (const auto pin : node.InputPins)
            {
                info.PinOwner[pin] = &node;
            }
        }
        for (const auto& link : graph.Links)
        {
            info.ConnectedPins.insert(link.SourcePin);
            info.ConnectedPins.insert(link.TargetPin);
            info.InputSource[link.TargetPin] = link.SourcePin;
        }
        const int selectedCount = Nodes::GetSelectedObjectCount();
        if (selectedCount > 0)
        {
            std::vector<Nodes::NodeId> selected(static_cast<size_t>(selectedCount));
            const int count = Nodes::GetSelectedNodes(selected.data(), selectedCount);
            for (int i = 0; i < count; ++i)
            {
                info.Selected.insert(static_cast<uint64_t>(selected[i].Get()));
            }
        }
        info.Hovered = static_cast<uint64_t>(Nodes::GetHoveredNode().Get());
        return info;
    }

    // UE と同じ：未接続は中抜きの円、接続済みは塗りつぶしの円
    void PinIcon(ImU32 color, bool connected)
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

    float PinCenterY()
    {
        return (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
    }

    void ValidateAfterEdit(GraphDocument& document, MaterialGraphNode& node, const std::array<float, 4>& before)
    {
        std::string error;
        if (!document.Graph.Validate(error))
        {
            node.Value = before; // 接続と矛盾する変更（成分数など）は取り消す
            document.Error = "変更できません: " + error;
        }
        else
        {
            ++document.Graph.Revision;
        }
    }

    void DrawValueEditors(GraphDocument& document, MaterialGraphNode& node, float width)
    {
        const auto before = node.Value;
        bool changed = false;
        ImGui::SetNextItemWidth(width);
        switch (node.Kind)
        {
            case Kind::Constant:
            case Kind::ScalarParameter:
                changed = ImGui::DragFloat("##Value", node.Value.data(), 0.01f);
                TrackValueEdit(document, node, before, changed);
                break;
            case Kind::Constant2Vector:
                changed = ImGui::DragFloat2("##Vector2", node.Value.data(), 0.01f);
                TrackValueEdit(document, node, before, changed);
                break;
            case Kind::VectorParameter:
                changed = ImGui::DragFloat3("##Vector", node.Value.data(), 0.01f);
                TrackValueEdit(document, node, before, changed);
                break;
            case Kind::Constant3Vector:
                changed = ImGui::ColorEdit3("##Color3", node.Value.data(),
                                            ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                TrackValueEdit(document, node, before, changed);
                break;
            case Kind::ColorParameter:
                changed = ImGui::ColorEdit4("##Color", node.Value.data(), ImGuiColorEditFlags_Float);
                TrackValueEdit(document, node, before, changed);
                break;
            case Kind::Constant4Vector:
                changed = ImGui::ColorEdit4("##Color4", node.Value.data(),
                                            ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                TrackValueEdit(document, node, before, changed);
                break;
            case Kind::TextureCoordinate:
                changed = ImGui::DragFloat2("Tiling", node.Value.data(), 0.01f);
                TrackValueEdit(document, node, before, changed);
                break;
            case Kind::ComponentMask: {
                static const char* labels[] = {"R", "G", "B", "A"};
                for (int i = 0; i < 4; ++i)
                {
                    if (i > 0)
                    {
                        ImGui::SameLine(0.0f, 2.0f);
                    }
                    bool enabled = node.Value[i] > 0.5f;
                    if (ImGui::Checkbox(labels[i], &enabled))
                    {
                        PushHistory(document, document.Graph);
                        node.Value[i] = enabled ? 1.0f : 0.0f;
                        ValidateAfterEdit(document, node, before);
                    }
                }
                break;
            }
            case Kind::TextureSample: {
                auto path = MaterialManager::Registry().Resolve(node.Texture).filename().string();
                if (path.empty())
                {
                    path = "Textureをドロップ";
                }
                ImGui::Button(path.c_str(), ImVec2(width, 0));
                ImGui::SetItemTooltip("Content Browser からテクスチャをドロップ");
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
                break;
            }
            default:
                break;
        }
    }

    float NodeHeight(const MaterialGraphNode& node)
    {
        return node.Height > 0.0f ? std::clamp(node.Height, 48.0f, 2048.0f) : 0.0f;
    }

    // ノードのサイズ変更：右端 = 幅のみ、下端 = 高さのみ、右下の角 = 両方（UE / Blender と同じ）
    //  ダブルクリックでその方向を既定（自動）に戻す
    void DrawResizeHandles(GraphDocument& document, MaterialGraphNode& node, ImVec2 contentMin, float contentBottom)
    {
        enum Axis
        {
            Horizontal = 1,
            Vertical = 2,
        };
        const float edge = 6.0f;
        const float corner = 12.0f;
        const float width = NodeWidth(node);
        const float headerSkip = ImGui::GetFrameHeight(); // 右上の出力ピンと重ならないように
        const ImVec2 savedCursor = ImGui::GetCursorScreenPos();

        struct Handle
        {
            const char* Id;
            ImVec2 Min;
            ImVec2 Size;
            int Axes;
            ImGuiMouseCursor Cursor;
            const char* Tooltip;
        };
        const Handle handles[] = {
            {"##ResizeRight", ImVec2(contentMin.x + width - edge, contentMin.y + headerSkip),
             ImVec2(edge, (std::max)(contentBottom - contentMin.y - headerSkip - corner, 1.0f)), Horizontal,
             ImGuiMouseCursor_ResizeEW, "ドラッグ: 幅を変更 / ダブルクリック: 既定の幅"},
            {"##ResizeBottom", ImVec2(contentMin.x, contentBottom - edge),
             ImVec2((std::max)(width - corner, 1.0f), edge), Vertical, ImGuiMouseCursor_ResizeNS,
             "ドラッグ: 高さを変更 / ダブルクリック: 内容に合わせる"},
            {"##ResizeCorner", ImVec2(contentMin.x + width - corner, contentBottom - corner), ImVec2(corner, corner),
             Horizontal | Vertical, ImGuiMouseCursor_ResizeNWSE,
             "ドラッグ: 幅と高さを変更 / ダブルクリック: 既定のサイズ"},
        };
        bool cornerHot = false;
        for (const auto& handle : handles)
        {
            ImGui::SetCursorScreenPos(handle.Min);
            ImGui::InvisibleButton(handle.Id, handle.Size);
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (hot)
            {
                ImGui::SetMouseCursor(handle.Cursor);
            }
            if (handle.Axes == (Horizontal | Vertical))
            {
                cornerHot = hot;
            }
            if (ImGui::IsItemActivated() && !document.PendingMove)
            {
                document.PendingMove = document.Graph;
                // 高さが自動のときは、現在の見た目の高さから変更を始める
                if ((handle.Axes & Vertical) && node.Height <= 0.0f)
                {
                    node.Height = contentBottom - contentMin.y;
                }
            }
            if (ImGui::IsItemActive())
            {
                const ImVec2 mouse = ImGui::GetMousePos();
                if (handle.Axes & Horizontal)
                {
                    const float newWidth = std::clamp(mouse.x - contentMin.x, 96.0f, 512.0f);
                    if (fabsf(newWidth - node.Width) > 0.5f)
                    {
                        node.Width = newWidth;
                        document.Dirty = true;
                        document.RecoveryDirty = true;
                    }
                }
                if (handle.Axes & Vertical)
                {
                    const float newHeight = std::clamp(mouse.y - contentMin.y, 48.0f, 2048.0f);
                    if (fabsf(newHeight - node.Height) > 0.5f)
                    {
                        node.Height = newHeight;
                        document.Dirty = true;
                        document.RecoveryDirty = true;
                    }
                }
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                PushHistory(document, document.Graph);
                if (handle.Axes & Horizontal)
                {
                    node.Width = 0.0f;
                }
                if (handle.Axes & Vertical)
                {
                    node.Height = 0.0f;
                }
            }
            ImGui::SetItemTooltip("%s", handle.Tooltip);
            if (hot && handle.Axes != (Horizontal | Vertical))
            {
                // 辺のハイライト
                ImGui::GetWindowDrawList()->AddRectFilled(
                    handle.Axes == Horizontal ? ImVec2(handle.Min.x + edge - 2.0f, handle.Min.y) : ImVec2(handle.Min.x, handle.Min.y + edge - 2.0f),
                    ImVec2(handle.Min.x + handle.Size.x, handle.Min.y + handle.Size.y), IM_COL32(255, 190, 60, 200));
            }
        }
        // 右下のグリップ表示
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 max(contentMin.x + width, contentBottom);
        const ImU32 color = cornerHot ? IM_COL32(255, 190, 60, 255) : IM_COL32(150, 150, 150, 160);
        for (int i = 1; i <= 3; ++i)
        {
            const float offset = i * 3.5f;
            draw->AddLine(ImVec2(max.x - offset, max.y - 1.0f), ImVec2(max.x - 1.0f, max.y - offset), color, 1.2f);
        }
        ImGui::SetCursorScreenPos(savedCursor);
        ImGui::Dummy(ImVec2(0.0f, 0.0f)); // カーソル移動の後にアイテムを置いて境界を確定（ImGui 1.92 の検証対策）
    }

    void DrawNodeComment(const MaterialGraphNode& node)
    {
        // UE のノードコメント：ノードの上に吹き出しで表示
        if (node.Comment.empty())
        {
            return;
        }
        const ImVec2 position = Nodes::GetNodePosition(Nodes::NodeId(node.Id));
        const ImVec2 size = Nodes::GetNodeSize(Nodes::NodeId(node.Id));
        const float wrap = (std::max)(size.x, 120.0f);
        const ImVec2 text = ImGui::CalcTextSize(node.Comment.c_str(), nullptr, false, wrap - 12.0f);
        const ImVec2 max(position.x + (std::min)(text.x + 12.0f, wrap), position.y - 6.0f);
        const ImVec2 min(position.x, max.y - text.y - 8.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, max, IM_COL32(240, 240, 220, 235), 4.0f);
        draw->AddTriangleFilled(ImVec2(min.x + 10.0f, max.y), ImVec2(min.x + 20.0f, max.y),
                                ImVec2(min.x + 12.0f, max.y + 5.0f), IM_COL32(240, 240, 220, 235));
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + 6.0f, min.y + 4.0f),
                      IM_COL32(30, 30, 30, 255), node.Comment.c_str(), nullptr, wrap - 12.0f);
    }

    void DrawCommentGroup(GraphDocument& document, MaterialGraphNode& node)
    {
        const ImVec4 color = CommentColor(node);
        Nodes::PushStyleColor(Nodes::StyleColor_NodeBg, ImVec4(color.x, color.y, color.z, 0.22f));
        Nodes::PushStyleColor(Nodes::StyleColor_NodeBorder, ImVec4(color.x, color.y, color.z, 0.85f));
        Nodes::PushStyleColor(Nodes::StyleColor_GroupBg, ImVec4(color.x, color.y, color.z, 0.22f));
        Nodes::PushStyleColor(Nodes::StyleColor_GroupBorder, ImVec4(color.x, color.y, color.z, 0.85f));
        Nodes::BeginNode(Nodes::NodeId(node.Id));
        ImGui::PushID(std::to_string(node.Id).c_str());
        if (document.EditingComment == node.Id)
        {
            ImGui::SetNextItemWidth((std::max)(node.GroupSize[0] - 16.0f, 120.0f));
            if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive())
            {
                ImGui::SetKeyboardFocusHere();
            }
            if (ImGui::InputText("##CommentTitle", document.CommentBuffer, sizeof(document.CommentBuffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            {
                PushHistory(document, document.Graph);
                node.Name = document.CommentBuffer;
                document.EditingComment = 0;
            }
            if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit())
            {
                document.EditingComment = 0;
            }
        }
        else
        {
            ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.3f);
            ImGui::TextUnformatted(node.Name.c_str());
            ImGui::PopFont();
        }
        Nodes::Group(ImVec2(node.GroupSize[0], node.GroupSize[1]));
        ImGui::PopID();
        Nodes::EndNode();
        Nodes::PopStyleColor(4);

        // ズームアウト時も読めるように、タイトルを大きく表示（UE と同じ）
        if (Nodes::BeginGroupHint(Nodes::NodeId(node.Id)))
        {
            const float alpha = std::clamp((Nodes::GetCurrentZoom() - 1.2f) * 2.0f, 0.0f, 1.0f);
            if (alpha > 0.01f)
            {
                const ImVec2 min = Nodes::GetGroupMin();
                ImDrawList* draw = Nodes::GetHintForegroundDrawList();
                const float size = ImGui::GetFontSize() * 1.6f;
                draw->AddText(ImGui::GetFont(), size, ImVec2(min.x + 8.0f, min.y - size - 6.0f),
                              ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)), node.Name.c_str());
            }
        }
        Nodes::EndGroupHint();
    }

    void DrawRerouteNode(GraphDocument& document, MaterialGraphNode& node, const FrameInfo& frame)
    {
        // 中継ノード（UE の Reroute）：ピンだけの小さなノード
        const auto found = frame.OutputTypes.find(node.Id);
        const ImU32 color = TypeColor(found != frame.OutputTypes.end() ? found->second : MaterialGraphType::Numeric);
        Nodes::PushStyleVar(Nodes::StyleVar_NodePadding, ImVec4(3, 2, 3, 2));
        Nodes::PushStyleVar(Nodes::StyleVar_NodeRounding, 8.0f);
        Nodes::BeginNode(Nodes::NodeId(node.Id));
        ImGui::PushID(std::to_string(node.Id).c_str());
        Nodes::BeginPin(Nodes::PinId(node.InputPins[0]), Nodes::PinKind::Input);
        Nodes::PinPivotAlignment(ImVec2(0.0f, 0.5f));
        Nodes::PinPivotSize(ImVec2(0, 0));
        PinIcon(color, frame.ConnectedPins.contains(node.InputPins[0]));
        document.PinY[node.InputPins[0]] = PinCenterY();
        Nodes::EndPin();
        ImGui::SameLine(0.0f, 1.0f);
        Nodes::BeginPin(Nodes::PinId(node.OutputPin), Nodes::PinKind::Output);
        Nodes::PinPivotAlignment(ImVec2(1.0f, 0.5f));
        Nodes::PinPivotSize(ImVec2(0, 0));
        PinIcon(color, frame.ConnectedPins.contains(node.OutputPin));
        document.PinY[node.OutputPin] = PinCenterY();
        Nodes::EndPin();
        ImGui::PopID();
        Nodes::EndNode();
        Nodes::PopStyleVar(2);
    }

    void DrawNode(GraphDocument& document, MaterialGraphNode& node, const FrameInfo& frame)
    {
        if (node.Kind == Kind::Group)
        {
            DrawCommentGroup(document, node);
            return;
        }
        if (node.Kind == Kind::Reroute)
        {
            DrawRerouteNode(document, node, frame);
            return;
        }
        const auto& definition = MaterialGraph::Definition(node.Kind);
        const float width = NodeWidth(node);
        const auto id = Nodes::NodeId(node.Id);
        Nodes::BeginNode(id);
        ImGui::PushID(std::to_string(node.Id).c_str());
        const ImVec2 start = ImGui::GetCursorScreenPos();

        // ---- ヘッダー：プレビュー切替 / タイトル / 出力ピン（UE と同じく右上）
        const bool hasPreview = showNodePreviews && MaterialGraphPreview::HasPreview(node);
        const bool previewCollapsed = MaterialGraphPreview::IsNodePreviewCollapsed(document.Graph, node.Id);
        if (hasPreview)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::SmallButton(previewCollapsed ? "▸" : "▾"))
            {
                MaterialGraphPreview::ToggleNodePreview(document.Graph, node.Id);
            }
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::SetItemTooltip(previewCollapsed ? "プレビューを表示" : "プレビューを隠す");
            ImGui::SameLine(0.0f, 4.0f);
        }
        ImGui::TextUnformatted(node.Name.c_str());
        if (node.Name != definition.Name)
        {
            ImGui::SetItemTooltip("%s", definition.Name);
        }
        float headerBottom = ImGui::GetItemRectMax().y;
        if (node.OutputPin)
        {
            const auto typeFound = frame.OutputTypes.find(node.Id);
            const auto type = typeFound != frame.OutputTypes.end() ? typeFound->second : MaterialGraphType::Numeric;
            ImGui::SameLine(0.0f, 0.0f);
            const float iconWidth = ImGui::GetTextLineHeight() * 0.72f;
            ImGui::SetCursorScreenPos(
                ImVec2((std::max)(start.x + width - iconWidth, ImGui::GetCursorScreenPos().x + 6.0f), start.y));
            Nodes::BeginPin(Nodes::PinId(node.OutputPin), Nodes::PinKind::Output);
            Nodes::PinPivotAlignment(ImVec2(1.0f, 0.5f));
            Nodes::PinPivotSize(ImVec2(0, 0));
            PinIcon(TypeColor(type), frame.ConnectedPins.contains(node.OutputPin));
            document.PinY[node.OutputPin] = PinCenterY();
            Nodes::EndPin();
            ImGui::SetItemTooltip("出力: %s", TypeName(type));
            headerBottom = (std::max)(headerBottom, ImGui::GetItemRectMax().y);
        }
        ImGui::Dummy(ImVec2(width, 4.0f)); // ヘッダーの下の余白（ノード幅もここで確定）

        // ---- 値の編集
        DrawValueEditors(document, node, width);

        // ---- 入力ピン
        for (size_t i = 0; i < node.InputPins.size(); ++i)
        {
            const uint64_t pin = node.InputPins[i];
            const bool connected = frame.ConnectedPins.contains(pin);
            MaterialGraphType type = definition.Inputs[i].Type;
            if (connected)
            {
                const auto source = frame.InputSource.find(pin);
                if (source != frame.InputSource.end())
                {
                    const auto owner = frame.PinOwner.find(source->second);
                    if (owner != frame.PinOwner.end())
                    {
                        const auto found = frame.OutputTypes.find(owner->second->Id);
                        type = found != frame.OutputTypes.end() ? found->second : type;
                    }
                }
            }
            Nodes::BeginPin(Nodes::PinId(pin), Nodes::PinKind::Input);
            Nodes::PinPivotAlignment(ImVec2(0.0f, 0.5f));
            Nodes::PinPivotSize(ImVec2(0, 0));
            PinIcon(TypeColor(type), connected);
            document.PinY[pin] = PinCenterY();
            ImGui::SameLine(0.0f, 4.0f);
            if (connected)
            {
                ImGui::TextUnformatted(definition.Inputs[i].Name);
            }
            else
            {
                ImGui::TextDisabled("%s", definition.Inputs[i].Name);
            }
            Nodes::EndPin();
        }

        // ---- プレビュー（高さを指定している場合は、残りの高さをプレビュー / 余白に使う）
        const float fixedHeight = NodeHeight(node);
        const float used = ImGui::GetCursorScreenPos().y - start.y;
        if (hasPreview && !previewCollapsed)
        {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            const float previewHeight = fixedHeight > 0.0f ? (std::max)(fixedHeight - used - 8.0f, 24.0f) : width;
            MaterialGraphPreview::DrawNodePreview(document.Graph, node, width, previewHeight);
        }
        else
        {
            const float pad = fixedHeight > 0.0f ? (std::max)(fixedHeight - used, 6.0f) : 6.0f;
            ImGui::Dummy(ImVec2(width, pad));
        }
        DrawResizeHandles(document, node, start, ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y);
        ImGui::PopID();
        Nodes::EndNode();

        // ---- ヘッダーの色帯（ノード背景の上、内容の下に描く）
        const ImVec2 position = Nodes::GetNodePosition(id);
        const ImVec2 size = Nodes::GetNodeSize(id);
        if (size.x > 0.0f)
        {
            const auto& style = Nodes::GetStyle();
            ImDrawList* background = Nodes::GetNodeBackgroundDrawList(id);
            const float inset = style.NodeBorderWidth * 0.5f;
            const ImVec2 headerMin(position.x + inset, position.y + inset);
            const ImVec2 headerMax(position.x + size.x - inset, headerBottom + 3.0f);
            const ImU32 color = Category(node.Kind).Color;
            background->AddRectFilled(headerMin, headerMax, color, style.NodeRounding, ImDrawFlags_RoundCornersTop);
            // 下に向かって少し暗くする（UE のヘッダーのグラデーション）
            background->AddRectFilledMultiColor(ImVec2(headerMin.x, (headerMin.y + headerMax.y) * 0.5f), headerMax,
                                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 70),
                                                IM_COL32(0, 0, 0, 70));
            background->AddLine(ImVec2(headerMin.x, headerMax.y), headerMax, IM_COL32(0, 0, 0, 140), 1.0f);
        }
        DrawNodeComment(node);
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
            destination->Width = source.Width;
            destination->Height = source.Height;
            destination->Comment = source.Comment;
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

    // ============================================================ ノード整理（UE の Alignment / Straighten / Comment）
    enum class ArrangeOp
    {
        None,
        AlignTop,
        AlignMiddle,
        AlignBottom,
        AlignLeft,
        AlignCenter,
        AlignRight,
        DistributeHorizontal,
        DistributeVertical,
        Straighten,
        AutoLayout,
        CommentSelection,
    };

    struct NodeRect
    {
        MaterialGraphNode* Node;
        ImVec2 Position;
        ImVec2 Size;
    };

    std::vector<NodeRect> SelectedRects(GraphDocument& document, bool includeGroups = false)
    {
        std::vector<NodeRect> rects;
        for (auto& node : document.Graph.Nodes)
        {
            const auto id = Nodes::NodeId(node.Id);
            if ((!includeGroups && node.Kind == Kind::Group) || !Nodes::IsNodeSelected(id))
            {
                continue;
            }
            rects.push_back({&node, Nodes::GetNodePosition(id), Nodes::GetNodeSize(id)});
        }
        return rects;
    }

    void MoveNode(MaterialGraphNode& node, ImVec2 position)
    {
        position.x = roundf(position.x);
        position.y = roundf(position.y);
        Nodes::SetNodePosition(Nodes::NodeId(node.Id), position);
        node.Position = {position.x, position.y};
    }

    void AutoLayout(GraphDocument& document)
    {
        // Material Output から上流へ層（深さ）を求め、右から左へ列に並べる
        auto& graph = document.Graph;
        const MaterialGraphNode* output = nullptr;
        for (const auto& node : graph.Nodes)
        {
            if (node.Kind == Kind::Output)
            {
                output = &node;
            }
        }
        if (!output)
        {
            return;
        }
        std::unordered_map<uint64_t, int> depth;
        depth[output->Id] = 0;
        // 最長経路で深さを決める（グラフは非巡回）
        bool changed = true;
        for (int iteration = 0; changed && iteration < 512; ++iteration)
        {
            changed = false;
            for (const auto& link : graph.Links)
            {
                const auto* target = graph.FindPinNode(link.TargetPin);
                const auto* source = graph.FindPinNode(link.SourcePin);
                if (!target || !source)
                {
                    continue;
                }
                const auto found = depth.find(target->Id);
                if (found == depth.end())
                {
                    continue;
                }
                const int candidate = found->second + 1;
                auto& current = depth[source->Id];
                if (current < candidate)
                {
                    current = candidate;
                    changed = true;
                }
            }
        }
        int maxDepth = 0;
        for (const auto& [id, d] : depth)
        {
            maxDepth = (std::max)(maxDepth, d);
        }
        std::vector<std::vector<MaterialGraphNode*>> layers(maxDepth + 1);
        for (auto& node : graph.Nodes)
        {
            const auto found = depth.find(node.Id);
            if (found != depth.end())
            {
                layers[found->second].push_back(&node);
            }
        }
        // 各層の並び：接続先のピンの高さ順（交差を減らす）
        std::unordered_map<uint64_t, float> pinOrder;
        const ImVec2 outputPosition = Nodes::GetNodePosition(Nodes::NodeId(output->Id));
        const ImVec2 outputSize = Nodes::GetNodeSize(Nodes::NodeId(output->Id));
        float columnRight = outputPosition.x - 90.0f;
        const float centerY = outputPosition.y + outputSize.y * 0.5f;
        for (int layer = 1; layer <= maxDepth; ++layer)
        {
            auto& nodes = layers[layer];
            auto key = [&](const MaterialGraphNode* node) {
                float best = 1e9f;
                for (const auto& link : graph.Links)
                {
                    if (link.SourcePin == node->OutputPin)
                    {
                        const auto found = document.PinY.find(link.TargetPin);
                        if (found != document.PinY.end())
                        {
                            best = (std::min)(best, found->second);
                        }
                    }
                }
                return best;
            };
            std::stable_sort(nodes.begin(), nodes.end(),
                             [&](const auto* a, const auto* b) { return key(a) < key(b); });
            float columnWidth = 0.0f;
            float totalHeight = 0.0f;
            for (const auto* node : nodes)
            {
                const ImVec2 size = Nodes::GetNodeSize(Nodes::NodeId(node->Id));
                columnWidth = (std::max)(columnWidth, size.x);
                totalHeight += size.y + 32.0f;
            }
            float y = centerY - totalHeight * 0.5f;
            for (auto* node : nodes)
            {
                const ImVec2 size = Nodes::GetNodeSize(Nodes::NodeId(node->Id));
                MoveNode(*node, ImVec2(columnRight - size.x, y));
                y += size.y + 32.0f;
            }
            columnRight -= columnWidth + 90.0f;
        }
    }

    void ApplyArrange(GraphDocument& document, ArrangeOp op)
    {
        if (op == ArrangeOp::None)
        {
            return;
        }
        const auto before = document.Graph;
        bool applied = false;
        if (op == ArrangeOp::AutoLayout)
        {
            AutoLayout(document);
            applied = true;
        }
        else if (op == ArrangeOp::CommentSelection)
        {
            // UE の C キー：選択ノードを囲むコメントを作る（未選択ならマウス位置に作る）
            auto rects = SelectedRects(document);
            ImVec2 min(FLT_MAX, FLT_MAX);
            ImVec2 max(-FLT_MAX, -FLT_MAX);
            for (const auto& rect : rects)
            {
                min = ImVec2((std::min)(min.x, rect.Position.x), (std::min)(min.y, rect.Position.y));
                max = ImVec2((std::max)(max.x, rect.Position.x + rect.Size.x),
                             (std::max)(max.y, rect.Position.y + rect.Size.y));
            }
            if (rects.empty())
            {
                min = Nodes::ScreenToCanvas(ImGui::GetMousePos());
                max = ImVec2(min.x + 320.0f, min.y + 200.0f);
            }
            const float pad = 24.0f;
            const auto id = document.Graph.AddNode(Kind::Group, min.x - pad, min.y - pad - 36.0f);
            if (auto* comment = document.Graph.FindNode(id))
            {
                comment->GroupSize = {max.x - min.x + pad * 2.0f, max.y - min.y + pad * 2.0f};
                document.NewGroups.push_back(id);
                document.RestorePositions = true;
                document.EditingComment = id;
                snprintf(document.CommentBuffer, sizeof(document.CommentBuffer), "%s", comment->Name.c_str());
                applied = true;
            }
        }
        else if (op == ArrangeOp::Straighten)
        {
            // UE の Q：選択ノードと接続先のピンの高さを揃える
            for (auto& rect : SelectedRects(document))
            {
                auto& node = *rect.Node;
                const MaterialGraphLink* outgoing = nullptr;
                for (const auto& link : document.Graph.Links)
                {
                    if (node.OutputPin && link.SourcePin == node.OutputPin)
                    {
                        outgoing = &link;
                        break;
                    }
                }
                const auto pinY = [&](uint64_t pin) {
                    const auto found = document.PinY.find(pin);
                    return found != document.PinY.end() ? found->second : NAN;
                };
                if (outgoing)
                {
                    const float target = pinY(outgoing->TargetPin);
                    const float source = pinY(node.OutputPin);
                    if (std::isfinite(target) && std::isfinite(source))
                    {
                        MoveNode(node, ImVec2(rect.Position.x, rect.Position.y + (target - source)));
                        applied = true;
                    }
                    continue;
                }
                for (const auto pin : node.InputPins)
                {
                    const auto* link = document.Graph.InputLink(pin);
                    auto* upstream = link ? const_cast<MaterialGraphNode*>(document.Graph.FindPinNode(link->SourcePin))
                                          : nullptr;
                    if (!upstream || Nodes::IsNodeSelected(Nodes::NodeId(upstream->Id)))
                    {
                        continue;
                    }
                    const float target = pinY(pin);
                    const float source = pinY(upstream->OutputPin);
                    if (std::isfinite(target) && std::isfinite(source))
                    {
                        const ImVec2 position = Nodes::GetNodePosition(Nodes::NodeId(upstream->Id));
                        MoveNode(*upstream, ImVec2(position.x, position.y + (target - source)));
                        applied = true;
                    }
                }
            }
        }
        else
        {
            auto rects = SelectedRects(document);
            const bool distribute = op == ArrangeOp::DistributeHorizontal || op == ArrangeOp::DistributeVertical;
            if (rects.size() >= (distribute ? 3u : 2u))
            {
                float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
                for (const auto& rect : rects)
                {
                    minX = (std::min)(minX, rect.Position.x);
                    minY = (std::min)(minY, rect.Position.y);
                    maxX = (std::max)(maxX, rect.Position.x + rect.Size.x);
                    maxY = (std::max)(maxY, rect.Position.y + rect.Size.y);
                }
                if (distribute)
                {
                    const bool horizontal = op == ArrangeOp::DistributeHorizontal;
                    std::sort(rects.begin(), rects.end(), [&](const auto& a, const auto& b) {
                        return horizontal ? a.Position.x < b.Position.x : a.Position.y < b.Position.y;
                    });
                    float total = 0.0f;
                    for (const auto& rect : rects)
                    {
                        total += horizontal ? rect.Size.x : rect.Size.y;
                    }
                    const float span = horizontal ? maxX - minX : maxY - minY;
                    const float gap = (span - total) / static_cast<float>(rects.size() - 1);
                    float cursor = horizontal ? minX : minY;
                    for (auto& rect : rects)
                    {
                        MoveNode(*rect.Node, horizontal ? ImVec2(cursor, rect.Position.y) : ImVec2(rect.Position.x, cursor));
                        cursor += (horizontal ? rect.Size.x : rect.Size.y) + gap;
                    }
                }
                else
                {
                    for (auto& rect : rects)
                    {
                        ImVec2 p = rect.Position;
                        switch (op)
                        {
                            case ArrangeOp::AlignTop:
                                p.y = minY;
                                break;
                            case ArrangeOp::AlignMiddle:
                                p.y = (minY + maxY) * 0.5f - rect.Size.y * 0.5f;
                                break;
                            case ArrangeOp::AlignBottom:
                                p.y = maxY - rect.Size.y;
                                break;
                            case ArrangeOp::AlignLeft:
                                p.x = minX;
                                break;
                            case ArrangeOp::AlignCenter:
                                p.x = (minX + maxX) * 0.5f - rect.Size.x * 0.5f;
                                break;
                            case ArrangeOp::AlignRight:
                                p.x = maxX - rect.Size.x;
                                break;
                            default:
                                break;
                        }
                        MoveNode(*rect.Node, p);
                    }
                }
                applied = true;
            }
            else
            {
                document.Status = distribute ? "等間隔に並べるには 3 つ以上のノードを選択してください。"
                                             : "整列するには 2 つ以上のノードを選択してください。";
            }
        }
        if (applied)
        {
            PushHistory(document, before);
        }
    }

    void DrawArrangeMenuItems(GraphDocument& document)
    {
        struct Item
        {
            const char* Label;
            const char* Shortcut;
            ArrangeOp Op;
        };
        static const Item items[] = {
            {"上揃え", "Shift+W", ArrangeOp::AlignTop},
            {"上下中央揃え", "Alt+Shift+W", ArrangeOp::AlignMiddle},
            {"下揃え", "Shift+S", ArrangeOp::AlignBottom},
            {"左揃え", "Shift+A", ArrangeOp::AlignLeft},
            {"左右中央揃え", "Alt+Shift+S", ArrangeOp::AlignCenter},
            {"右揃え", "Shift+D", ArrangeOp::AlignRight},
            {nullptr, nullptr, ArrangeOp::None},
            {"横に等間隔", "", ArrangeOp::DistributeHorizontal},
            {"縦に等間隔", "", ArrangeOp::DistributeVertical},
            {nullptr, nullptr, ArrangeOp::None},
            {"接続を真っすぐにする", "Q", ArrangeOp::Straighten},
            {"選択をコメントで囲む", "C", ArrangeOp::CommentSelection},
            {"グラフ全体を自動整列", "Ctrl+Shift+L", ArrangeOp::AutoLayout},
        };
        for (const auto& item : items)
        {
            if (!item.Label)
            {
                ImGui::Separator();
                continue;
            }
            if (ImGui::MenuItem(item.Label, item.Shortcut))
            {
                document.PendingArrange = static_cast<int>(item.Op);
            }
        }
    }

    // ============================================================ ノード追加（UE のパレット）
    const char* const kCategoryOrder[] = {"Parameters", "Constants", "Texture", "Coordinates",
                                          "Math",       "Vector",    "Utility", "Comment"};

    uint64_t AddNodeAt(GraphDocument& document, Kind kind, ImVec2 position)
    {
        const auto before = document.Graph;
        const auto id = document.Graph.AddNode(kind, position.x, position.y);
        if (!id)
        {
            document.Error = "ノードを追加できません（上限に達しています）。";
            return 0;
        }
        if (kind == Kind::Group)
        {
            document.NewGroups.push_back(id);
        }
        // ピンからドラッグして離した場合は、新しいノードと自動で接続する（UE と同じ）
        if (document.PendingConnectPin)
        {
            bool input = false;
            const auto* pinNode = document.Graph.FindPinNode(document.PendingConnectPin, &input);
            const auto* added = document.Graph.FindNode(id);
            std::string error;
            if (pinNode && added)
            {
                if (!input)
                {
                    for (const auto target : added->InputPins)
                    {
                        if (document.Graph.CanConnect(document.PendingConnectPin, target, error))
                        {
                            document.Graph.Connect(document.PendingConnectPin, target, error);
                            break;
                        }
                    }
                }
                else if (added->OutputPin &&
                         document.Graph.CanConnect(added->OutputPin, document.PendingConnectPin, error))
                {
                    document.Graph.Connect(added->OutputPin, document.PendingConnectPin, error);
                }
            }
            document.PendingConnectPin = 0;
        }
        PushHistory(document, before);
        document.RestorePositions = true;
        document.PendingSelection = {id};
        return id;
    }

    void DrawAddNodeMenu(GraphDocument& document)
    {
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(240.0f);
        const bool enter = ImGui::InputTextWithHint("##Search", "ノードを検索（例: lerp, uv, noise）", document.Search,
                                                    sizeof(document.Search), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGuiTextFilter filter(document.Search);
        const auto label = [](const MaterialNodeDefinition& definition) {
            return definition.Kind == Kind::Group ? "Comment" : definition.Name;
        };
        if (filter.IsActive())
        {
            // 検索中はカテゴリを開かずに一覧表示
            bool first = true;
            for (const auto& definition : MaterialGraph::Definitions())
            {
                if (definition.Kind == Kind::Output || !filter.PassFilter(label(definition)))
                {
                    continue;
                }
                const bool chosen = ImGui::MenuItem(label(definition), Category(definition.Kind).Name);
                if (chosen || (enter && first))
                {
                    AddNodeAt(document, definition.Kind, document.AddPosition);
                    ImGui::CloseCurrentPopup();
                    break;
                }
                first = false;
            }
            return;
        }
        for (const char* category : kCategoryOrder)
        {
            if (!ImGui::BeginMenu(category))
            {
                continue;
            }
            for (const auto& definition : MaterialGraph::Definitions())
            {
                if (definition.Kind == Kind::Output || strcmp(Category(definition.Kind).Name, category) != 0)
                {
                    continue;
                }
                if (ImGui::MenuItem(label(definition)))
                {
                    AddNodeAt(document, definition.Kind, document.AddPosition);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::TextDisabled("ヒント: キー + 左クリックで配置（1〜4 / A / M / L / T / U / S / V ...）");
    }

    // UE と同じく「キーを押しながら背景を左クリック」でノードを置く
    Kind PlacementKind(bool& found)
    {
        struct Binding
        {
            ImGuiKey Key;
            Kind NodeKind;
        };
        static const Binding bindings[] = {
            {ImGuiKey_1, Kind::Constant},          {ImGuiKey_2, Kind::Constant2Vector},
            {ImGuiKey_3, Kind::Constant3Vector},   {ImGuiKey_4, Kind::Constant4Vector},
            {ImGuiKey_A, Kind::Add},               {ImGuiKey_M, Kind::Multiply},
            {ImGuiKey_D, Kind::Divide},            {ImGuiKey_L, Kind::Lerp},
            {ImGuiKey_O, Kind::OneMinus},          {ImGuiKey_E, Kind::Power},
            {ImGuiKey_P, Kind::Panner},            {ImGuiKey_T, Kind::TextureSample},
            {ImGuiKey_U, Kind::TextureCoordinate}, {ImGuiKey_S, Kind::ScalarParameter},
            {ImGuiKey_V, Kind::VectorParameter},   {ImGuiKey_N, Kind::Normalize},
            {ImGuiKey_R, Kind::ReflectionVector},  {ImGuiKey_I, Kind::If},
            {ImGuiKey_F, Kind::Fresnel},           {ImGuiKey_B, Kind::Reroute},
        };
        for (const auto& binding : bindings)
        {
            if (ImGui::IsKeyDown(binding.Key))
            {
                found = true;
                return binding.NodeKind;
            }
        }
        found = false;
        return Kind::Constant;
    }

    void HandleCanvasShortcuts(GraphDocument& document, bool canvasHovered, bool canvasFocused)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput || ImGui::IsAnyItemActive() || !(canvasHovered || canvasFocused))
        {
            return;
        }
        auto pressed = [](ImGuiKey key) {
            return ImGui::IsKeyPressed(key, false);
        };
        if (io.KeyShift && !io.KeyCtrl)
        {
            if (pressed(ImGuiKey_W))
                document.PendingArrange = static_cast<int>(io.KeyAlt ? ArrangeOp::AlignMiddle : ArrangeOp::AlignTop);
            if (pressed(ImGuiKey_S))
                document.PendingArrange = static_cast<int>(io.KeyAlt ? ArrangeOp::AlignCenter : ArrangeOp::AlignBottom);
            if (pressed(ImGuiKey_A) && !io.KeyAlt)
                document.PendingArrange = static_cast<int>(ArrangeOp::AlignLeft);
            if (pressed(ImGuiKey_D) && !io.KeyAlt)
                document.PendingArrange = static_cast<int>(ArrangeOp::AlignRight);
        }
        if (io.KeyCtrl && io.KeyShift && pressed(ImGuiKey_L))
        {
            document.PendingArrange = static_cast<int>(ArrangeOp::AutoLayout);
        }
        if (!io.KeyCtrl && !io.KeyShift && !io.KeyAlt)
        {
            if (pressed(ImGuiKey_Q))
                document.PendingArrange = static_cast<int>(ArrangeOp::Straighten);
            if (pressed(ImGuiKey_C) && Nodes::GetSelectedObjectCount() > 0)
                document.PendingArrange = static_cast<int>(ArrangeOp::CommentSelection);
        }
    }

    void DrawCanvas(GraphDocument& document)
    {
        std::optional<std::pair<uint64_t, uint64_t>> connection;
        // UE 風の配色（暗い背景、オレンジの選択枠）
        Nodes::PushStyleColor(Nodes::StyleColor_Bg, ImVec4(0.09f, 0.09f, 0.10f, 1.0f));
        Nodes::PushStyleColor(Nodes::StyleColor_Grid, ImVec4(1.0f, 1.0f, 1.0f, 0.05f));
        Nodes::PushStyleColor(Nodes::StyleColor_NodeBg, ImVec4(0.11f, 0.11f, 0.12f, 0.96f));
        Nodes::PushStyleColor(Nodes::StyleColor_NodeBorder, ImVec4(0.0f, 0.0f, 0.0f, 0.9f));
        Nodes::PushStyleColor(Nodes::StyleColor_HovNodeBorder, ImVec4(0.85f, 0.85f, 0.85f, 0.8f));
        Nodes::PushStyleColor(Nodes::StyleColor_SelNodeBorder, ImVec4(1.0f, 0.66f, 0.15f, 1.0f));
        Nodes::PushStyleColor(Nodes::StyleColor_SelLinkBorder, ImVec4(1.0f, 0.66f, 0.15f, 1.0f));
        Nodes::PushStyleColor(Nodes::StyleColor_Flow, ImVec4(1.0f, 0.80f, 0.35f, 1.0f));
        Nodes::PushStyleColor(Nodes::StyleColor_FlowMarker, ImVec4(1.0f, 0.80f, 0.35f, 1.0f));
        Nodes::PushStyleVar(Nodes::StyleVar_NodePadding, ImVec4(8, 4, 8, 6));
        Nodes::PushStyleVar(Nodes::StyleVar_NodeRounding, 6.0f);
        Nodes::PushStyleVar(Nodes::StyleVar_SelectedNodeBorderWidth, 2.5f);
        Nodes::PushStyleVar(Nodes::StyleVar_LinkStrength, 120.0f);
        Nodes::PushStyleVar(Nodes::StyleVar_FlowDuration, 1.2f);
        Nodes::Begin("MaterialGraphCanvas");
        const bool canvasHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        const bool canvasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        if (document.RestorePositions)
        {
            RestorePositions(document);
        }
        const FrameInfo frame = BuildFrameInfo(document.Graph);
        document.PinY.clear();
        // コメントを先に描く（ノードの後ろに来るように）
        for (auto& node : document.Graph.Nodes)
        {
            if (node.Kind == Kind::Group)
            {
                DrawNode(document, node, frame);
            }
        }
        for (auto& node : document.Graph.Nodes)
        {
            if (node.Kind != Kind::Group)
            {
                DrawNode(document, node, frame);
            }
        }
        for (const auto id : document.NewGroups)
        {
            if (const auto* group = document.Graph.FindNode(id))
            {
                Nodes::SetGroupSize(Nodes::NodeId(id), ImVec2(group->GroupSize[0], group->GroupSize[1]));
            }
        }
        document.NewGroups.clear();
        if (!document.PendingSelection.empty())
        {
            Nodes::ClearSelection();
            for (const auto id : document.PendingSelection)
            {
                Nodes::SelectNode(Nodes::NodeId(id), true);
            }
            document.PendingSelection.clear();
        }

        // ---- 接続線：出力の型で色分け、選択 / ホバー中のノードに繋がる線を太く強調し、流れを表示する
        const bool selectionChanged = Nodes::HasSelectionChanged();
        for (const auto& link : document.Graph.Links)
        {
            const auto source = frame.PinOwner.find(link.SourcePin);
            const auto target = frame.PinOwner.find(link.TargetPin);
            const uint64_t sourceNode = source != frame.PinOwner.end() ? source->second->Id : 0;
            const uint64_t targetNode = target != frame.PinOwner.end() ? target->second->Id : 0;
            const auto typeFound = frame.OutputTypes.find(sourceNode);
            const auto type = typeFound != frame.OutputTypes.end() ? typeFound->second : MaterialGraphType::Numeric;
            const bool highlight = frame.Selected.contains(sourceNode) || frame.Selected.contains(targetNode) ||
                                   (frame.Hovered != 0 && (frame.Hovered == sourceNode || frame.Hovered == targetNode));
            const bool dim = !frame.Selected.empty() && !highlight;
            const ImVec4 color = ImGui::ColorConvertU32ToFloat4(TypeColor(type, dim ? 0.35f : (highlight ? 1.0f : 0.85f)));
            Nodes::Link(Nodes::LinkId(link.Id), Nodes::PinId(link.SourcePin), Nodes::PinId(link.TargetPin), color,
                        highlight ? 3.5f : 2.0f);
            if (selectionChanged && highlight && !frame.Selected.empty())
            {
                Nodes::Flow(Nodes::LinkId(link.Id));
            }
        }

        // ---- ノード整理（ツールバー / メニュー / ショートカットからの要求）
        HandleCanvasShortcuts(document, canvasHovered, canvasFocused);
        if (document.PendingArrange != 0)
        {
            ApplyArrange(document, static_cast<ArrangeOp>(document.PendingArrange));
            document.PendingArrange = 0;
        }

        // ---- 接続の作成（ピンからドラッグ。何もない所で離すと追加メニュー → 自動接続）
        if (Nodes::BeginCreate(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), 2.5f))
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
                    if (Nodes::AcceptNewItem(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), 3.0f))
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
                    Nodes::RejectNewItem(ImVec4(0.9f, 0.25f, 0.2f, 1), 2.0f);
                    document.Error = error;
                }
            }
            Nodes::PinId pin;
            if (Nodes::QueryNewNode(&pin) && pin)
            {
                if (Nodes::AcceptNewItem(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), 2.0f))
                {
                    document.PendingConnectPin = pin.Get();
                    document.OpenAddMenu = true;
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

        // ---- キー + クリックでノード配置 / 線のダブルクリックで中継点 / コメントのダブルクリックで名前変更
        if (Nodes::IsBackgroundClicked() &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left)) &&
            !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyAlt)
        {
            bool found = false;
            const Kind kind = PlacementKind(found);
            if (found)
            {
                AddNodeAt(document, kind, Nodes::ScreenToCanvas(ImGui::GetMousePos()));
            }
        }
        if (const auto doubleLink = Nodes::GetDoubleClickedLink())
        {
            const auto it = std::find_if(document.Graph.Links.begin(), document.Graph.Links.end(),
                                         [&](const auto& link) { return link.Id == doubleLink.Get(); });
            if (it != document.Graph.Links.end())
            {
                const auto link = *it;
                MaterialGraph candidate = document.Graph;
                const ImVec2 at = Nodes::ScreenToCanvas(ImGui::GetMousePos());
                const auto reroute = candidate.AddNode(Kind::Reroute, at.x - 10.0f, at.y - 8.0f);
                const auto* added = candidate.FindNode(reroute);
                std::string error;
                if (added && candidate.RemoveLink(link.Id) &&
                    candidate.Connect(link.SourcePin, added->InputPins[0], error))
                {
                    const auto outputPin = candidate.FindNode(reroute)->OutputPin;
                    if (candidate.Connect(outputPin, link.TargetPin, error))
                    {
                        PushHistory(document, document.Graph);
                        document.Graph = std::move(candidate);
                        document.RestorePositions = true;
                    }
                }
            }
        }
        if (const auto doubleNode = Nodes::GetDoubleClickedNode())
        {
            if (const auto* node = document.Graph.FindNode(doubleNode.Get()); node && node->Kind == Kind::Group)
            {
                document.EditingComment = node->Id;
                snprintf(document.CommentBuffer, sizeof(document.CommentBuffer), "%s", node->Name.c_str());
            }
        }

        Nodes::NodeId context;
        const bool nodeMenu = Nodes::ShowNodeContextMenu(&context);
        const bool backgroundMenu = Nodes::ShowBackgroundContextMenu();
        Nodes::Suspend();
        if (backgroundMenu || document.OpenAddMenu)
        {
            if (backgroundMenu)
            {
                document.PendingConnectPin = 0;
            }
            document.OpenAddMenu = false;
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
                snprintf(document.CommentBuffer, sizeof(document.CommentBuffer), "%s", node->Comment.c_str());
                if (!Nodes::IsNodeSelected(context))
                {
                    Nodes::SelectNode(context);
                }
                ImGui::OpenPopup("MaterialNodeMenu");
            }
        }
        if (ImGui::BeginPopup("AddMaterialNode"))
        {
            DrawAddNodeMenu(document);
            ImGui::EndPopup();
        }
        else if (!ImGui::IsPopupOpen("AddMaterialNode"))
        {
            // メニューを閉じたら、保留中の自動接続も破棄する
            if (!document.OpenAddMenu)
            {
                document.PendingConnectPin = 0;
            }
        }
        if (ImGui::BeginPopup("MaterialNodeMenu"))
        {
            auto* contextNode = document.Graph.FindNode(document.ContextNode);
            const bool isComment = contextNode && contextNode->Kind == Kind::Group;
            if (ImGui::InputText(isComment ? "タイトル" : "名前", document.Rename, sizeof(document.Rename),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (contextNode)
                {
                    PushHistory(document, document.Graph);
                    contextNode->Name = document.Rename;
                }
                ImGui::CloseCurrentPopup();
            }
            if (contextNode && isComment)
            {
                ImVec4 color = CommentColor(*contextNode);
                if (ImGui::ColorEdit3("色", &color.x, ImGuiColorEditFlags_NoInputs))
                {
                    if (!document.PendingEdit)
                    {
                        document.PendingEdit = document.Graph;
                    }
                    contextNode->Value = {color.x, color.y, color.z, 1.0f};
                    document.ValueChanged = true;
                    document.Dirty = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    FinishValueEdit(document);
                }
            }
            else if (contextNode)
            {
                if (ImGui::InputTextWithHint("ノードコメント", "ノードの上に表示（Enter で確定）", document.CommentBuffer,
                                             sizeof(document.CommentBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    PushHistory(document, document.Graph);
                    contextNode->Comment = document.CommentBuffer;
                    ImGui::CloseCurrentPopup();
                }
                if (!contextNode->Comment.empty() && ImGui::MenuItem("ノードコメントを削除"))
                {
                    PushHistory(document, document.Graph);
                    contextNode->Comment.clear();
                }
                if (MaterialGraphPreview::HasPreview(*contextNode) &&
                    ImGui::MenuItem("プレビューの表示 / 非表示", nullptr,
                                    !MaterialGraphPreview::IsNodePreviewCollapsed(document.Graph, contextNode->Id)))
                {
                    MaterialGraphPreview::ToggleNodePreview(document.Graph, contextNode->Id);
                }
                if ((contextNode->Width > 0.0f || contextNode->Height > 0.0f) && ImGui::MenuItem("サイズを既定に戻す"))
                {
                    PushHistory(document, document.Graph);
                    contextNode->Width = 0.0f;
                    contextNode->Height = 0.0f;
                }
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("整列"))
            {
                DrawArrangeMenuItems(document);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("コピー", "Ctrl+C"))
            {
                CopySelection(document);
            }
            if (ImGui::MenuItem("複製", "Ctrl+D"))
            {
                CopySelection(document);
                PasteSelection(document);
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
            if (sourceNode && sourceNode->Kind != Kind::Output && ImGui::MenuItem("削除", "Delete"))
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
        Nodes::PopStyleVar(5);
        Nodes::PopStyleColor(9);
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

namespace
{
    // グラフ → マテリアルアセット（エンジンが描画に使う値）へ焼き込む
    void ApplyGraphToMaterial(AssetHandle material, GraphDocument& document, bool force)
    {
        if (document.LoadFailed)
        {
            return;
        }
        auto& runtime = MaterialManager::GetRuntime(material);
        if (!document.ApplyInitialized)
        {
            // 既存のテクスチャのうち、グラフの Texture Sample と同じものは「グラフ管理」とみなす
            for (size_t slot = 0; slot < runtime.Editing.Textures.size(); ++slot)
            {
                const AssetHandle texture = runtime.Editing.Textures[slot];
                for (const auto& node : document.Graph.Nodes)
                {
                    if (texture != 0 && node.Kind == Kind::TextureSample && node.Texture == texture)
                    {
                        document.ManagedSlots |= 1u << slot;
                    }
                }
            }
            document.ApplyInitialized = true;
        }
        const uint64_t signature = MaterialGraphPreview::ContentSignature(document.Graph);
        if (!force && signature == document.AppliedSignature)
        {
            return;
        }
        document.AppliedSignature = signature;
        auto baked = MaterialGraphPreview::BakeToMaterial(document.Graph, runtime.Editing, document.ManagedSlots);
        document.ManagedSlots = baked.ManagedSlots;
        document.ApplyNotes = std::move(baked.Notes);
        if (baked.HasOutput && !(baked.Data == runtime.Editing))
        {
            MaterialManager::SetEditing(material, baked.Data);
        }
    }
} // namespace

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
        // グラフとマテリアル（反映済みの値）を両方保存する
        ApplyGraphToMaterial(material, document, true);
        if (Save(material))
        {
            MaterialManager::Save(material);
        }
    }
    ImGui::SetItemTooltip("グラフ（.graph.json）とマテリアル（.material）を保存します");
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
    if (ImGui::Button("整列 ▾"))
    {
        ImGui::OpenPopup("GraphArrangeMenu");
    }
    ImGui::SetItemTooltip("UE と同じノード整理（Shift+W/A/S/D、Q、C）");
    if (ImGui::BeginPopup("GraphArrangeMenu"))
    {
        DrawArrangeMenuItems(document);
        ImGui::EndPopup();
    }
    ContinueToolbar();
    if (ImGui::Button("+ コメント"))
    {
        document.PendingArrange = static_cast<int>(ArrangeOp::CommentSelection);
    }
    ImGui::SetItemTooltip("選択ノードをコメントで囲む (C)");
    ContinueToolbar();
    ImGui::Checkbox("ノードプレビュー", &showNodePreviews);
    ContinueToolbar();
    ImGui::Checkbox("自動反映", &document.AutoApply);
    ImGui::SetItemTooltip("グラフの変更をすぐにマテリアル（シーン上のオブジェクト）へ反映します");
    if (!document.AutoApply)
    {
        ContinueToolbar();
        if (ImGui::Button("マテリアルに反映"))
        {
            ApplyGraphToMaterial(material, document, true);
        }
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
        // 左パネルの「選択ノードをプレビュー」用に、単一選択のノードを記録する
        if (document.AutoApply)
        {
            ApplyGraphToMaterial(material, document, false);
        }
        Nodes::NodeId selected[2];
        document.SelectedNode =
            Nodes::GetSelectedNodes(selected, 2) == 1 ? static_cast<uint64_t>(selected[0].Get()) : 0;
    }
    else
    {
        ImGui::TextWrapped("グラフを読み込めませんでした。既存ファイルを保持しています: %s",
                           document.Path.string().c_str());
    }
    Nodes::SetCurrentEditor(nullptr);
    ImGui::PopID();
}

void MaterialGraphEditor::DrawPreviewPanel(AssetHandle material)
{
    auto& document = Document(material);
    ImGui::PushID("GraphPreviewPanel");
    // エンジンのマテリアルプレビュー（実際のシェーダーで描画）。グラフの変更は自動反映で即座に映る
    if (document.AutoApply)
    {
        ApplyGraphToMaterial(material, document, false); // 同じフレームでプレビューに反映させる
    }
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("EnginePreviewImage", ImVec2(width, width), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    MaterialPreview::DrawImage(material);
    ImGui::EndChild();
    MaterialPreview::DrawControls();

    ImGui::SeparatorText("グラフ");
    size_t textures = 0;
    for (const auto& node : document.Graph.Nodes)
    {
        textures += node.Kind == Kind::TextureSample ? 1 : 0;
    }
    ImGui::TextDisabled("ノード %zu / 接続 %zu / テクスチャ %zu", document.Graph.Nodes.size(),
                        document.Graph.Links.size(), textures);
    ImGui::SeparatorText("マテリアルへの反映");
    if (document.ApplyNotes.empty())
    {
        ImGui::TextColored(ImVec4(0.45f, 0.82f, 0.50f, 1.0f), document.AutoApply ? "✓ 自動反映中" : "手動反映");
    }
    for (const auto& note : document.ApplyNotes)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.72f, 0.20f, 1.0f));
        ImGui::TextWrapped("⚠ %s", note.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::TextDisabled("保存は「Save」または Ctrl+S（未保存の変更は * 表示）");
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
    MaterialGraphTextures::Clear();
    focusedMaterial = 0;
    nextRecoverySave = 0;
}
