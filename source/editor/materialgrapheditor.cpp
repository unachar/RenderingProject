#include "pch.h"
#include "backgroundtasks.h"
#include <cstdarg>
#include <imgui_internal.h>
#include <limits>
#include <functional>
#include "materialgrapheditor.h"
#include "materialgraph.h"
#include "materialgraphcompiler.h"
#include "materialgraphshader.h"
#include "materialmanager.h"
#include "materialgraphpreview.h"
#include "materialgraphtextures.h"
#include "materialpreview.h"
#include "materialgraphbake.h"
#include "materialgraphmath.h"
#include "assetthumbnailmanager.h"
#include "texturemanager.h"
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

    struct FrameInfo
    {
        std::unordered_map<uint64_t, const MaterialGraphNode*> PinOwner;
        std::unordered_map<uint64_t, uint64_t> InputSource;
        std::unordered_set<uint64_t> ConnectedPins;
        std::unordered_map<uint64_t, MaterialGraphType> OutputTypes;
        std::unordered_map<uint64_t, MaterialGraphType> PinTypes;
        std::unordered_set<uint64_t> Selected;
        uint64_t Hovered = 0;
    };

    struct GraphDocument
    {
        MaterialGraph Graph;
        MaterialGraph SavedGraph;
        std::vector<MaterialGraph> Undo;
        std::vector<MaterialGraph> Redo;
        std::optional<MaterialGraph> PendingEdit;
        std::optional<MaterialGraph> PendingMove;
        ImVec2 ResizeStartMouse{};
        ImVec2 ResizeStartSize{};
        FrameInfo CanvasFrame;
        uint64_t CanvasSignature = ~0ull;
        const MaterialGraphNode* CanvasNodeStorage = nullptr;
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
        uint64_t ContextPin = 0; // 右クリックしたピン
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
        double LastApplyTime = -1.0;  // 最後にマテリアルへ反映した時刻（ドラッグ中の反映を間引く）
        int SignatureFrame = -1;      // ContentSignature を計算したフレーム（1 フレームに 2 回呼ばれるため）
        uint64_t FrameSignature = 0;
        uint32_t ManagedSlots = 0;
        std::vector<std::string> ApplyNotes;
        // ピクセルごとの式 → エンジン用テクスチャのベイク
        MaterialGraphPreview::BakedTextures Baked;
        std::future<MaterialGraphBake::Result> BakeJob;
        uint64_t BakeJobSignature = 0;
        uint64_t FailedBakeSignature = 0;
        uint64_t LastContentSignature = 0;
        double LastContentChange = 0.0;
        uint32_t PendingBakeMask = 0;
        MaterialGraphPreview::AlphaSource PendingAlpha = MaterialGraphPreview::AlphaSource::BaseColor;
        bool BakeRequested = false;
        std::string BakeStatus;
        std::vector<std::string> BakeWarnings;
        // ライブ更新（Time を使う式を実行中に動かす）
        uint64_t LiveSignature = 0;
        // Convert To Texture の変換
        AssetHandle Material = 0;
        std::future<MaterialGraphBake::NodeResult> NodeJob;
        uint64_t NodeJobNode = 0;
        std::string NodeStatus;

        ~GraphDocument()
        {
            if (NodeJob.valid())
            {
                NodeJob.wait();
            }
            if (BakeJob.valid())
            {
                BakeJob.wait(); // ワーカーはグラフのコピーで動くので、待つだけでよい
            }
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
        document.Material = material;
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
        // 旧グラフ（マテリアルモード未設定）：マテリアルの値からモードを決める（ピンの有効 / 無効に使う）
        const auto& asset = MaterialManager::GetRuntime(material).Editing;
        const auto migrateMode = [&](MaterialGraph& graph) {
            if (graph.Settings.Mode >= 0)
            {
                return;
            }
            MaterialGraphMode mode = MaterialGraph::ModeFromMaterial(asset);
            if (graph.Settings.Surface == 0)
            {
                mode = MaterialGraphMode::Opaque;
            }
            else if (graph.Settings.Surface == 1)
            {
                mode = MaterialGraphMode::Masked;
            }
            else if (graph.Settings.Surface == 2 && mode != MaterialGraphMode::Glass)
            {
                mode = MaterialGraphMode::Translucent;
            }
            graph.Settings.Mode = static_cast<int>(mode);
        };
        if (!document.LoadFailed)
        {
            migrateMode(document.Graph);
        }
        document.SavedGraph = document.Graph;
        if (!document.LoadFailed && std::filesystem::exists(document.RecoveryPath, error))
        {
            MaterialGraph recovered;
            std::string recoveredLayout;
            if (ReadDocument(document.RecoveryPath, recovered, recoveredLayout, document.Error))
            {
                migrateMode(recovered);
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
            case Kind::TriplanarTexture:
            case Kind::ConvertToTexture:
            case Kind::ConvertToHeight:
            case Kind::ConvertToNormal:
                return {"Texture", IM_COL32(48, 92, 168, 255)};
            case Kind::ConvertToNormalHeight:
                return {"Legacy", IM_COL32(48, 92, 168, 255)}; // 追加メニューには出さない
            case Kind::UV:
            case Kind::TextureCoordinate:
            case Kind::Panner:
            case Kind::Rotator:
            case Kind::WorldPosition:
            case Kind::WorldNormal:
            case Kind::CameraVector:
            case Kind::ReflectionVector:
            case Kind::SolidCoordinate:
            case Kind::LocalPosition:
            case Kind::VertexColor:
            case Kind::CameraDistance:
            case Kind::CameraPosition:
            case Kind::PixelDepth:
            case Kind::ScreenPosition:
                return {"Coordinates", IM_COL32(150, 62, 62, 255)};
            case Kind::ObjectPosition:
            case Kind::ObjectScale:
            case Kind::ObjectOrientation:
            case Kind::ObjectRadius:
            case Kind::ObjectBounds:
            case Kind::BoundingBoxUVW:
            case Kind::PerInstanceRandom:
            case Kind::TransformVector:
            case Kind::TransformPosition:
                return {"Object", IM_COL32(170, 110, 50, 255)};
            case Kind::Normalize:
            case Kind::Dot:
            case Kind::CrossProduct:
            case Kind::Distance:
            case Kind::Length:
            case Kind::AppendVector:
            case Kind::ComponentMask:
            case Kind::BreakVector:
                return {"Vector", IM_COL32(108, 78, 146, 255)};
            case Kind::Fresnel:
            case Kind::Desaturation:
            case Kind::If:
            case Kind::SphereMask:
            case Kind::CheapContrast:
            case Kind::Noise:
            case Kind::Reroute:
            case Kind::StaticSwitch:
            case Kind::SlopeMask:
            case Kind::HeightLerp:
            case Kind::DitherOpacity:
            case Kind::DistanceFade:
            case Kind::HeightGradient:
                return {"Utility", IM_COL32(140, 104, 40, 255)};
            case Kind::Pi:
                return {"Constants", IM_COL32(56, 118, 70, 255)};
            case Kind::HueShift:
            case Kind::Blend:
            case Kind::ColorAdjust:
            case Kind::BlackBody:
                return {"Color", IM_COL32(170, 70, 120, 255)};
            case Kind::LinearGradient:
            case Kind::RadialGradient:
            case Kind::Voronoi:
            case Kind::ProceduralNoise:
            case Kind::FractalNoise:
            case Kind::NoiseCombine:
            case Kind::NoiseLayers:
            case Kind::SolidTexture:
            case Kind::SolidPattern:
            case Kind::ThreeColorRamp:
            case Kind::ConvertToSolid:
                return {"Procedural", IM_COL32(60, 128, 140, 255)};
            case Kind::NormalFromHeight:
            case Kind::BlendAngleCorrectedNormals:
            case Kind::FlattenNormal:
            case Kind::BumpOffset:
                return {"Normal / Height", IM_COL32(86, 96, 170, 255)};
            case Kind::RotateAboutAxis:
                return {"Vector", IM_COL32(108, 78, 146, 255)};
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

    const char* ComponentName(size_t component)
    {
        static const char* names[] = {"X (R)", "Y (G)", "Z (B)", "W (A)"};
        return names[(std::min)(component, size_t{3})];
    }

    ImVec4 ComponentColor(size_t component)
    {
        static const ImVec4 colors[] = {{0.96f, 0.32f, 0.28f, 1.0f}, {0.35f, 0.85f, 0.38f, 1.0f},
                                        {0.35f, 0.60f, 1.0f, 1.0f}, {0.85f, 0.85f, 0.88f, 1.0f}};
        return colors[(std::min)(component, size_t{3})];
    }

    // 数字は読みやすい明度を保ち、背景と枠で成分を区別する。
    void PushComponentFieldStyle(size_t component)
    {
        const ImVec4 color = ComponentColor(component);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(color.x * 0.18f, color.y * 0.18f, color.z * 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(color.x * 0.30f, color.y * 0.30f, color.z * 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(color.x * 0.42f, color.y * 0.42f, color.z * 0.42f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, color);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    }

    void PopComponentFieldStyle()
    {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }

    float NodeWidth(const MaterialGraphNode& node)
    {
        if (node.Width > 0.0f)
        {
            return std::clamp(node.Width, 96.0f, 512.0f);
        }
        if (node.Kind == Kind::ComponentMask || node.Kind == Kind::VectorParameter ||
            (MaterialGraph::IsComponentConstant(node.Kind) && node.Kind != Kind::Constant))
        {
            return 176.0f;
        }
        return node.Kind == Kind::Output ? 168.0f : 128.0f;
    }

    ImVec4 CommentColor(const MaterialGraphNode& node)
    {
        const bool unset = node.Value[0] == 0.0f && node.Value[1] == 0.0f && node.Value[2] == 0.0f;
        return unset ? ImVec4(0.30f, 0.42f, 0.60f, 1.0f) : ImVec4(node.Value[0], node.Value[1], node.Value[2], 1.0f);
    }

    // 1 フレーム分の描画情報（接続状態・型・強調）
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
                info.PinTypes[node.OutputPin] = info.OutputTypes[node.Id];
            }
            for (const auto pin : node.SplitPins)
            {
                info.PinOwner[pin] = &node;
                info.PinTypes[pin] = MaterialGraphType::Scalar;
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
        return info;
    }

    void UpdateFrameSelection(FrameInfo& info)
    {
        info.Selected.clear();
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

    std::string SlotList(uint32_t mask)
    {
        std::string text;
        for (size_t slot = 0; slot < MaterialGraphBake::kSlotCount; ++slot)
        {
            if (mask & (1u << slot))
            {
                text += (text.empty() ? "" : ", ") + std::string(MaterialGraphBake::SlotName(static_cast<MaterialTextureSlot>(slot)));
            }
        }
        return text;
    }

    // グラフ設定の編集を 1 回の Undo にまとめる（ドラッグ中は確定しない）
    constexpr uint64_t kSettingsEditNode = ~0ull;

    void TrackSettingsEdit(GraphDocument& document, const MaterialGraphSettings& before, bool changed)
    {
        if (ImGui::IsItemActivated())
        {
            FinishValueEdit(document);
            document.PendingEdit = document.Graph;
            document.PendingEdit->Settings = before;
            document.PendingValueNode = kSettingsEditNode;
            document.ValueChanged = false;
        }
        if (changed)
        {
            if (!document.PendingEdit || document.PendingValueNode != kSettingsEditNode)
            {
                FinishValueEdit(document);
                document.PendingEdit = document.Graph;
                document.PendingEdit->Settings = before;
                document.PendingValueNode = kSettingsEditNode;
            }
            document.ValueChanged = true;
            document.Dirty = true;
            document.RecoveryDirty = true;
            ++document.Graph.Revision;
        }
        if (document.PendingEdit && document.PendingValueNode == kSettingsEditNode &&
            (ImGui::IsItemDeactivatedAfterEdit() ||
             (!ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))))
        {
            FinishValueEdit(document);
        }
    }

    // UE のマテリアル Details に相当する設定（グラフに保存。「マテリアルの設定」はアセット側の値を使う）
    void DrawGraphSettings(GraphDocument& document, AssetHandle material)
    {
        auto& settings = document.Graph.Settings;
        const float width = ImGui::GetContentRegionAvail().x * 0.55f;
        // ---- マテリアルモード（UE の Blend Mode に相当）。Material Output の使えるピンが変わる
        {
            static const char* modes[] = {"Opaque（不透明）", "Masked（切り抜き）", "Translucent（半透明）",
                                          "Glass（屈折・透過）"};
            static const char* tooltips[] = {
                "不透明。Opacity / Opacity Mask / 屈折系のピンは使えません",
                "アルファで切り抜き。Opacity Mask と Opacity Mask Clip を使います",
                "半透明。Opacity を使います（POM は使えません）",
                "ガラス。IOR / Transmission / Transmission Roughness / Thickness / Absorption を使います"};
            const MaterialGraphSettings before = settings;
            const int current = settings.Mode;
            ImGui::SetNextItemWidth(width);
            bool changed = false;
            if (ImGui::BeginCombo("マテリアルモード", current >= 0 && current <= 3 ? modes[current] : "（未設定：すべてのピン）"))
            {
                for (int i = 0; i < 4; ++i)
                {
                    if (ImGui::Selectable(modes[i], i == current))
                    {
                        if (i != current)
                        {
                            document.Graph.SetMode(static_cast<MaterialGraphMode>(i));
                            if (i == static_cast<int>(MaterialGraphMode::Masked) && settings.AlphaCutoff < 0.0f)
                            {
                                settings.AlphaCutoff = 0.3333f;
                            }
                            changed = true;
                        }
                    }
                    ImGui::SetItemTooltip("%s", tooltips[i]);
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("UE の Blend Mode と同じく、モードに合わない Material Output のピンは灰色になり接続できません。\n"
                                  "既存の接続は残り、モードを戻すと再び有効になります。");
            TrackSettingsEdit(document, before, changed);
        }
        const auto combo = [&](const char* label, int& value, const char* const* names, int count, const char* tooltip) {
            const MaterialGraphSettings before = settings;
            int index = value + 1; // 0 = マテリアルの設定
            ImGui::SetNextItemWidth(width);
            bool changed = false;
            if (ImGui::BeginCombo(label, index >= 0 && index <= count ? names[index] : names[0]))
            {
                for (int i = 0; i <= count; ++i)
                {
                    if (ImGui::Selectable(names[i], i == index))
                    {
                        changed = i != index;
                        value = i - 1;
                    }
                }
                ImGui::EndCombo();
            }
            if (tooltip)
            {
                ImGui::SetItemTooltip("%s", tooltip);
            }
            TrackSettingsEdit(document, before, changed);
        };
        static const char* surfaces[] = {"マテリアルの設定", "Opaque", "Masked", "Translucent"};
        static const char* culls[] = {"マテリアルの設定", "片面（Back）", "両面（Two Sided）", "Front"};
        static const char* models[] = {"マテリアルの設定", "Default Lit", "Clear Coat", "Cloth", "Subsurface (Skin)", "Hair",
                                       "Anisotropic"};
        static const char* shadows[] = {"マテリアルの設定", "None", "Opaque", "Alpha Test", "Transmittance", "Auto"};
        if (settings.Mode < 0)
        {
            // 旧グラフ（マテリアルモード未設定）だけ、従来の Blend Mode を直接選べる
            combo("Blend Mode", settings.Surface, surfaces, 3, "Opacity / Opacity Mask を繋ぐと自動で Translucent / Masked になります");
        }
        combo("Two Sided", settings.Cull, culls, 3, nullptr);
        combo("Shading Model", settings.ShadingModel, models, 6, nullptr);
        combo("Shadow", settings.ShadowMode, shadows, 5, nullptr);
        {
            const MaterialGraphSettings before = settings;
            bool useClip = settings.AlphaCutoff >= 0.0f;
            bool changed = ImGui::Checkbox("##UseClip", &useClip);
            if (changed)
            {
                settings.AlphaCutoff = useClip ? 0.3333f : -1.0f;
            }
            TrackSettingsEdit(document, before, changed);
            ImGui::SameLine();
            ImGui::BeginDisabled(!useClip);
            float clip = useClip ? settings.AlphaCutoff : 0.3333f;
            ImGui::SetNextItemWidth(width - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
            changed = ImGui::SliderFloat("Opacity Mask Clip", &clip, 0.0f, 1.0f, "%.4f");
            if (changed && useClip)
            {
                settings.AlphaCutoff = clip;
            }
            TrackSettingsEdit(document, before, changed && useClip);
            ImGui::EndDisabled();
        }
        // マテリアルの現在の値（ガラス / 半透明 / テクスチャなど）から、ちゃんとしたグラフを作り直す
        // ---- 描画時評価（GPU）
        ImGui::SeparatorText("描画時評価（GPU）");
        {
            const MaterialGraphSettings before = settings;
            bool changed = ImGui::Checkbox("描画時に評価する", &settings.RuntimeEvaluation);
            ImGui::SetItemTooltip("グラフをエンジンのシェーダーで画素ごとに評価します（UE と同じ）。\n"
                                  "3D 位置で計算するので、ソリッドテクスチャ / ノイズに UV の継ぎ目や伸びが出ません。\n"
                                  "値を変えてもシェーダーの再コンパイルは不要です。\n"
                                  "オフ、または対応外のノードがあるときは、ベイクしたテクスチャを使います。");
            static const char* spaces[] = {"オブジェクト空間", "ワールド空間"};
            int space = std::clamp(settings.SolidSpace, 0, 1);
            ImGui::SetNextItemWidth(width);
            if (ImGui::BeginCombo("ソリッドの座標", spaces[space]))
            {
                for (int i = 0; i < 2; ++i)
                {
                    if (ImGui::Selectable(spaces[i], i == space) && i != space)
                    {
                        settings.SolidSpace = i;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("Solid / Noise / Convert To Solid / Triplanar の Position が未接続のときの座標。\n"
                                  "オブジェクト空間：動かしても模様が付いてくる（既定）\n"
                                  "ワールド空間：複数のオブジェクトで模様がつながる（動かすと模様が流れる）");
            TrackSettingsEdit(document, before, changed);
        }
        if (const auto* runtime = MaterialManager::FindRuntime(material))
        {
            if (runtime->GraphActive)
            {
                ImGui::TextColored(ImVec4(0.45f, 0.82f, 0.50f, 1.0f), "● GPU で評価中（命令 %d / レジスタ %d%s）",
                                   runtime->GraphInstructions, runtime->GraphRegisters,
                                   runtime->GraphUsesTime ? " / Time" : "");
            }
            else if (!runtime->GraphStatus.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.72f, 0.20f, 1.0f));
                ImGui::TextWrapped("ベイクを使用：%s", runtime->GraphStatus.c_str());
                ImGui::PopStyleColor();
            }
        }
        if (ImGui::Button("マテリアルの値からグラフを再生成"))
        {
            ImGui::OpenPopup("RegenerateGraph");
        }
        ImGui::SetItemTooltip("マテリアルアセットの現在の値（色 / テクスチャ / POM / ガラスの屈折・透過 / モード）から\n"
                              "グラフを作り直します（元に戻す: Ctrl+Z）");
        if (ImGui::BeginPopup("RegenerateGraph"))
        {
            ImGui::TextUnformatted("今のグラフを、マテリアルの値から作り直しますか？");
            if (ImGui::Button("再生成"))
            {
                const MaterialGraph before = document.Graph;
                MaterialGraph regenerated = MaterialGraph::FromMaterial(MaterialManager::GetRuntime(material).Editing);
                regenerated.Settings.BakeResolution = before.Settings.BakeResolution;
                regenerated.Settings.BakeTime = before.Settings.BakeTime;
                regenerated.Settings.AutoBake = before.Settings.AutoBake;
                PushHistory(document, before);
                document.Graph = std::move(regenerated);
                ++document.Graph.Revision;
                document.Dirty = true;
                document.RecoveryDirty = true;
                document.RestorePositions = true;
                document.FramesUntilFit = 2;
                document.Status = "マテリアルの値からグラフを再生成しました。";
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SeparatorText("テクスチャベイク（エンジン反映）");
        {
            static const int resolutions[] = {128, 256, 512, 1024, 2048};
            static const char* resolutionNames[] = {"128", "256", "512", "1024", "2048"};
            int current = 2;
            for (int i = 0; i < 5; ++i)
            {
                if (resolutions[i] == settings.BakeResolution)
                {
                    current = i;
                }
            }
            const MaterialGraphSettings before = settings;
            ImGui::SetNextItemWidth(width);
            bool changed = false;
            if (ImGui::BeginCombo("解像度", resolutionNames[current]))
            {
                for (int i = 0; i < 5; ++i)
                {
                    if (ImGui::Selectable(resolutionNames[i], i == current))
                    {
                        changed = i != current;
                        settings.BakeResolution = resolutions[i];
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("ピクセルごとの式（UV 演算 / Noise / Blend / Normal From Height など）を焼くテクスチャの解像度");
            TrackSettingsEdit(document, before, changed);
        }
        {
            const MaterialGraphSettings before = settings;
            ImGui::SetNextItemWidth(width);
            const bool changed = ImGui::DragFloat("ベイク時刻 (秒)", &settings.BakeTime, 0.05f, 0.0f, 3600.0f, "%.2f");
            ImGui::SetItemTooltip("Time / Panner / Rotator を含む式は、この時刻の値で焼かれます");
            TrackSettingsEdit(document, before, changed);
        }
        {
            const MaterialGraphSettings before = settings;
            const bool changed = ImGui::Checkbox("自動ベイク", &settings.AutoBake);
            ImGui::SetItemTooltip("グラフの変更が 0.5 秒落ち着いたら自動で焼き直します");
            TrackSettingsEdit(document, before, changed);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(document.PendingBakeMask == 0 || document.BakeJob.valid());
        if (ImGui::Button("今すぐベイク"))
        {
            document.BakeRequested = true;
        }
        ImGui::EndDisabled();
        if (document.PendingBakeMask != 0)
        {
            ImGui::TextDisabled("対象: %s", SlotList(document.PendingBakeMask).c_str());
        }
        else
        {
            ImGui::TextDisabled("ベイク不要（定数 / テクスチャを直接使用）");
        }
        if (!document.BakeStatus.empty() && document.PendingBakeMask != 0)
        {
            ImGui::TextWrapped("%s", document.BakeStatus.c_str());
        }
        for (const auto& warning : document.BakeWarnings)
        {
            ImGui::TextColored(ImVec4(0.98f, 0.72f, 0.20f, 1.0f), "⚠ %s", warning.c_str());
        }
        // ---- ライブ更新（Time を使う式をマテリアル上で動かす）
        ImGui::SeparatorText("ライブ更新（Time で動かす）");
        {
            const MaterialGraphSettings before = settings;
            bool changed = ImGui::Checkbox("ライブ更新", &settings.LiveUpdate);
            ImGui::SetItemTooltip("Time を使う式を、実行中に焼いたテクスチャへ書き込み続けてマテリアルを動かします\n"
                                  "（ワーカースレッドで評価。保存したベイク画像は「ベイク時刻」の値のまま）");
            static const int resolutions[] = {64, 128, 256, 512, 1024};
            static const char* resolutionNames[] = {"64", "128", "256", "512", "1024"};
            int current = 2;
            for (int i = 0; i < 5; ++i)
            {
                current = resolutions[i] == settings.LiveResolution ? i : current;
            }
            ImGui::SetNextItemWidth(width);
            if (ImGui::BeginCombo("ライブ解像度", resolutionNames[current]))
            {
                for (int i = 0; i < 5; ++i)
                {
                    if (ImGui::Selectable(resolutionNames[i], i == current) && i != current)
                    {
                        settings.LiveResolution = resolutions[i];
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("大きいほど細かいが CPU 負荷が増えます（ベイク解像度とは別）");
            ImGui::SetNextItemWidth(width);
            changed |= ImGui::SliderFloat("更新レート (回/秒)", &settings.LiveRate, 1.0f, 120.0f, "%.0f",
                                          ImGuiSliderFlags_AlwaysClamp);
            TrackSettingsEdit(document, before, changed);
        }
        if (const uint32_t live = MaterialGraphLive::ActiveSlots(material))
        {
            ImGui::TextColored(ImVec4(0.45f, 0.82f, 0.50f, 1.0f), "● 更新中: %s（%.1f ms / 回）", SlotList(live).c_str(),
                               MaterialGraphLive::LastSeconds(material) * 1000.0);
        }
        else if (MaterialGraphBake::TimeDependentSlots(document.Graph, document.PendingAlpha) != 0)
        {
            ImGui::TextDisabled(settings.LiveUpdate ? "ベイクが終わると動き始めます" : "ライブ更新はオフです");
        }
        else
        {
            ImGui::TextDisabled("Time を使う式はありません");
        }
    }

    std::shared_ptr<MaterialGraphGpu::Job> SubmitNodeBake(const MaterialGraphBake::NodeRequest& request);
    std::shared_ptr<MaterialGraphGpu::Job> SubmitBake(const MaterialGraphBake::Request& request);

    // ============================================================ ズームしても文字をくっきり描く
    // ノードエディターはキャンバスを頂点の拡大で描くので、文字は等倍で作ったグリフの拡大（ぼやけたラスター）になる。
    // ImGui 1.92 の動的フォントで、ズーム率に合わせた密度でグリフを作り直す（ベクターのアウトラインから再ラスター化）
    float CanvasFontDensity()
    {
        // 少しずつ変わるズームのたびにアトラスへ新しい大きさを焼かないよう、1/4 刻みに丸める
        // GetCurrentZoom は「1 画面画素あたりのキャンバス単位」（1 未満 = 拡大表示）なので、拡大率はその逆数
        const float zoom = Nodes::GetCurrentZoom();
        const float magnification = std::isfinite(zoom) && zoom > 1e-4f ? 1.0f / zoom : 1.0f;
        return std::clamp(std::ceil(magnification * 4.0f) / 4.0f, 1.0f, 4.0f);
    }

    void SetCanvasFontDensity(float density)
    {
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19200
        ImGui::SetFontRasterizerDensity(density);
#else
        (void)density;
#endif
    }

    // ============================================================ ノード内のプルダウン / ツールチップ
    // ノードエディターのキャンバスは座標変換（パン / ズーム）されているので、ノードの中で
    // BeginCombo / SetItemTooltip を開くと、ウィンドウがキャンバス座標の位置（画面の別の場所）に出てしまう。
    // ノード内では「開く要求」だけを記録し、Nodes::Suspend() の後（画面座標）で実際に開く。
    struct CanvasPopupRequest
    {
        bool Open = false;
        uint64_t Node = 0;
        ImVec2 Position{};
        float Width = 0.0f;
        float MaxHeight = 0.0f;    // 0 = 16 行（プルダウン）。カラーピッカーなどは FLT_MAX（内容に合わせる）
        float AnchorHeight = 0.0f; // 開いたボタンの高さ（画面の下で上に出すとき用）
        std::function<void(GraphDocument&, MaterialGraphNode&)> Body;
    };
    CanvasPopupRequest g_CanvasPopup;
    std::string g_CanvasTooltip;
    constexpr const char* kCanvasPopupId = "##MaterialNodeCombo";

    // ノード内のツールチップ（SetItemTooltip の代わり）
    void CanvasTooltip(const char* format, ...)
    {
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        {
            return;
        }
        char buffer[1024];
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(buffer, sizeof(buffer), format, arguments);
        va_end(arguments);
        g_CanvasTooltip = buffer;
    }

    // ノード内のプルダウン。ボタンを押したら、項目の一覧を画面座標のポップアップで開く
    void NodeCombo(MaterialGraphNode& node, const char* id, size_t valueIndex, int count, const char* (*name)(int),
                   const char* prefix, float width)
    {
        const int current = std::clamp(static_cast<int>(node.Value[valueIndex]), 0, count - 1);
        const std::string label = std::string(prefix) + name(current) + "  ▾##" + id;
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        const bool clicked = ImGui::Button(label.c_str(), ImVec2(width, 0.0f));
        ImGui::PopStyleVar();
        if (!clicked)
        {
            return;
        }
        const ImVec2 min = Nodes::CanvasToScreen(ImGui::GetItemRectMin());
        const ImVec2 max = Nodes::CanvasToScreen(ImGui::GetItemRectMax());
        g_CanvasPopup.Open = true;
        g_CanvasPopup.Node = node.Id;
        g_CanvasPopup.Position = ImVec2(min.x, max.y);
        g_CanvasPopup.Width = max.x - min.x;
        g_CanvasPopup.MaxHeight = 0.0f;
        g_CanvasPopup.AnchorHeight = max.y - min.y;
        g_CanvasPopup.Body = [valueIndex, count, name](GraphDocument& document, MaterialGraphNode& target) {
            const int selected = std::clamp(static_cast<int>(target.Value[valueIndex]), 0, count - 1);
            for (int i = 0; i < count; ++i)
            {
                if (ImGui::Selectable(name(i), i == selected) && i != selected)
                {
                    PushHistory(document, document.Graph);
                    target.Value[valueIndex] = static_cast<float>(i);
                    ++document.Graph.Revision;
                    ++document.Graph.ParameterRevision;
                    document.Dirty = true;
                    document.RecoveryDirty = true;
                }
                if (i == selected && ImGui::IsWindowAppearing())
                {
                    ImGui::SetScrollHereY();
                }
            }
        };
    }

    // Nodes::Suspend() と Resume() の間（画面座標）で呼ぶ
    void DrawCanvasOverlays(GraphDocument& document)
    {
        if (g_CanvasPopup.Open)
        {
            g_CanvasPopup.Open = false;
            ImGui::OpenPopup(kCanvasPopupId);
            ImGui::SetNextWindowPos(g_CanvasPopup.Position, ImGuiCond_Always);
        }
        const float maxHeight =
            g_CanvasPopup.MaxHeight > 0.0f ? g_CanvasPopup.MaxHeight : ImGui::GetTextLineHeightWithSpacing() * 16.0f;
        ImGui::SetNextWindowSizeConstraints(ImVec2(g_CanvasPopup.Width, 0.0f), ImVec2(FLT_MAX, maxHeight));
        if (ImGui::BeginPopup(kCanvasPopupId, ImGuiWindowFlags_AlwaysAutoResize))
        {
            // 位置を API で指定したポップアップは ImGui が画面内へ押し戻さないので、ここで収める
            // （画面の下 / 右端のノードからカラーピッカーを開くと見切れていた）
#ifdef IMGUI_HAS_VIEWPORT
            const ImGuiViewport* viewport = ImGui::GetWindowViewport();
#else
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
#endif
            if (viewport)
            {
                const ImVec2 size = ImGui::GetWindowSize();
                const ImVec2 position = ImGui::GetWindowPos();
                const ImVec2 workMin = viewport->WorkPos;
                const ImVec2 workMax(viewport->WorkPos.x + viewport->WorkSize.x,
                                     viewport->WorkPos.y + viewport->WorkSize.y);
                ImVec2 clamped = position;
                if (clamped.y + size.y > workMax.y)
                {
                    // 下に入らなければ、開いたボタンの上に出す
                    const float above = g_CanvasPopup.Position.y - g_CanvasPopup.AnchorHeight - size.y;
                    clamped.y = above >= workMin.y ? above : workMax.y - size.y;
                }
                clamped.x = (std::min)(clamped.x, workMax.x - size.x);
                clamped.x = (std::max)(clamped.x, workMin.x);
                clamped.y = (std::max)(clamped.y, workMin.y);
                if (clamped.x != position.x || clamped.y != position.y)
                {
                    ImGui::SetWindowPos(clamped);
                }
            }
            auto* node = document.Graph.FindNode(g_CanvasPopup.Node);
            if (node && g_CanvasPopup.Body)
            {
                g_CanvasPopup.Body(document, *node);
            }
            else
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        else if (document.PendingEdit && document.PendingValueNode == g_CanvasPopup.Node && !ImGui::IsAnyItemActive())
        {
            FinishValueEdit(document); // ポップアップ（カラーピッカー）を閉じた：編集を 1 回の Undo にまとめる
        }
        if (!g_CanvasTooltip.empty())
        {
            if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
            {
                ImGui::SetTooltip("%s", g_CanvasTooltip.c_str());
            }
            g_CanvasTooltip.clear();
        }
    }

    // 分割した出力ピン 1 行分（右寄せ：「名前 ○」）。sameRow = 入力ピンと同じ行に置く
    void DrawSplitOutputPin(GraphDocument& document, const MaterialGraphNode& node, size_t component,
                            const FrameInfo& frame, float nodeLeft, float width, float rowY, bool sameRow)
    {
        const uint64_t pin = node.SplitPins[component];
        const char* name = ComponentName(component);
        const int available = static_cast<int>(document.Graph.ResolveOutputType(node.Id));
        const bool missing = node.Kind != Kind::BreakVector ? component >= static_cast<size_t>((std::max)(available, 1))
                                                            : available > 1 && component >= static_cast<size_t>(available);
        const float iconWidth = ImGui::GetTextLineHeight() * 0.72f;
        const float labelWidth = ImGui::CalcTextSize(name).x;
        const float right = nodeLeft + width;
        float x = right - labelWidth - 4.0f - iconWidth;
        if (sameRow)
        {
            ImGui::SameLine(0.0f, 0.0f);
            x = (std::max)(x, ImGui::GetCursorScreenPos().x + 12.0f);
        }
        ImGui::SetCursorScreenPos(ImVec2(x, rowY));
        Nodes::BeginPin(Nodes::PinId(pin), Nodes::PinKind::Output);
        Nodes::PinPivotAlignment(ImVec2(1.0f, 0.5f));
        Nodes::PinPivotSize(ImVec2(0, 0));
        const bool connected = frame.ConnectedPins.contains(pin);
        if (missing)
        {
            ImGui::TextDisabled("%s", name);
        }
        else
        {
            ImGui::TextColored(ComponentColor(component), "%s", name);
        }
        ImGui::SameLine(0.0f, 4.0f);
        PinIcon(missing ? IM_COL32(90, 90, 96, 255) : ImGui::ColorConvertFloat4ToU32(ComponentColor(component)), connected);
        document.PinY[pin] = PinCenterY();
        Nodes::EndPin();
        CanvasTooltip("%s（Scalar）%s%s", name, missing ? "\n今の出力にはこの成分がありません（0 になります）" : "",
                      node.Kind == Kind::BreakVector ? "" : "\n右クリック →「ピンを統合」で元に戻します");
    }

    // 入力ピン横の数値ボックス。ドラッグ / ダブルクリックで入力。既定に戻すのはピンの右クリックメニュー
    void DrawPinValueBox(GraphDocument& document, MaterialGraphNode& node, size_t input)
    {
        const auto& definition = MaterialGraph::Definition(node.Kind);
        const float defaultValue = definition.Inputs[input].Default[0];
        const bool overridden = MaterialGraph::HasPinValue(node, input);
        float value = overridden ? node.PinValues[input] : defaultValue;
        const float before = value;
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::PushID(static_cast<int>(input));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.6f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 0.0f));
        if (!overridden)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }
        const bool changed = ImGui::DragFloat("##PinValue", &value, 0.01f, 0.0f, 0.0f, "%.3g");
        if (!overridden)
        {
            ImGui::PopStyleColor();
        }
        ImGui::PopStyleVar();
        // 編集の開始で 1 回だけ履歴の元を取る（ドラッグ中の全フレームを Undo に積まない）
        if (ImGui::IsItemActivated())
        {
            FinishEdits(document);
            document.PendingEdit = document.Graph;
            if (auto* original = document.PendingEdit->FindNode(node.Id); original && original->PinValues.size() > input)
            {
                original->PinValues[input] = overridden ? before : std::numeric_limits<float>::quiet_NaN();
            }
            document.PendingValueNode = node.Id;
            document.ValueChanged = false;
        }
        if (changed && std::isfinite(value))
        {
            if (!document.PendingEdit)
            {
                document.PendingEdit = document.Graph;
                document.PendingValueNode = node.Id;
            }
            if (node.PinValues.size() <= input)
            {
                node.PinValues.resize(input + 1, std::numeric_limits<float>::quiet_NaN());
            }
            node.PinValues[input] = value;
            ++document.Graph.Revision;
            document.ValueChanged = true;
            document.Dirty = true;
            document.RecoveryDirty = true;
        }
        if (document.PendingEdit && document.PendingValueNode == node.Id && ImGui::IsItemDeactivated())
        {
            FinishValueEdit(document);
        }
        CanvasTooltip("%s: 未接続のときの値（既定 %.3g）\nドラッグ / ダブルクリックで入力。ピンの右クリックで既定に戻す",
                      definition.Inputs[input].Name, defaultValue);
        ImGui::PopID();
    }

    // ノード内の色ボタン。ノードの中で ColorEdit のピッカーを開くと、キャンバスの座標変換のせいで
    // ポップアップが別の場所に出て見切れ、マウスも当たらない。ボタンだけをノード内に描き、
    // ピッカーは Nodes::Suspend() の後（画面座標）のポップアップで開く（NodeCombo と同じ仕組み）
    void NodeColorButton(GraphDocument& document, MaterialGraphNode& node, bool alpha, bool hdr, float width)
    {
        const ImVec4 color(node.Value[0], node.Value[1], node.Value[2], alpha ? node.Value[3] : 1.0f);
        ImGuiColorEditFlags buttonFlags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop;
        buttonFlags |= alpha ? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha;
        const bool clicked =
            ImGui::ColorButton("##Swatch", color, buttonFlags, ImVec2(width, ImGui::GetFrameHeight()));
        CanvasTooltip("クリックでカラーピッカーを開く\nR %.3f  G %.3f  B %.3f%s", node.Value[0], node.Value[1],
                      node.Value[2], alpha ? "（A はピッカーで編集）" : "");
        if (!clicked)
        {
            return;
        }
        FinishEdits(document);
        const ImVec2 min = Nodes::CanvasToScreen(ImGui::GetItemRectMin());
        const ImVec2 max = Nodes::CanvasToScreen(ImGui::GetItemRectMax());
        g_CanvasPopup.Open = true;
        g_CanvasPopup.Node = node.Id;
        g_CanvasPopup.Position = ImVec2(min.x, max.y + 2.0f);
        g_CanvasPopup.Width = 0.0f;
        g_CanvasPopup.MaxHeight = FLT_MAX;
        g_CanvasPopup.AnchorHeight = max.y - min.y + 4.0f;
        const std::array<float, 4> original = node.Value; // 「Original」（クリックで元に戻せる）
        g_CanvasPopup.Body = [alpha, hdr, original](GraphDocument& target, MaterialGraphNode& edited) {
            // 表示形式は指定しない（ピッカーの既定 = RGB / HSV / Hex の入力欄をすべて出す）
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_PickerHueBar;
            flags |= alpha ? (ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)
                           : ImGuiColorEditFlags_NoAlpha;
            if (hdr)
            {
                flags |= ImGuiColorEditFlags_HDR; // 数値入力は 1 を超えてもよい（発光色など）
            }
            const auto before = edited.Value;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13.0f);
            bool changed = ImGui::ColorPicker4("##NodeColorPicker", edited.Value.data(), flags, original.data());
            // ピッカーは複数の部品からなるので、開始 / 終了は TrackValueEdit と下の判定の両方で拾う
            for (auto& component : edited.Value)
            {
                if (!std::isfinite(component))
                {
                    edited.Value = before;
                    changed = false;
                    break;
                }
            }
            if (!alpha)
            {
                edited.Value[3] = before[3];
            }
            TrackValueEdit(target, edited, before, changed);
            if (target.PendingEdit && target.PendingValueNode == edited.Id && !ImGui::IsAnyItemActive())
            {
                FinishValueEdit(target); // ドラッグ 1 回 = Undo 1 回
            }
            ImGui::Separator();
            if (ImGui::Button("閉じる"))
            {
                ImGui::CloseCurrentPopup();
            }
        };
    }

    // 定数入力と Vector Parameter に共通の成分編集。値と Undo は node.Value を使う。
    void DrawComponentValueBox(GraphDocument& document, MaterialGraphNode& node, size_t input)
    {
        if (input >= node.Value.size())
        {
            return;
        }
        const auto before = node.Value;
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::PushID(static_cast<int>(input) + 1000);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.2f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 0.0f));
        const bool vectorComponent = node.Kind != Kind::Constant;
        if (vectorComponent)
        {
            PushComponentFieldStyle(input);
        }
        bool changed = ImGui::DragFloat("##Component", &node.Value[input], 0.01f, 0.0f, 0.0f, "%.3f");
        if (vectorComponent)
        {
            PopComponentFieldStyle();
        }
        ImGui::PopStyleVar();
        if (!std::isfinite(node.Value[input]))
        {
            node.Value = before;
            changed = false;
        }
        TrackValueEdit(document, node, before, changed);
        if (node.Kind == Kind::VectorParameter)
        {
            CanvasTooltip("%s の値\nドラッグ / ダブルクリックで入力。", ComponentName(input));
        }
        else
        {
            CanvasTooltip("未接続のときの値\nドラッグ / ダブルクリックで入力。ノードを繋ぐとその値を使います");
        }
        ImGui::PopID();
    }

    // ---- Material Output のベクトルピン（Base Color / Normal / Emissive / Absorption）の直値
    // ピンの横に色の四角を描き、クリックで画面座標のポップアップ（カラーピッカー / 法線の XYZ）を開く。
    // ポップアップを開いてから閉じるまでの編集は Undo 1 回にまとめる
    void DrawPinVectorBox(GraphDocument& document, MaterialGraphNode& node, size_t input)
    {
        const auto& definition = MaterialGraph::Definition(node.Kind);
        const bool overridden = MaterialGraph::HasPinVector(node, input);
        const auto current = MaterialGraph::UnconnectedInputValue(node, input);
        const bool normal = node.Kind == Kind::Output && input == static_cast<size_t>(MaterialOutputPin::Normal);
        const bool alpha = definition.Inputs[input].Type == MaterialGraphType::Vector4;
        ImVec4 swatch(current[0], current[1], current[2], alpha ? current[3] : 1.0f);
        if (normal)
        {
            // 法線は法線マップと同じ色（* 0.5 + 0.5）で見せる
            swatch = ImVec4(current[0] * 0.5f + 0.5f, current[1] * 0.5f + 0.5f, current[2] * 0.5f + 0.5f, 1.0f);
        }
        if (!overridden)
        {
            swatch.w *= 0.35f; // 未設定（既定値 / マテリアルの値）は薄く
        }
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::PushID(static_cast<int>(input) + 2000);
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop;
        flags |= alpha || !overridden ? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha;
        const float height = ImGui::GetFrameHeight();
        const bool clicked = ImGui::ColorButton("##PinVector", swatch, flags, ImVec2(height * 1.8f, height * 0.8f));
        CanvasTooltip(normal ? "%s: 未接続のときの法線（接空間）%s\nX %.3f  Y %.3f  Z %.3f\nクリックで編集。ピンの右クリックで既定に戻す"
                             : "%s: 未接続のときの値%s\nR %.3f  G %.3f  B %.3f\nクリックで編集。ピンの右クリックで既定に戻す",
                      definition.Inputs[input].Name, overridden ? "" : "（未設定 = 既定値）", current[0], current[1],
                      current[2]);
        ImGui::PopID();
        if (!clicked)
        {
            return;
        }
        FinishEdits(document);
        document.PendingEdit = document.Graph; // 開く前の状態（閉じたときに変更があれば Undo に積む）
        document.PendingValueNode = node.Id;
        document.ValueChanged = false;
        const ImVec2 min = Nodes::CanvasToScreen(ImGui::GetItemRectMin());
        const ImVec2 max = Nodes::CanvasToScreen(ImGui::GetItemRectMax());
        g_CanvasPopup.Open = true;
        g_CanvasPopup.Node = node.Id;
        g_CanvasPopup.Position = ImVec2(min.x, max.y + 2.0f);
        g_CanvasPopup.Width = 0.0f;
        g_CanvasPopup.MaxHeight = FLT_MAX;
        g_CanvasPopup.AnchorHeight = max.y - min.y + 4.0f;
        const auto original = current;
        g_CanvasPopup.Body = [input, normal, alpha, original](GraphDocument& target, MaterialGraphNode& edited) {
            const auto& pin = MaterialGraph::Definition(edited.Kind).Inputs[input];
            ImGui::TextUnformatted(pin.Name);
            ImGui::Separator();
            auto value = MaterialGraph::UnconnectedInputValue(edited, input);
            bool changed = false;
            if (normal)
            {
                for (size_t component = 0; component < 3; ++component)
                {
                    ImGui::PushID(static_cast<int>(component));
                    ImGui::TextColored(ComponentColor(component), "%s", ComponentName(component));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
                    PushComponentFieldStyle(component);
                    changed |= ImGui::DragFloat("##NormalComponent", &value[component], 0.005f, -1.0f, 1.0f, "%.3f");
                    PopComponentFieldStyle();
                    ImGui::PopID();
                }
                if (ImGui::Button("正規化"))
                {
                    const float length = sqrtf(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
                    if (length > 1e-6f)
                    {
                        value[0] /= length;
                        value[1] /= length;
                        value[2] /= length;
                        changed = true;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("平ら (0, 0, 1)"))
                {
                    value = {0.0f, 0.0f, 1.0f, 0.0f};
                    changed = true;
                }
            }
            else
            {
                ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_PickerHueBar;
                flags |= alpha ? (ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)
                               : ImGuiColorEditFlags_NoAlpha;
                if (input != static_cast<size_t>(MaterialOutputPin::BaseColor))
                {
                    flags |= ImGuiColorEditFlags_HDR; // Emissive / Absorption は 1 を超えてよい
                }
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13.0f);
                changed |= ImGui::ColorPicker4("##PinColor", value.data(), flags, original.data());
            }
            for (const float component : value)
            {
                changed &= std::isfinite(component);
            }
            if (changed)
            {
                if (!target.PendingEdit)
                {
                    target.PendingEdit = target.Graph;
                    target.PendingValueNode = edited.Id;
                }
                if (edited.PinVectors.size() <= input)
                {
                    std::array<float, 4> unset;
                    unset.fill(std::numeric_limits<float>::quiet_NaN());
                    edited.PinVectors.resize(input + 1, unset);
                }
                edited.PinVectors[input] = value;
                ++target.Graph.Revision;
                target.ValueChanged = true;
                target.Dirty = true;
                target.RecoveryDirty = true;
            }
            ImGui::Separator();
            ImGui::BeginDisabled(!MaterialGraph::HasPinVector(edited, input));
            if (ImGui::Button("既定に戻す"))
            {
                if (!target.PendingEdit)
                {
                    target.PendingEdit = target.Graph;
                    target.PendingValueNode = edited.Id;
                }
                MaterialGraph::ClearPinOverride(edited, input);
                ++target.Graph.Revision;
                target.ValueChanged = true;
                target.Dirty = true;
                target.RecoveryDirty = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("閉じる"))
            {
                ImGui::CloseCurrentPopup();
            }
        };
    }

    void DrawValueEditors(GraphDocument& document, MaterialGraphNode& node, float width)
    {
        const auto before = node.Value;
        bool changed = false;
        ImGui::SetNextItemWidth(width);
        switch (node.Kind)
        {
            case Kind::ScalarParameter:
                changed = ImGui::DragFloat("##Value", node.Value.data(), 0.01f);
                TrackValueEdit(document, node, before, changed);
                break;
            case Kind::Constant:
            case Kind::Constant2Vector:
                break; // 値は成分ピンの横のボックスで編集する
            case Kind::VectorParameter:
                for (size_t component = 0; component < 3; ++component)
                {
                    ImGui::TextColored(ComponentColor(component), "%s", ComponentName(component));
                    DrawComponentValueBox(document, node, component);
                }
                break;
            // 色：ボタンだけノード内に描き、ピッカーは画面座標のポップアップで開く（NodeColorButton）
            case Kind::Constant3Vector:
                NodeColorButton(document, node, false, true, width);
                break;
            case Kind::ColorParameter:
                NodeColorButton(document, node, true, false, width);
                break;
            case Kind::Constant4Vector:
                NodeColorButton(document, node, true, true, width);
                break;
            case Kind::TextureCoordinate:
                changed = ImGui::DragFloat2("Tiling", node.Value.data(), 0.01f);
                TrackValueEdit(document, node, before, changed);
                break;
            case Kind::ComponentMask: {
                for (int i = 0; i < 4; ++i)
                {
                    if (i % 2 != 0)
                    {
                        ImGui::SameLine(0.0f, 2.0f);
                    }
                    bool enabled = node.Value[i] > 0.5f;
                    ImGui::PushStyleColor(ImGuiCol_CheckMark, ComponentColor(i));
                    ImGui::PushStyleColor(ImGuiCol_Text, ComponentColor(i));
                    const bool toggled = ImGui::Checkbox(ComponentName(i), &enabled);
                    ImGui::PopStyleColor(2);
                    if (toggled)
                    {
                        PushHistory(document, document.Graph);
                        node.Value[i] = enabled ? 1.0f : 0.0f;
                        ValidateAfterEdit(document, node, before);
                    }
                }
                break;
            }
            case Kind::TextureSample:
            case Kind::TriplanarTexture: {
                auto path = MaterialManager::Registry().Resolve(node.Texture).filename().string();
                if (path.empty())
                {
                    path = "Textureをドロップ";
                }
                ImGui::Button(path.c_str(), ImVec2(width, 0));
                CanvasTooltip("Content Browser からテクスチャをドロップ");
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
                // UE の Sampler Type（Color = sRGB / Normal = 法線マップ / Linear = マスク・高さ）
                static const char* samplers[] = {"Sampler: Color", "Sampler: Normal", "Sampler: Linear"};
                int sampler = std::clamp(static_cast<int>(node.Value[0]), 0, 2);
                ImGui::SetNextItemWidth(width);
                changed = ImGui::SliderInt("##Sampler", &sampler, 0, 2, samplers[sampler]);
                if (changed)
                {
                    node.Value[0] = static_cast<float>(sampler);
                }
                TrackValueEdit(document, node, before, changed);
                CanvasTooltip("Color: カラー（sRGB）\nNormal: 法線マップ（-1〜1 に展開）\nLinear: マスク / 高さ / ラフネス");
                break;
            }
            // ---- オブジェクト / カメラ
            case Kind::ObjectBounds:
                NodeCombo(node, "BoundsOutput", 0, 4, &MaterialGraphMath::ObjectBoundsOutputName, "出力: ", width);
                CanvasTooltip("オブジェクトのローカルの範囲（AABB）。プレビューでは -1〜1 として評価します");
                break;
            case Kind::TransformVector:
                NodeCombo(node, "TransformVectorMode", 0, 4, &MaterialGraphMath::TransformVectorModeName, "", width);
                CanvasTooltip("方向（法線など）を変換します（平行移動は含まない）。プレビューでは変換しません");
                break;
            case Kind::TransformPosition:
                NodeCombo(node, "TransformPositionMode", 0, 2, &MaterialGraphMath::TransformPositionModeName, "", width);
                CanvasTooltip("位置を変換します（平行移動を含む）。プレビューでは変換しません");
                break;
            case Kind::HeightGradient:
                NodeCombo(node, "HeightSpace", 0, 3, &MaterialGraphMath::HeightGradientSpaceName, "基準: ", width);
                CanvasTooltip("ワールド Y：地面からの高さ\nローカル Y：オブジェクトの原点からの高さ\n"
                              "範囲内 0〜1：オブジェクトの下端 = 0、上端 = 1（Start 0 / End 1 で根元→先端）");
                break;
            case Kind::ConvertToSolid: {
                // 元画像：Content Browser からドロップ、または Value の入力から作成
                auto path = MaterialManager::Registry().Resolve(node.Texture).filename().string();
                if (path.empty())
                {
                    path = "Textureをドロップ";
                }
                ImGui::Button(path.c_str(), ImVec2(width, 0));
                CanvasTooltip("元にする 2D テクスチャ（Content Browser からドロップ）\n"
                              "ノイズなどは Value に繋いで「Value から作成」を押してください");
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
                                document.Dirty = true;
                                document.RecoveryDirty = true;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                NodeCombo(node, "SolidMethod", 0, static_cast<int>(MaterialSolidConvertMethod::Count),
                          &MaterialGraphMath::SolidConvertMethodName, "", width);
                CanvasTooltip("ストキャスティック（既定）: 3 方向から投影し、各面をヘックス タイルでランダムにずらして混ぜる。\n"
                              "  拡大しても同じ模様が並ばず、面による縞も出ない（Sharpness / Randomness / Seed を使用）\n"
                              "トライプラナー: 3 方向から投影して法線で混ぜる。元画像の見た目を保つが、拡大すると繰り返す\n"
                              "ボリューム（スタジオ互換）: スタジオの「画像からボリューム」と同じ。繰り返し・面による縞が出る\n"
                              "  （Octaves / Vertical Variation / Seed を使用）");
                {
                    // Normal は立体にすると向きが崩れるので Color / Linear のみ
                    const auto beforeSampler = node.Value;
                    int sampler = static_cast<int>(node.Value[1]) == static_cast<int>(MaterialSamplerType::Color) ? 0 : 1;
                    static const char* samplers[] = {"形式: Color（sRGB）", "形式: Linear"};
                    ImGui::SetNextItemWidth(width);
                    const bool samplerChanged = ImGui::SliderInt("##SolidSampler", &sampler, 0, 1, samplers[sampler]);
                    if (samplerChanged)
                    {
                        node.Value[1] = static_cast<float>(sampler == 0 ? MaterialSamplerType::Color : MaterialSamplerType::Linear);
                    }
                    TrackValueEdit(document, node, beforeSampler, samplerChanged);
                    CanvasTooltip("Color: 色の画像（sRGB）\nLinear: ノイズ / マスク / 高さ");
                }
                if (static_cast<int>(node.Value[0]) != static_cast<int>(MaterialSolidConvertMethod::Triplanar))
                {
                    const auto beforeSeed = node.Value;
                    int seed = static_cast<int>((std::max)(node.Value[2], 0.0f));
                    ImGui::SetNextItemWidth(width);
                    const bool seedChanged = ImGui::DragInt("##ConvertSolidSeed", &seed, 1.0f, 0, 16777215, "Seed %d");
                    if (seedChanged)
                    {
                        node.Value[2] = static_cast<float>(std::clamp(seed, 0, 16777215));
                    }
                    TrackValueEdit(document, node, beforeSeed, seedChanged);
                }
                {
                    bool gray = node.Value[3] > 0.5f;
                    if (ImGui::Checkbox("グレースケール", &gray))
                    {
                        PushHistory(document, document.Graph);
                        node.Value[3] = gray ? 1.0f : 0.0f;
                        ++document.Graph.Revision;
                        document.Dirty = true;
                        document.RecoveryDirty = true;
                    }
                    CanvasTooltip("RGB の平均（スタジオのボリューム = 密度と同じ）");
                }
                // Value（ノイズなど）→ 元画像を作る（UV 0〜1 で焼く。タイルできるノイズだと継ぎ目が出ない）
                const bool running = document.NodeJob.valid() && document.NodeJobNode == node.Id;
                ImGui::BeginDisabled(document.NodeJob.valid() || !document.Graph.InputLink(node.InputPins[0]));
                if (ImGui::Button(running ? "作成中..." : "Value から作成", ImVec2(width, 0)))
                {
                    const auto materialPath = MaterialManager::Registry().Resolve(document.Material);
                    if (materialPath.empty())
                    {
                        document.NodeStatus = "作成失敗: マテリアルのパスが見つかりません";
                    }
                    else
                    {
                        MaterialGraphBake::NodeRequest request;
                        request.Graph = document.Graph;
                        request.Node = node.Id;
                        request.Resolution = std::clamp(document.Graph.Settings.BakeResolution, 128, 2048);
                        request.Time = document.Graph.Settings.BakeTime;
                        request.Sampler = static_cast<int>(node.Value[1]) == static_cast<int>(MaterialSamplerType::Color)
                                              ? MaterialSamplerType::Color
                                              : MaterialSamplerType::Linear;
                        const auto stem = materialPath.stem().string();
                        request.File = materialPath.parent_path() / (stem + "_GraphBake") /
                                       (stem + "_Solid" + std::to_string(node.Id) + ".png");
                        if (auto* self = request.Graph.FindNode(node.Id))
                        {
                            self->Texture = 0;
                        }
                        for (const auto& item : request.Graph.Nodes)
                        {
                            if (UsesNodeTexture(item.Kind) && item.Texture != 0 &&
                                !request.TexturePaths.contains(item.Texture))
                            {
                                const auto texturePath = MaterialManager::Registry().Resolve(item.Texture);
                                if (!texturePath.empty())
                                {
                                    request.TexturePaths[item.Texture] = texturePath;
                                }
                            }
                        }
                        document.NodeJobNode = node.Id;
                        document.NodeStatus = "作成中...";
                        request.Gpu = SubmitNodeBake(request);
                        document.NodeJob = std::async(std::launch::async, [request = std::move(request)] {
                            BackgroundTasks::Scope task("テクスチャの作成");
                            return MaterialGraphBake::RunNode(request);
                        });
                    }
                }
                ImGui::EndDisabled();
                CanvasTooltip("Value に繋いだノイズなどを、UV 0〜1 の 2D 画像（ベイク解像度）に焼いて元画像にします\n"
                              "（入力を変えたら押し直し。タイル可能なノイズにすると継ぎ目が出ません）");
                if (node.Texture != 0)
                {
                    if (ImGui::Button("元画像を外す", ImVec2(width, 0)))
                    {
                        PushHistory(document, document.Graph);
                        node.Texture = 0;
                        ++document.Graph.Revision;
                        document.Dirty = true;
                        document.RecoveryDirty = true;
                    }
                }
                else
                {
                    ImGui::TextDisabled("元画像なし（Value をそのまま通す）");
                }
                if (document.NodeJobNode == node.Id && !document.NodeStatus.empty())
                {
                    ImGui::TextDisabled("%s", document.NodeStatus.c_str());
                }
                break;
            }
            case Kind::SolidTexture:
            case Kind::SolidPattern: {
                // プロシージャルスタジオの「立体テクスチャ」と同じ種類 / 基底 / フラクタル / シード
                NodeCombo(node, "SolidKind", 0, static_cast<int>(MaterialSolidKind::Count),
                          &MaterialGraphMath::SolidKindName, "", width);
                CanvasTooltip("立体テクスチャの種類（スタジオで立体に対応している 6 種）\n"
                              "大理石 / 木目: Turbulence・Ring Count、雲: Coverage・Softness を使います");
                const int kind = static_cast<int>(node.Value[0]);
                if (kind != static_cast<int>(MaterialSolidKind::Voronoi))
                {
                    NodeCombo(node, "SolidBasis", 1, MaterialGraphMath::kNoiseBasisCount,
                              &MaterialGraphMath::NoiseBasisName, "基底: ", width);
                    CanvasTooltip("ノイズの基底（御影石の粒、雲の形に使います）");
                    if (kind != static_cast<int>(MaterialSolidKind::Cloud))
                    {
                        NodeCombo(node, "SolidFractal", 2, MaterialGraphMath::kNoiseFractalCount,
                                  &MaterialGraphMath::NoiseFractalName, "フラクタル: ", width);
                    }
                }
                {
                    const auto beforeSeed = node.Value;
                    int seed = static_cast<int>((std::max)(node.Value[3], 0.0f));
                    ImGui::SetNextItemWidth(width);
                    const bool seedChanged = ImGui::DragInt("##SolidSeed", &seed, 1.0f, 0, 16777215, "Seed %d");
                    if (seedChanged)
                    {
                        node.Value[3] = static_cast<float>(std::clamp(seed, 0, 16777215));
                    }
                    TrackValueEdit(document, node, beforeSeed, seedChanged);
                }
                break;
            }
            case Kind::ConvertToTexture: {
                static const auto samplerName = [](int index) -> const char* {
                    static const char* names[] = {"Color（sRGB）", "Normal", "Linear"};
                    return names[std::clamp(index, 0, 2)];
                };
                static const auto resolutionName = [](int index) -> const char* {
                    static const char* names[] = {"256", "512", "1024", "2048"};
                    return names[std::clamp(index, 0, 3)];
                };
                NodeCombo(node, "ConvertSampler", 0, 3, +samplerName, "形式: ", width);
                CanvasTooltip("Color: 色（sRGB で保存）\nNormal: 法線（-1〜1 → 0〜1 で保存し、法線として読む）\n"
                              "Linear: ノイズ / マスク / 高さ（そのまま保存）");
                NodeCombo(node, "ConvertResolution", 1, 4, +resolutionName, "解像度: ", width);
                const bool running = document.NodeJob.valid() && document.NodeJobNode == node.Id;
                ImGui::BeginDisabled(document.NodeJob.valid());
                if (ImGui::Button(running ? "変換中..." : (node.Texture ? "テクスチャを更新" : "テクスチャに変換"),
                                  ImVec2(width, 0)))
                {
                    const auto materialPath = MaterialManager::Registry().Resolve(document.Material);
                    if (materialPath.empty())
                    {
                        document.NodeStatus = "変換失敗: マテリアルのパスが見つかりません";
                    }
                    else
                    {
                        static const int resolutions[] = {256, 512, 1024, 2048};
                        MaterialGraphBake::NodeRequest request;
                        request.Graph = document.Graph;
                        request.Node = node.Id;
                        request.Resolution = resolutions[std::clamp(static_cast<int>(node.Value[1]), 0, 3)];
                        request.Time = document.Graph.Settings.BakeTime;
                        request.Sampler =
                            static_cast<MaterialSamplerType>(std::clamp(static_cast<int>(node.Value[0]), 0, 2));
                        const auto stem = materialPath.stem().string();
                        request.File = materialPath.parent_path() / (stem + "_GraphBake") /
                                       (stem + "_Node" + std::to_string(node.Id) + ".png");
                        // 入力側は「未変換」として焼く（自分自身のテクスチャを読まない）
                        if (auto* self = request.Graph.FindNode(node.Id))
                        {
                            self->Texture = 0;
                        }
                        for (const auto& item : request.Graph.Nodes)
                        {
                            if (UsesNodeTexture(item.Kind) &&
                                item.Texture != 0 && !request.TexturePaths.contains(item.Texture))
                            {
                                const auto path = MaterialManager::Registry().Resolve(item.Texture);
                                if (!path.empty())
                                {
                                    request.TexturePaths[item.Texture] = path;
                                }
                            }
                        }
                        document.NodeJobNode = node.Id;
                        document.NodeStatus = "変換中...";
                        request.Gpu = SubmitNodeBake(request);
                        document.NodeJob = std::async(std::launch::async, [request = std::move(request)] {
                            BackgroundTasks::Scope task("テクスチャの作成");
                            return MaterialGraphBake::RunNode(request);
                        });
                    }
                }
                ImGui::EndDisabled();
                CanvasTooltip("Value の入力を UV 0〜1 でテクスチャに焼き、以後はそのテクスチャを読みます\n"
                              "（重いノイズを 1 枚の画像にする。Time は「ベイク時刻」の値で固定）");
                if (node.Texture != 0)
                {
                    if (ImGui::Button("変換を解除", ImVec2(width, 0)))
                    {
                        PushHistory(document, document.Graph);
                        node.Texture = 0;
                        ++document.Graph.Revision;
                        document.Dirty = true;
                        document.RecoveryDirty = true;
                    }
                    CanvasTooltip("テクスチャを使わず、入力をそのまま通します");
                }
                else
                {
                    ImGui::TextDisabled("未変換（入力をそのまま通す）");
                }
                if (document.NodeJobNode == node.Id && !document.NodeStatus.empty())
                {
                    ImGui::TextDisabled("%s", document.NodeStatus.c_str());
                }
                break;
            }
            case Kind::ConvertToNormalHeight:
            case Kind::ConvertToHeight:
            case Kind::ConvertToNormal: {
                // テクスチャのドロップ（Texture Sample と同じ）
                auto path = MaterialManager::Registry().Resolve(node.Texture).filename().string();
                if (path.empty())
                {
                    path = "Textureをドロップ";
                }
                ImGui::Button(path.c_str(), ImVec2(width, 0));
                CanvasTooltip("Content Browser から色画像（アルベドなど）をドロップ");
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
                                document.Dirty = true;
                                document.RecoveryDirty = true;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (node.Kind == Kind::ConvertToNormalHeight)
                {
                    ImGui::TextDisabled("旧ノード: %s", node.Value[0] > 0.5f ? "Normal 出力" : "Height 出力");
                    CanvasTooltip("今は Convert To Height / Convert To Normal に分かれています。\n"
                                  "このノードは読み込み互換のために残しています");
                }
                NodeCombo(node, "ConvertChannel", 1, MaterialGraphMath::kConvertChannelCount,
                          &MaterialGraphMath::ConvertChannelName, "チャンネル: ", width);
                CanvasTooltip("高さの元にするチャンネル（プロシージャルスタジオと同じ）");
                const auto toggle = [&](const char* label, size_t index, const char* tooltip) {
                    bool value = node.Value[index] > 0.5f;
                    if (ImGui::Checkbox(label, &value))
                    {
                        PushHistory(document, document.Graph);
                        node.Value[index] = value ? 1.0f : 0.0f;
                        ++document.Graph.Revision;
                        document.Dirty = true;
                        document.RecoveryDirty = true;
                    }
                    CanvasTooltip("%s", tooltip);
                };
                toggle("反転", 2, "明るい所を低く（凹）する");
                const bool normal = node.Kind == Kind::ConvertToNormal ||
                                    (node.Kind == Kind::ConvertToNormalHeight && node.Value[0] > 0.5f);
                if (normal)
                {
                    ImGui::SameLine();
                    toggle("Sobel", 3, "オン: 3x3（ノイズに強い） / オフ: 中央差分（鋭い）");
                }
                break;
            }
            case Kind::StaticSwitch: {
                bool value = node.Value[0] > 0.5f;
                changed = ImGui::Checkbox("Value", &value);
                if (changed)
                {
                    node.Value[0] = value ? 1.0f : 0.0f;
                }
                TrackValueEdit(document, node, before, changed);
                break;
            }
            case Kind::Pi:
                changed = ImGui::DragFloat("##Multiplier", node.Value.data(), 0.01f, 0.0f, 0.0f, "x %.3f");
                TrackValueEdit(document, node, before, changed);
                break;
            case Kind::Blend: {
                int mode = std::clamp(static_cast<int>(node.Value[0]), 0, static_cast<int>(MaterialBlendMode::Count) - 1);
                changed = ImGui::SliderInt("##BlendMode", &mode, 0, static_cast<int>(MaterialBlendMode::Count) - 1,
                                           MaterialGraphMath::BlendModeName(mode));
                if (changed)
                {
                    node.Value[0] = static_cast<float>(mode);
                }
                TrackValueEdit(document, node, before, changed);
                break;
            }
            case Kind::LinearGradient: {
                static const char* directions[] = {"方向: U（横）", "方向: V（縦）"};
                int direction = node.Value[0] > 0.5f ? 1 : 0;
                changed = ImGui::SliderInt("##Direction", &direction, 0, 1, directions[direction]);
                if (changed)
                {
                    node.Value[0] = static_cast<float>(direction);
                }
                TrackValueEdit(document, node, before, changed);
                break;
            }
            case Kind::ProceduralNoise:
            case Kind::FractalNoise: {
                // プロシージャルスタジオと同じ基底 / フラクタル / シード / タイル
                const bool fractal = node.Kind == Kind::FractalNoise;
                NodeCombo(node, "Basis", 0, MaterialGraphMath::kNoiseBasisCount, &MaterialGraphMath::NoiseBasisName, "",
                          width);
                CanvasTooltip("ノイズの基底（プロシージャルスタジオと同じ）");
                if (fractal)
                {
                    NodeCombo(node, "Fractal", 1, MaterialGraphMath::kNoiseFractalCount,
                              &MaterialGraphMath::NoiseFractalName, "", width);
                    CanvasTooltip("フラクタルの種類（Domain Warp は Warp Strength、Ridged 系は Ridge Offset を使います）");
                }
                {
                    const size_t seedIndex = fractal ? 2 : 1;
                    const auto beforeSeed = node.Value;
                    int seed = static_cast<int>((std::max)(node.Value[seedIndex], 0.0f));
                    ImGui::SetNextItemWidth(width);
                    const bool seedChanged = ImGui::DragInt("##Seed", &seed, 1.0f, 0, 16777215, "Seed %d");
                    if (seedChanged)
                    {
                        node.Value[seedIndex] = static_cast<float>(std::clamp(seed, 0, 16777215));
                    }
                    TrackValueEdit(document, node, beforeSeed, seedChanged);
                }
                {
                    const size_t tileIndex = fractal ? 3 : 2;
                    bool tileable = node.Value[tileIndex] > 0.5f;
                    if (ImGui::Checkbox("タイル", &tileable))
                    {
                        PushHistory(document, document.Graph);
                        node.Value[tileIndex] = tileable ? 1.0f : 0.0f;
                        ++document.Graph.Revision;
                        document.Dirty = true;
                        document.RecoveryDirty = true;
                    }
                    CanvasTooltip("UV 0〜1 で継ぎ目なく繰り返す（Frequency / Lacunarity は整数に丸めます）");
                }
                break;
            }
            case Kind::NoiseCombine: {
                int mode = std::clamp(static_cast<int>(node.Value[0]), 0, static_cast<int>(MaterialNoiseCombineMode::Count) - 1);
                changed = ImGui::SliderInt("##CombineMode", &mode, 0, static_cast<int>(MaterialNoiseCombineMode::Count) - 1,
                                           MaterialGraphMath::NoiseCombineModeName(mode));
                if (changed)
                {
                    node.Value[0] = static_cast<float>(mode);
                }
                TrackValueEdit(document, node, before, changed);
                CanvasTooltip("Mix: Factor で A→B\nAdd〜Difference: A に B を合成し、Factor で効き具合\n"
                                      "Height Blend: 高い方を残す（Factor = 境界の幅）");
                bool clampResult = node.Value[1] > 0.5f;
                if (ImGui::Checkbox("0〜1 に収める", &clampResult))
                {
                    PushHistory(document, document.Graph);
                    node.Value[1] = clampResult ? 1.0f : 0.0f;
                    ++document.Graph.Revision;
                    document.Dirty = true;
                    document.RecoveryDirty = true;
                }
                break;
            }
            case Kind::Voronoi: {
                static const char* outputs[] = {"出力: 距離 (F1)", "出力: セル値"};
                int output = node.Value[0] > 0.5f ? 1 : 0;
                changed = ImGui::SliderInt("##VoronoiOutput", &output, 0, 1, outputs[output]);
                if (changed)
                {
                    node.Value[0] = static_cast<float>(output);
                }
                TrackValueEdit(document, node, before, changed);
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

    // 右下の斜めグリップで、幅と高さを独立に変更する。
    void DrawResizeHandles(GraphDocument& document, MaterialGraphNode& node, ImVec2 contentMin, float contentBottom)
    {
        const float corner = 12.0f;
        const float width = NodeWidth(node);
        const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
        const ImVec2 max(contentMin.x + width, contentBottom);
        ImGui::SetCursorScreenPos(ImVec2(max.x - corner, max.y - corner));
        ImGui::InvisibleButton("##ResizeCorner", ImVec2(corner, corner));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        }
        if (ImGui::IsItemActivated())
        {
            if (!document.PendingMove)
            {
                document.PendingMove = document.Graph;
            }
            document.ResizeStartMouse = ImGui::GetMousePos();
            document.ResizeStartSize = ImVec2(width, contentBottom - contentMin.y);
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            node.Width = 0.0f;
            node.Height = 0.0f;
            document.Dirty = true;
            document.RecoveryDirty = true;
        }
        else if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            // 掴んだ位置からの移動量を使い、押した瞬間のサイズの跳ねを防ぐ。
            const ImVec2 mouse = ImGui::GetMousePos();
            const float newWidth =
                std::clamp(document.ResizeStartSize.x + mouse.x - document.ResizeStartMouse.x, 96.0f, 512.0f);
            const float newHeight =
                std::clamp(document.ResizeStartSize.y + mouse.y - document.ResizeStartMouse.y, 48.0f, 2048.0f);
            if (fabsf(newWidth - NodeWidth(node)) > 0.5f || fabsf(newHeight - NodeHeight(node)) > 0.5f)
            {
                node.Width = newWidth;
                node.Height = newHeight;
                document.Dirty = true;
                document.RecoveryDirty = true;
            }
        }
        CanvasTooltip("ドラッグ: 幅と高さを変更 / ダブルクリック: 既定のサイズ");
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 color = hot ? IM_COL32(255, 190, 60, 255) : IM_COL32(150, 150, 150, 160);
        for (int i = 1; i <= 3; ++i)
        {
            const float offset = i * 3.5f;
            draw->AddLine(ImVec2(max.x - offset, max.y - 1.0f), ImVec2(max.x - 1.0f, max.y - offset), color, 1.2f);
        }
        ImGui::SetCursorScreenPos(savedCursor);
        ImGui::Dummy(ImVec2(0.0f, 0.0f)); // カーソル復帰後にレイアウトの境界を確定する。
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
            CanvasTooltip(previewCollapsed ? "プレビューを表示" : "プレビューを隠す");
            ImGui::SameLine(0.0f, 4.0f);
        }
        ImGui::TextUnformatted(node.Name.c_str());
        if (node.Name != definition.Name)
        {
            CanvasTooltip("%s", definition.Name);
        }
        float headerBottom = ImGui::GetItemRectMax().y;
        if (node.OutputPin && node.SplitPins.empty()) // 分割中は右側の X / Y / Z / W に置き換わる
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
            CanvasTooltip("出力: %s%s", TypeName(type),
                          document.Graph.CanSplitOutput(node.Id) ? "\n右クリック →「ピンを分割」で X / Y / Z / W に分けられます" : "");
            headerBottom = (std::max)(headerBottom, ImGui::GetItemRectMax().y);
        }
        ImGui::Dummy(ImVec2(width, 4.0f)); // ヘッダーの下の余白（ノード幅もここで確定）

        // ---- 値の編集
        DrawValueEditors(document, node, width);

        // ---- 入力ピン（左）と、分割した出力ピン（右：UE の Split Struct Pin と同じ並び）
        const size_t rowCount = (std::max)(node.InputPins.size(), node.SplitPins.size());
        for (size_t i = 0; i < rowCount; ++i)
        {
            const float rowY = ImGui::GetCursorScreenPos().y;
            if (i >= node.InputPins.size())
            {
                DrawSplitOutputPin(document, node, i, frame, start.x, width, rowY, false);
                continue;
            }
            const uint64_t pin = node.InputPins[i];
            const bool connected = frame.ConnectedPins.contains(pin);
            MaterialGraphType type = definition.Inputs[i].Type;
            if (connected)
            {
                const auto source = frame.InputSource.find(pin);
                if (source != frame.InputSource.end())
                {
                    const auto found = frame.PinTypes.find(source->second);
                    type = found != frame.PinTypes.end() ? found->second : type;
                }
            }
            // Material Output：マテリアルモードで使えないピンは灰色（接続も不可。既存の接続は無効になる）
            const char* inactiveReason =
                node.Kind == Kind::Output && i < static_cast<size_t>(MaterialOutputPin::Count)
                    ? MaterialGraph::OutputPinInactiveReason(static_cast<MaterialOutputPin>(i), document.Graph.Settings)
                    : nullptr;
            Nodes::BeginPin(Nodes::PinId(pin), Nodes::PinKind::Input);
            Nodes::PinPivotAlignment(ImVec2(0.0f, 0.5f));
            Nodes::PinPivotSize(ImVec2(0, 0));
            const bool componentInput = MaterialGraph::IsComponentConstant(node.Kind) && node.Kind != Kind::Constant;
            const char* inputName = componentInput ? ComponentName(i) : definition.Inputs[i].Name;
            const ImU32 inputColor = componentInput ? ImGui::ColorConvertFloat4ToU32(ComponentColor(i)) : TypeColor(type);
            PinIcon(inactiveReason ? IM_COL32(90, 90, 96, 255) : inputColor, connected);
            document.PinY[pin] = PinCenterY();
            ImGui::SameLine(0.0f, 4.0f);
            if (inactiveReason)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.42f, 0.45f, 1.0f));
                ImGui::TextUnformatted(inputName);
                ImGui::PopStyleColor();
            }
            else if (componentInput)
            {
                ImGui::TextColored(ComponentColor(i), "%s", inputName);
            }
            else if (connected)
            {
                ImGui::TextUnformatted(definition.Inputs[i].Name);
            }
            else
            {
                ImGui::TextDisabled("%s", definition.Inputs[i].Name);
            }
            Nodes::EndPin();
            if (inactiveReason)
            {
                CanvasTooltip("%s%s", inactiveReason, connected ? "\n（接続は残っていますが使われません）" : "");
            }
            // 未接続の Scalar ピン：横に数値ボックス（UE と同じ。接続すると隠れる）
            if (!connected && MaterialGraph::IsComponentConstant(node.Kind))
            {
                DrawComponentValueBox(document, node, i); // 定数の成分（UE の Constant の値）
            }
            else if (!connected && !inactiveReason && MaterialGraph::IsPinVectorEditable(node.Kind, i))
            {
                DrawPinVectorBox(document, node, i); // Material Output の色 / 法線の直値
            }
            else if (!connected && !inactiveReason && MaterialGraph::IsPinValueEditable(node.Kind, i))
            {
                DrawPinValueBox(document, node, i);
            }
            if (i < node.SplitPins.size())
            {
                DrawSplitOutputPin(document, node, i, frame, start.x, width, rowY, true);
            }
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

    // ピンの右クリックメニュー（UE と同じ：出力は「ピンを分割 / 統合」、入力は「既定値に戻す」）
    void DrawPinMenu(GraphDocument& document)
    {
        bool input = false;
        size_t index = 0;
        const auto* pinNode = document.Graph.FindPinNode(document.ContextPin, &input, &index);
        if (!pinNode)
        {
            ImGui::CloseCurrentPopup();
            return;
        }
        const uint64_t nodeId = pinNode->Id;
        if (!input)
        {
            if (index == 0)
            {
                const int count = static_cast<int>(document.Graph.ResolveOutputType(nodeId));
                static const char* labels[] = {"ピンを分割", "ピンを分割", "ピンを分割（X / Y）", "ピンを分割（X / Y / Z）",
                                               "ピンを分割（X / Y / Z / W）"};
                if (ImGui::MenuItem(labels[std::clamp(count, 0, 4)], nullptr, false,
                                    document.Graph.CanSplitOutput(nodeId)))
                {
                    const auto before = document.Graph;
                    if (document.Graph.SplitOutput(nodeId, document.Error))
                    {
                        PushHistory(document, before);
                        document.Dirty = true;
                        document.RecoveryDirty = true;
                    }
                }
                ImGui::SetItemTooltip("float2 以上の出力を、同じノードの X / Y / Z / W ピンに分けます\n"
                                      "（今の出力の接続は外れます。Ctrl+Z で戻せます）");
            }
            else
            {
                if (ImGui::MenuItem("ピンを統合", nullptr, false, pinNode->Kind != Kind::BreakVector))
                {
                    const auto before = document.Graph;
                    if (document.Graph.RecombineOutput(nodeId))
                    {
                        PushHistory(document, before);
                        document.Dirty = true;
                        document.RecoveryDirty = true;
                    }
                }
                ImGui::SetItemTooltip(pinNode->Kind == Kind::BreakVector ? "Break Vector は常に分割されています"
                                                                         : "X / Y / Z / W を 1 つの出力に戻します\n"
                                                                           "（成分ピンの接続は外れます）");
            }
        }
        else if (ImGui::MenuItem("既定値に戻す", nullptr, false,
                                 MaterialGraph::HasPinValue(*pinNode, index) ||
                                     MaterialGraph::HasPinVector(*pinNode, index)))
        {
            PushHistory(document, document.Graph);
            if (auto* node = document.Graph.FindNode(nodeId))
            {
                MaterialGraph::ClearPinOverride(*node, index);
            }
            ++document.Graph.Revision;
            document.Dirty = true;
            document.RecoveryDirty = true;
        }
        bool linked = false;
        for (const auto& link : document.Graph.Links)
        {
            linked |= link.SourcePin == document.ContextPin || link.TargetPin == document.ContextPin;
        }
        if (ImGui::MenuItem("接続を切る", nullptr, false, linked))
        {
            const auto before = document.Graph;
            std::erase_if(document.Graph.Links, [&](const auto& link) {
                return link.SourcePin == document.ContextPin || link.TargetPin == document.ContextPin;
            });
            ++document.Graph.Revision;
            PushHistory(document, before);
            document.Dirty = true;
            document.RecoveryDirty = true;
        }
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
                pins.insert(node.SplitPins.begin(), node.SplitPins.end());
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
            destination->PinValues = source.PinValues;
            destination->PinVectors = source.PinVectors;
            if (!source.SplitPins.empty() && destination->SplitPins.empty())
            {
                // 分割をそのまま複製する（接続前で型が決まっていなくても、同じ成分数で分ける）
                std::string splitError;
                candidate.SplitOutput(id, splitError, static_cast<int>(source.SplitPins.size()));
                destination = candidate.FindNode(id);
            }
            remappedPins[source.OutputPin] = destination->OutputPin;
            for (size_t i = 0; i < source.SplitPins.size() && i < destination->SplitPins.size(); ++i)
            {
                remappedPins[source.SplitPins[i]] = destination->SplitPins[i];
            }
            for (size_t i = 0; i < source.InputPins.size(); ++i)
            {
                remappedPins[source.InputPins[i]] = destination->InputPins[i];
            }
            selection.push_back(id);
        }
        for (const auto& link : copiedLinks)
        {
            const auto sourcePin = remappedPins.find(link.SourcePin);
            const auto targetPin = remappedPins.find(link.TargetPin);
            if (sourcePin == remappedPins.end() || targetPin == remappedPins.end())
            {
                continue; // 複製先で成分数が合わなかった分割ピン
            }
            if (!candidate.Connect(sourcePin->second, targetPin->second, document.Error))
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
                    if (node->OwnsOutput(link.SourcePin))
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
                    if (node.OwnsOutput(link.SourcePin))
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
                    const float source = pinY(outgoing->SourcePin);
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
                    const float source = pinY(link->SourcePin);
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
    const char* const kCategoryOrder[] = {"Parameters", "Constants", "Texture",         "Coordinates", "Object", "Math",
                                          "Vector",     "Color",     "Normal / Height", "Procedural",  "Utility",
                                          "Comment"};

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
                        if (document.Graph.CanConnectActive(document.PendingConnectPin, target, error))
                        {
                            document.Graph.Connect(document.PendingConnectPin, target, error);
                            break;
                        }
                    }
                }
                else
                {
                    // 分割ノード（Break Vector）は X から順に、それ以外はまとめた出力から繋ぐ
                    std::vector<uint64_t> outputs = added->SplitPins;
                    if (added->SplitPins.empty() && added->OutputPin)
                    {
                        outputs.push_back(added->OutputPin);
                    }
                    for (const auto output : outputs)
                    {
                        if (document.Graph.CanConnectActive(output, document.PendingConnectPin, error))
                        {
                            document.Graph.Connect(output, document.PendingConnectPin, error);
                            break;
                        }
                    }
                }
            }
            document.PendingConnectPin = 0;
        }
        PushHistory(document, before);
        document.RestorePositions = true;
        document.PendingSelection = {id};
        return id;
    }

    const char* NodeSearchAliases(Kind kind)
    {
        switch (kind)
        {
            case Kind::Add:
                return "plus addition 加算 足す";
            case Kind::Subtract:
                return "sub minus subtraction 減算 引く";
            case Kind::Multiply:
                return "mul mult times multiplication 乗算 掛ける";
            case Kind::Divide:
                return "div division 除算 割る";
            case Kind::Power:
                return "pow exponent 累乗";
            case Kind::Fmod:
                return "mod modulo remainder 余り 剰余";
            case Kind::OneMinus:
                return "oneminus invert inverse 反転";
            case Kind::Lerp:
                return "mix interpolate interpolation 補間";
            case Kind::TextureSample:
                return "tex texture sample テクスチャ";
            case Kind::TextureCoordinate:
                return "uv texcoord texture coordinate テクスチャ座標";
            case Kind::Constant:
                return "const float value 定数";
            case Kind::Constant2Vector:
                return "vec2 float2 constant2 vector2";
            case Kind::Constant3Vector:
                return "vec3 float3 constant3 vector3 rgb";
            case Kind::Constant4Vector:
                return "vec4 float4 constant4 vector4 rgba";
            case Kind::ScalarParameter:
                return "sp scalarparam scalar parameter スカラー パラメータ";
            case Kind::VectorParameter:
                return "vp vectorparam vector parameter ベクトル パラメータ";
            case Kind::ComponentMask:
                return "mask swizzle component チャンネル マスク";
            case Kind::AppendVector:
                return "append combine join 結合";
            case Kind::BreakVector:
                return "split break separate 分割";
            case Kind::Normalize:
                return "norm normalize 正規化";
            case Kind::Dot:
                return "dot 内積";
            case Kind::CrossProduct:
                return "cross 外積";
            case Kind::Group:
                return "comment group コメント グループ";
            default:
                return "";
        }
    }

    std::optional<Kind> SymbolSearchKind(const char* text)
    {
        std::string query(text);
        const size_t first = query.find_first_not_of(" \t");
        if (first == std::string::npos)
        {
            return std::nullopt;
        }
        query = query.substr(first, query.find_last_not_of(" \t") - first + 1);
        // 「-」は ImGuiTextFilter では除外条件になるため、記号の完全一致を先に扱う。
        static const std::pair<const char*, Kind> symbols[] = {
            {"+", Kind::Add}, {"-", Kind::Subtract}, {"−", Kind::Subtract},
            {"*", Kind::Multiply}, {"×", Kind::Multiply}, {"/", Kind::Divide}, {"÷", Kind::Divide},
            {"^", Kind::Power}, {"**", Kind::Power}, {"%", Kind::Fmod}, {"1-x", Kind::OneMinus},
            {"1", Kind::Constant}, {"2", Kind::Constant2Vector},
            {"3", Kind::Constant3Vector}, {"4", Kind::Constant4Vector}};
        for (const auto& [symbol, kind] : symbols)
        {
            if (query == symbol)
            {
                return kind;
            }
        }
        return std::nullopt;
    }

    void DrawAddNodeMenu(GraphDocument& document)
    {
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(240.0f);
        const bool enter = ImGui::InputTextWithHint("##Search", "ノードを検索（例: +, mul, tex, uv）", document.Search,
                                                    sizeof(document.Search), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGuiTextFilter filter(document.Search);
        const auto symbolKind = SymbolSearchKind(document.Search);
        const auto label = [](const MaterialNodeDefinition& definition) {
            return definition.Kind == Kind::Group ? "Comment" : definition.Name;
        };
        if (filter.IsActive())
        {
            // 検索中はカテゴリを開かずに一覧表示
            bool first = true;
            for (const auto& definition : MaterialGraph::Definitions())
            {
                if (definition.Kind == Kind::Output || definition.Kind == Kind::ConvertToNormalHeight)
                {
                    continue;
                }
                const std::string searchText = std::string(label(definition)) + " " + NodeSearchAliases(definition.Kind);
                const bool matches = symbolKind ? definition.Kind == *symbolKind : filter.PassFilter(searchText.c_str());
                if (!matches)
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
        const float canvasFontDensity = CanvasFontDensity();
        SetCanvasFontDensity(canvasFontDensity); // キャンバス内の文字（ノード名 / ピン名 / 数値）
        const bool canvasHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        const bool canvasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        if (document.RestorePositions)
        {
            RestorePositions(document);
        }
        // 型の解決は上流ノードも辿るため、内容が変わったときだけ行う。
        // Undo / 読み直しでノード配列が置き換わった場合も、ポインターを作り直す。
        const uint64_t canvasSignature = MaterialGraphPreview::ContentSignature(document.Graph);
        if (document.CanvasSignature != canvasSignature || document.CanvasNodeStorage != document.Graph.Nodes.data())
        {
            document.CanvasFrame = BuildFrameInfo(document.Graph);
            document.CanvasSignature = canvasSignature;
            document.CanvasNodeStorage = document.Graph.Nodes.data();
        }
        auto& frame = document.CanvasFrame;
        UpdateFrameSelection(frame);
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
            const auto typeFound = frame.PinTypes.find(link.SourcePin);
            const auto type = typeFound != frame.PinTypes.end() ? typeFound->second : MaterialGraphType::Numeric;
            const bool highlight = frame.Selected.contains(sourceNode) || frame.Selected.contains(targetNode) ||
                                   (frame.Hovered != 0 && (frame.Hovered == sourceNode || frame.Hovered == targetNode));
            const bool dim = !frame.Selected.empty() && !highlight;
            ImVec4 color = ImGui::ColorConvertU32ToFloat4(TypeColor(type));
            if (source != frame.PinOwner.end())
            {
                const auto& components = source->second->SplitPins;
                const auto component = std::find(components.begin(), components.end(), link.SourcePin);
                if (component != components.end())
                {
                    color = ComponentColor(static_cast<size_t>(component - components.begin()));
                }
            }
            color.w = dim ? 0.35f : (highlight ? 1.0f : 0.85f);
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
                if (document.Graph.CanConnectActive(source, target, error))
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

        // ---- 線のダブルクリックで中継点 / コメントのダブルクリックで名前変更
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
        Nodes::PinId contextPin;
        const bool pinMenu = Nodes::ShowPinContextMenu(&contextPin);
        Nodes::Suspend();
        SetCanvasFontDensity(1.0f); // ポップアップ / ツールチップは画面座標（等倍）
        DrawCanvasOverlays(document);
        if (pinMenu)
        {
            document.ContextPin = contextPin.Get();
            ImGui::OpenPopup("MaterialPinMenu");
        }
        if (ImGui::BeginPopup("MaterialPinMenu"))
        {
            DrawPinMenu(document);
            ImGui::EndPopup();
        }
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
            if (sourceNode && sourceNode->OutputPin && sourceNode->SplitPins.empty() && ImGui::BeginMenu("出力の接続先"))
            {
                const auto sourcePin = sourceNode->OutputPin;
                for (const auto& target : document.Graph.Nodes)
                {
                    const auto& definition = MaterialGraph::Definition(target.Kind);
                    for (size_t i = 0; i < target.InputPins.size(); ++i)
                    {
                        const auto pin = target.InputPins[i];
                        std::string error;
                        const bool compatible = document.Graph.CanConnectActive(sourcePin, pin, error);
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
        SetCanvasFontDensity(canvasFontDensity);
        Nodes::End();
        // 背景クリックは End() 内で確定する。同じフレームのキー状態と画面座標を使う。
        const auto& io = ImGui::GetIO();
        if (Nodes::GetBackgroundClickButtonIndex() == ImGuiMouseButton_Left &&
            !io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        {
            bool found = false;
            const Kind kind = PlacementKind(found);
            if (found)
            {
                document.PendingConnectPin = 0;
                AddNodeAt(document, kind, Nodes::ScreenToCanvas(ImGui::GetMousePos()));
            }
        }
        // End() が画面座標に戻し、キャンバス全体をアイテムとして提出する。
        // ノード内の既存ドロップ先は矩形が小さいため、そちらの割り当てが優先される。
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
                        document.PendingConnectPin = 0;
                        const ImVec2 position = Nodes::ScreenToCanvas(ImGui::GetMousePos());
                        const auto id = AddNodeAt(document, Kind::TextureSample, position);
                        if (auto* node = document.Graph.FindNode(id))
                        {
                            node->Texture = asset.Handle;
                        }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        SetCanvasFontDensity(1.0f);
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
    uint64_t MixHash(uint64_t hash, uint64_t value)
    {
        hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
        return hash;
    }

    // ベイク結果を受け取り、テクスチャアセットとして登録してエンジンに再読み込みさせる
    bool PollBakeJob(AssetHandle material, GraphDocument& document)
    {
        if (!document.BakeJob.valid() ||
            document.BakeJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return false;
        }
        MaterialGraphBake::Result result = document.BakeJob.get();
        document.BakeWarnings = result.Warnings;
        if (!result.Success)
        {
            document.FailedBakeSignature = result.Signature;
            document.BakeStatus = "ベイク失敗: " + result.Error;
            return false;
        }
        MaterialGraphPreview::BakedTextures baked;
        baked.Signature = result.Signature;
        baked.EmissivePeak = result.EmissivePeak;
        auto& registry = MaterialManager::Registry();
        for (size_t slot = 0; slot < MaterialGraphBake::kSlotCount; ++slot)
        {
            if (result.Files[slot].empty())
            {
                continue;
            }
            std::string error;
            const AssetHandle handle = registry.Register(result.Files[slot], AssetType::Texture, error);
            if (handle == 0)
            {
                document.BakeWarnings.push_back(std::string(MaterialGraphBake::SlotName(static_cast<MaterialTextureSlot>(slot))) +
                                                ": " + error);
                continue;
            }
            baked.Textures[slot] = handle;
            baked.Mask |= 1u << slot;
            // 同じパス・同じ SRV のまま中身だけ差し替える（SRV を消費しない）
            TextureManager::RequestReload(result.Files[slot]);
            MaterialGraphTextures::Invalidate(handle);
            AssetThumbnailManager::MarkDirty(handle);
        }
        document.Baked = baked;
        AssetThumbnailManager::MarkDirty(material);
        char status[160];
        snprintf(status, sizeof(status), "ベイク完了: %s（%dpx / %.2f 秒 / %s）", SlotList(baked.Mask).c_str(),
                 result.Resolution, result.Seconds, result.Gpu ? "GPU" : "CPU");
        document.BakeStatus = status;
        return true;
    }

    uint64_t BakeSignature(const GraphDocument& document, uint64_t contentSignature)
    {
        uint64_t hash = MixHash(contentSignature, document.PendingBakeMask);
        return MixHash(hash, static_cast<uint64_t>(document.PendingAlpha) + 1);
    }

    // ---- GPU 評価（ベイク / ノードの作成）。命令列を作れないグラフ（対応外のノード）は nullptr = CPU で評価
    std::shared_ptr<MaterialGraphGpu::Job> SubmitNodeBake(const MaterialGraphBake::NodeRequest& request)
    {
        if (!MaterialGraphGpu::IsAvailable())
        {
            return nullptr;
        }
        MaterialGraphVm::Options options;
        options.TargetNode = request.Node; // 自分自身のテクスチャは外してあるので、入力（Value）がそのまま出る
        const auto program = MaterialGraphVm::Compile(request.Graph, &MaterialManager::ResolveGraphTexture, options);
        if (!program.Success)
        {
            return nullptr;
        }
        MaterialGraphGpu::BakeDesc desc;
        desc.Program = program.Texels;
        desc.JobMode = MaterialGraphGpu::Mode::BakeNode;
        desc.Resolution = request.Resolution;
        desc.Time = request.Time;
        desc.Flags = request.Sampler == MaterialSamplerType::Normal ? 1u : 0u;
        desc.SlotMask = static_cast<uint32_t>(program.TargetDims) << 4u;
        return MaterialGraphGpu::Submit(std::move(desc));
    }

    std::shared_ptr<MaterialGraphGpu::Job> SubmitBake(const MaterialGraphBake::Request& request)
    {
        if (!MaterialGraphGpu::IsAvailable() || request.Mask == 0)
        {
            return nullptr;
        }
        MaterialGraphVm::Options options;
        options.IncludeDefaults = true; // 未接続の出力も CPU のベイクと同じ既定値にする
        const auto program = MaterialGraphVm::Compile(request.Graph, &MaterialManager::ResolveGraphTexture, options);
        if (!program.Success)
        {
            return nullptr;
        }
        // 旧グラフの「Color サンプラーの法線マップ」（CPU のベイクと同じ判定）
        bool legacyColorNormal = false;
        for (const auto& node : request.Graph.Nodes)
        {
            if (node.Kind != Kind::Output)
            {
                continue;
            }
            const auto* source = MaterialGraphEval::ResolveSource(request.Graph, node, static_cast<size_t>(MaterialOutputPin::Normal));
            legacyColorNormal = source && source->SplitPins.empty() && source->Kind == Kind::TextureSample &&
                                static_cast<int>(source->Value[0]) == static_cast<int>(MaterialSamplerType::Color);
            break;
        }
        MaterialGraphGpu::BakeDesc desc;
        desc.Program = program.Texels;
        desc.JobMode = MaterialGraphGpu::Mode::Bake;
        desc.Resolution = request.Resolution;
        desc.Time = request.Time;
        desc.Flags = static_cast<uint32_t>(request.Alpha) | (legacyColorNormal ? 4u : 0u);
        desc.SlotMask = request.Mask;
        return MaterialGraphGpu::Submit(std::move(desc));
    }

    // 変更が落ち着いたら（または手動で）ピクセルごとの式をテクスチャへ焼く
    void ScheduleBake(AssetHandle material, GraphDocument& document, uint64_t contentSignature)
    {
        if (document.PendingBakeMask == 0)
        {
            document.BakeRequested = false;
            return;
        }
        const uint64_t signature = BakeSignature(document, contentSignature);
        const bool upToDate = document.Baked.Signature == signature &&
                              (document.Baked.Mask & document.PendingBakeMask) == document.PendingBakeMask;
        if (document.BakeJob.valid() || (upToDate && !document.BakeRequested))
        {
            return;
        }
        if (signature == document.FailedBakeSignature && !document.BakeRequested)
        {
            return; // 同じ内容で失敗したものは自動では繰り返さない
        }
        const auto& settings = document.Graph.Settings;
        const bool idle = ImGui::GetTime() - document.LastContentChange >= 0.5 && !ImGui::IsAnyItemActive();
        if (!document.BakeRequested && !(settings.AutoBake && idle))
        {
            if (!settings.AutoBake)
            {
                document.BakeStatus = "未ベイクの変更があります（「今すぐベイク」で反映）";
            }
            return;
        }
        const auto materialPath = MaterialManager::Registry().Resolve(material);
        if (materialPath.empty())
        {
            document.BakeStatus = "ベイク失敗: マテリアルのパスが見つかりません";
            document.FailedBakeSignature = signature;
            document.BakeRequested = false;
            return;
        }
        MaterialGraphBake::Request request;
        request.Graph = document.Graph;
        request.Mask = document.PendingBakeMask;
        request.Alpha = document.PendingAlpha;
        request.Resolution = std::clamp(settings.BakeResolution, 64, 2048);
        request.Time = settings.BakeTime;
        request.Signature = signature;
        request.Stem = materialPath.stem().string();
        request.Directory = materialPath.parent_path() / (request.Stem + "_GraphBake");
        for (const auto& node : document.Graph.Nodes)
        {
            if (UsesNodeTexture(node.Kind) &&
                node.Texture != 0 && !request.TexturePaths.contains(node.Texture))
            {
                const auto path = MaterialManager::Registry().Resolve(node.Texture);
                if (!path.empty())
                {
                    request.TexturePaths[node.Texture] = path;
                }
            }
        }
        document.BakeJobSignature = signature;
        document.BakeRequested = false;
        document.BakeStatus = "ベイク中: " + SlotList(request.Mask) + "...";
        request.Gpu = SubmitBake(request); // GPU で評価（できないグラフは CPU）
        document.BakeJob = std::async(std::launch::async, [request = std::move(request)] {
            BackgroundTasks::Scope task("マテリアルのベイク");
            return MaterialGraphBake::Run(request);
        });
    }

    // Convert To Texture の変換結果を受け取り、ノードにテクスチャを割り当てる
    void PollNodeJob(GraphDocument& document)
    {
        if (!document.NodeJob.valid() ||
            document.NodeJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return;
        }
        const auto result = document.NodeJob.get();
        if (!result.Success)
        {
            document.NodeStatus = "変換失敗: " + result.Error;
            return;
        }
        std::string error;
        const AssetHandle handle = MaterialManager::Registry().Register(result.File, AssetType::Texture, error);
        if (handle == 0)
        {
            document.NodeStatus = "変換失敗: " + error;
            return;
        }
        TextureManager::RequestReload(result.File); // 再変換：同じパス・同じ SRV のまま中身を差し替える
        MaterialGraphTextures::Invalidate(handle);
        AssetThumbnailManager::MarkDirty(handle);
        if (auto* node = document.Graph.FindNode(result.Node))
        {
            PushHistory(document, document.Graph);
            node->Texture = handle;
            ++document.Graph.Revision;
            ++document.Graph.ParameterRevision;
            document.Dirty = true;
            document.RecoveryDirty = true;
        }
        char status[128];
        snprintf(status, sizeof(status), "変換完了（%.2f 秒）", result.Seconds);
        document.NodeStatus = status;
    }

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
            // 以前のセッションで焼いたテクスチャがあれば、それもグラフ管理（ベイクし直すまで使い続ける）
            const auto materialPath = MaterialManager::Registry().Resolve(material);
            if (!materialPath.empty())
            {
                const auto stem = materialPath.stem().string();
                const auto directory = materialPath.parent_path() / (stem + "_GraphBake");
                for (size_t slot = 0; slot < MaterialGraphBake::kSlotCount; ++slot)
                {
                    const auto file =
                        directory / (stem + "_" + MaterialGraphBake::SlotName(static_cast<MaterialTextureSlot>(slot)) + ".png");
                    std::error_code exists;
                    if (runtime.Editing.Textures[slot] == 0 || !std::filesystem::exists(file, exists))
                    {
                        continue;
                    }
                    std::string error;
                    const AssetHandle handle = MaterialManager::Registry().Register(file, AssetType::Texture, error);
                    if (handle != 0 && handle == runtime.Editing.Textures[slot])
                    {
                        document.Baked.Textures[slot] = handle;
                        document.Baked.Mask |= 1u << slot;
                        document.ManagedSlots |= 1u << slot;
                        if (slot == static_cast<size_t>(MaterialTextureSlot::Emissive))
                        {
                            document.Baked.EmissivePeak = runtime.Editing.EmissiveIntensity;
                        }
                    }
                }
            }
            document.ApplyInitialized = true;
        }
        if (PollBakeJob(material, document))
        {
            force = true; // 焼き上がったテクスチャを割り当てる
        }
        // 同じフレームの 2 回目（Draw と DrawPreviewPanel）は、強制でなく何も変わっていなければ何もしない
        const int frame = ImGui::GetFrameCount();
        if (!force && document.SignatureFrame == frame && document.AppliedSignature == document.FrameSignature)
        {
            return;
        }
        const uint64_t signature = MaterialGraphPreview::ContentSignature(document.Graph);
        document.SignatureFrame = frame;
        document.FrameSignature = signature;
        const double now = ImGui::GetTime();
        if (signature != document.LastContentSignature)
        {
            document.LastContentSignature = signature;
            document.LastContentChange = now;
        }
        // ドラッグ / カラーピッカー操作中は毎フレーム値が変わる。反映（CPU 評価・VM コンパイル・
        // マテリアル更新・サムネイル再描画）は 20Hz に間引き、手を離したら次のフレームで必ず反映する
        constexpr double kEditingApplyInterval = 1.0 / 20.0;
        const bool editing = ImGui::IsAnyItemActive();
        const bool due = !editing || document.LastApplyTime < 0.0 || now - document.LastApplyTime >= kEditingApplyInterval;
        if (force || (signature != document.AppliedSignature && due))
        {
            document.AppliedSignature = signature;
            document.LastApplyTime = now;
            auto baked = MaterialGraphPreview::BakeToMaterial(document.Graph, runtime.Editing, document.ManagedSlots,
                                                             &document.Baked);
            document.ManagedSlots = baked.ManagedSlots;
            document.ApplyNotes = std::move(baked.Notes);
            document.PendingBakeMask = baked.TextureBakeMask;
            document.PendingAlpha = baked.Alpha;
            if (baked.HasOutput && !(baked.Data == runtime.Editing))
            {
                MaterialManager::SetEditing(material, baked.Data);
            }
            // 描画時評価（GPU）：グラフをプログラムにしてエンジンのシェーダーへ渡す（値の変更でも再コンパイル不要）
            MaterialManager::SetGraphProgram(material, &document.Graph);
        }
        ScheduleBake(material, document, signature);
        PollNodeJob(document);
        // ライブ更新：Time を使う式を、ベイク済みテクスチャへ書き込み続ける（内容かベイクが変わったときだけ登録し直す）
        // 登録はグラフ全体のシリアライズを伴うので、操作中は行わず、反映が追いついてから 1 回だけ行う
        const uint64_t liveSignature = MixHash(MixHash(signature, document.Baked.Signature),
                                               static_cast<uint64_t>(document.PendingAlpha) + 1);
        if (liveSignature != document.LiveSignature && !document.BakeJob.valid() && !editing &&
            document.AppliedSignature == signature)
        {
            document.LiveSignature = liveSignature;
            std::array<std::filesystem::path, MaterialGraphBake::kSlotCount> files{};
            for (size_t slot = 0; slot < MaterialGraphBake::kSlotCount; ++slot)
            {
                if ((document.Baked.Mask & (1u << slot)) && document.Baked.Textures[slot] != 0)
                {
                    files[slot] = MaterialManager::Registry().Resolve(document.Baked.Textures[slot]);
                }
            }
            MaterialGraphLive::Register(material, document.Graph, files, document.Baked.EmissivePeak,
                                        document.PendingAlpha);
        }
    }
} // namespace

void MaterialGraphEditor::BeginFrame()
{
    focusedMaterial = 0;
    for (auto& [handle, document] : documents)
    {
        PollNodeJob(*document); // Convert To Texture の変換結果（エディターを描いていないフレームでも受け取る）
        // エディターを描いていないフレームでもベイク結果を受け取って反映する
        if (document->BakeJob.valid() &&
            document->BakeJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            ApplyGraphToMaterial(handle, *document, true);
        }
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
                BackgroundTasks::Scope task("シェーダーのコンパイル");
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
        textures += (node.Kind == Kind::TextureSample || IsConvertTextureNode(node.Kind)) ? 1 : 0;
    }
    ImGui::TextDisabled("ノード %zu / 接続 %zu / テクスチャ %zu", document.Graph.Nodes.size(),
                        document.Graph.Links.size(), textures);
    if (!document.LoadFailed && ImGui::CollapsingHeader("マテリアル設定（Details）", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawGraphSettings(document, material);
    }
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
    // 毎フレーム呼ばれるので、ここでライブ更新（Time で動くマテリアル）も進める
    MaterialGraphLive::Update(ImGui::GetTime());
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
    MaterialGraphLive::Shutdown();
    MaterialGraphGpu::Shutdown(); // GPU 待ちのワーカーを解放してから、ドキュメント（ジョブの future）を破棄する
    documents.clear();
    MaterialGraphTextures::Clear();
    focusedMaterial = 0;
    nextRecoverySave = 0;
}







