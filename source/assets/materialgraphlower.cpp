#include "pch.h"
#include "materialgraphlower.h"
#include "materialgraphcustomhlsl.h"
#include "materialgraphmath.h"
#include "materialgraphnodes.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace MaterialFunctionLibrary
{
    namespace
    {
        using Kind = MaterialNodeKind;
        std::mutex g_Mutex;
        Loader g_Loader;
        std::unordered_map<AssetHandle, std::shared_ptr<const MaterialGraph>> g_Cache;
        uint64_t g_Revision = 1;

        int DimsOf(MaterialGraphType type)
        {
            return type == MaterialGraphType::Numeric ? 1 : static_cast<int>(type);
        }
    } // namespace

    void SetLoader(Loader loader)
    {
        std::lock_guard lock(g_Mutex);
        g_Loader = std::move(loader);
        ++g_Revision;
    }
    void Register(AssetHandle handle, std::shared_ptr<const MaterialGraph> graph)
    {
        std::lock_guard lock(g_Mutex);
        g_Cache[handle] = std::move(graph);
        ++g_Revision;
    }
    void Unregister(AssetHandle handle)
    {
        std::lock_guard lock(g_Mutex);
        g_Cache.erase(handle);
        ++g_Revision;
    }
    void Clear()
    {
        std::lock_guard lock(g_Mutex);
        g_Cache.clear();
        g_Loader = nullptr;
        ++g_Revision;
    }
    void Invalidate(AssetHandle handle)
    {
        std::lock_guard lock(g_Mutex);
        g_Cache.erase(handle);
        ++g_Revision;
    }
    uint64_t Revision()
    {
        std::lock_guard lock(g_Mutex);
        return g_Revision;
    }
    std::shared_ptr<const MaterialGraph> Find(AssetHandle handle)
    {
        Loader loader;
        {
            std::lock_guard lock(g_Mutex);
            const auto found = g_Cache.find(handle);
            if (found != g_Cache.end())
            {
                return found->second;
            }
            loader = g_Loader;
        }
        if (!loader || handle == 0)
        {
            return nullptr;
        }
        auto loaded = loader(handle); // ロード中は排他しない（ロード側が Find を呼んでもよい）
        if (loaded)
        {
            std::lock_guard lock(g_Mutex);
            g_Cache[handle] = loaded;
        }
        return loaded;
    }

    bool GetSignature(const MaterialGraph& function, Signature& signature, std::string& error)
    {
        signature = {};
        error.clear();
        if (!function.Settings.IsFunction)
        {
            error = "Material Function ではありません。";
            return false;
        }
        std::unordered_set<int> seenIn;
        std::unordered_set<int> seenOut;
        for (const auto& node : function.Nodes)
        {
            if (node.Kind != Kind::FunctionInput && node.Kind != Kind::FunctionOutput)
            {
                continue;
            }
            const bool input = node.Kind == Kind::FunctionInput;
            const int index = static_cast<int>(node.Value[1]);
            const int limit = input ? MaterialGraphNodes::kMaxFunctionInputs : MaterialGraphNodes::kMaxFunctionOutputs;
            if (index < 0 || index >= limit)
            {
                error = std::string(input ? "Function Input" : "Function Output") + " の番号は 0〜" + std::to_string(limit - 1) + " です（" + node.Text + "）。";
                return false;
            }
            if (!(input ? seenIn : seenOut).insert(index).second)
            {
                error = std::string(input ? "Function Input" : "Function Output") + " の番号 " + std::to_string(index) + " が重複しています。";
                return false;
            }
            Port port;
            port.Name = node.Text.empty() ? (input ? "Input" : "Output") + std::to_string(index) : node.Text;
            port.Dims = std::clamp(static_cast<int>(node.Value[0]), 1, 4);
            port.Node = node.Id;
            for (size_t i = 0; i < 4 && i < node.Params.size(); ++i)
            {
                port.Default[i] = node.Params[i];
            }
            auto& ports = input ? signature.Inputs : signature.Outputs;
            if (ports.size() <= static_cast<size_t>(index))
            {
                ports.resize(static_cast<size_t>(index) + 1);
            }
            ports[static_cast<size_t>(index)] = std::move(port);
        }
        // 番号に抜けがあると呼び出しノードのピンが対応しない
        for (const auto* ports : {&signature.Inputs, &signature.Outputs})
        {
            for (size_t i = 0; i < ports->size(); ++i)
            {
                if ((*ports)[i].Node == 0)
                {
                    error = std::string(ports == &signature.Inputs ? "Function Input" : "Function Output") + " の番号 " + std::to_string(i) + " がありません（番号は 0 から連続させてください）。";
                    return false;
                }
            }
        }
        if (signature.Outputs.empty())
        {
            error = "Function Output がありません。";
            return false;
        }
        // 型検証：Function Output の宣言と、実際に繋がっている値の型
        for (const auto& node : function.Nodes)
        {
            if (node.Kind != Kind::FunctionOutput)
            {
                continue;
            }
            const auto* link = node.InputPins.empty() ? nullptr : function.RawInputLink(node.InputPins[0]);
            if (!link)
            {
                error = "Function Output「" + node.Text + "」が接続されていません。";
                return false;
            }
            const int actual = DimsOf(function.PinOutputType(link->SourcePin));
            const int declared = std::clamp(static_cast<int>(node.Value[0]), 1, 4);
            if (actual != declared && actual != 1)
            {
                error = "Function Output「" + node.Text + "」は float" + std::to_string(declared) + " と宣言されていますが、float" +
                        std::to_string(actual) + " が繋がっています。";
                return false;
            }
        }
        return true;
    }

    bool RefreshCall(MaterialGraphNode& call, std::string& error)
    {
        error.clear();
        if (call.Kind != Kind::MaterialFunctionCall)
        {
            return false;
        }
        const auto function = Find(call.Function);
        if (!function)
        {
            error = "Material Function が見つかりません。";
            return false;
        }
        Signature signature;
        if (!GetSignature(*function, signature, error))
        {
            return false;
        }
        if (signature.Inputs.size() > MaterialGraphNodes::kMaxFunctionInputs || signature.Outputs.size() > MaterialGraphNodes::kMaxFunctionOutputs)
        {
            error = "関数の入出力が多すぎます（入力 8 / 出力 4 まで）。";
            return false;
        }
        call.Params.assign(14, 1.0f);
        call.Params[0] = static_cast<float>(signature.Outputs.size());
        call.Params[5] = static_cast<float>(signature.Inputs.size());
        call.PinNames.assign(12, std::string());
        for (size_t i = 0; i < signature.Outputs.size(); ++i)
        {
            call.Params[1 + i] = static_cast<float>(signature.Outputs[i].Dims);
            call.PinNames[8 + i] = signature.Outputs[i].Name;
        }
        for (size_t i = 0; i < signature.Inputs.size(); ++i)
        {
            call.Params[6 + i] = static_cast<float>(signature.Inputs[i].Dims);
            call.PinNames[i] = signature.Inputs[i].Name;
        }
        return true;
    }

    bool ValidateCall(const MaterialGraph& graph, const MaterialGraphNode& call, std::string& error)
    {
        error.clear();
        const auto function = Find(call.Function);
        if (!function)
        {
            error = "Material Function が見つかりません。";
            return false;
        }
        Signature signature;
        if (!GetSignature(*function, signature, error))
        {
            return false;
        }
        for (size_t i = 0; i < signature.Inputs.size() && i < call.InputPins.size(); ++i)
        {
            const auto* link = graph.RawInputLink(call.InputPins[i]);
            if (!link)
            {
                continue;
            }
            const int source = DimsOf(graph.PinOutputType(link->SourcePin));
            const int target = signature.Inputs[i].Dims;
            const bool ok = source == 1 || source >= target;
            if (!ok)
            {
                error = "入力「" + signature.Inputs[i].Name + "」は float" + std::to_string(target) + " ですが、float" + std::to_string(source) + " が繋がっています。";
                return false;
            }
        }
        return true;
    }

    namespace
    {
        bool Walk(const MaterialGraph& graph, std::vector<AssetHandle>& stack, std::string& error)
        {
            for (const auto& node : graph.Nodes)
            {
                if (node.Kind != Kind::MaterialFunctionCall)
                {
                    continue;
                }
                if (node.Function == 0)
                {
                    error = "Material Function Call に関数が設定されていません。";
                    return false;
                }
                if (std::find(stack.begin(), stack.end(), node.Function) != stack.end())
                {
                    error = "Material Function が循環参照しています（再帰呼び出しはできません）。";
                    return false;
                }
                const auto function = Find(node.Function);
                if (!function)
                {
                    error = "Material Function が見つかりません。";
                    return false;
                }
                if (stack.size() >= 16)
                {
                    error = "Material Function の入れ子が深すぎます（16 段まで）。";
                    return false;
                }
                stack.push_back(node.Function);
                const bool ok = Walk(*function, stack, error);
                stack.pop_back();
                if (!ok)
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    bool ValidateReferences(const MaterialGraph& graph, AssetHandle self, std::string& error)
    {
        error.clear();
        std::vector<AssetHandle> stack;
        if (self != 0)
        {
            stack.push_back(self);
        }
        return Walk(graph, stack, error);
    }
} // namespace MaterialFunctionLibrary

namespace MaterialGraphLower
{
    namespace
    {
        using Kind = MaterialNodeKind;

        AssetHandle ResolveParameter(const MaterialGraph& graph, const MaterialGraphNode& node)
        {
            for (const auto& [name, handle] : graph.InstanceTextures)
            {
                if (name == node.Text && handle != 0)
                {
                    return handle;
                }
            }
            return node.Texture;
        }

        // 1 つの呼び出しを展開する
        bool InlineCall(MaterialGraph& graph, uint64_t callId, std::string& error)
        {
            const MaterialGraphNode callCopy = *graph.FindNode(callId);
            const auto function = MaterialFunctionLibrary::Find(callCopy.Function);
            if (!function)
            {
                error = "Material Function が見つかりません。";
                return false;
            }
            MaterialFunctionLibrary::Signature signature;
            if (!MaterialFunctionLibrary::GetSignature(*function, signature, error) ||
                !MaterialFunctionLibrary::ValidateCall(graph, callCopy, error))
            {
                return false;
            }
            // ---- 関数のノードを複製（Function Input / Output は Reroute にする）
            std::unordered_map<uint64_t, uint64_t> pinMap; // 関数のピン → 新しいピン
            std::unordered_map<uint64_t, uint64_t> nodeMap;
            std::vector<uint64_t> inputReroutes(signature.Inputs.size(), 0);
            std::vector<uint64_t> outputReroutes(signature.Outputs.size(), 0);
            for (const auto& source : function->Nodes)
            {
                const bool isInput = source.Kind == Kind::FunctionInput;
                const bool isOutput = source.Kind == Kind::FunctionOutput;
                if (source.Kind == Kind::Group)
                {
                    continue;
                }
                const uint64_t id = graph.AddNode(isInput || isOutput ? Kind::Reroute : source.Kind, source.Position[0], source.Position[1]);
                if (id == 0)
                {
                    error = "展開するノードが多すぎます。";
                    return false;
                }
                auto* node = graph.FindNode(id);
                nodeMap[source.Id] = id;
                if (isInput || isOutput)
                {
                    const size_t index = static_cast<size_t>(source.Value[1]);
                    (isInput ? inputReroutes : outputReroutes)[index] = id;
                    pinMap[isInput ? source.OutputPin : source.InputPins[0]] = isInput ? node->OutputPin : node->InputPins[0];
                    if (isOutput)
                    {
                        pinMap[source.InputPins[0]] = node->InputPins[0];
                    }
                    continue;
                }
                node->Name = source.Name;
                node->Value = source.Value;
                node->Texture = source.Texture;
                node->Params = source.Params;
                node->Text = source.Text;
                node->PinNames = source.PinNames;
                node->Function = source.Function;
                node->PinValues = source.PinValues;
                node->PinVectors = source.PinVectors;
                for (size_t i = 0; i < source.InputPins.size() && i < node->InputPins.size(); ++i)
                {
                    pinMap[source.InputPins[i]] = node->InputPins[i];
                }
                pinMap[source.OutputPin] = node->OutputPin;
                if (!source.SplitPins.empty() && node->SplitPins.empty())
                {
                    std::string splitError;
                    graph.SplitOutput(id, splitError, static_cast<int>(source.SplitPins.size()));
                    node = graph.FindNode(id);
                }
                for (size_t i = 0; i < source.SplitPins.size() && i < node->SplitPins.size(); ++i)
                {
                    pinMap[source.SplitPins[i]] = node->SplitPins[i];
                }
            }
            // ---- 関数の中の接続
            for (const auto& link : function->Links)
            {
                const auto s = pinMap.find(link.SourcePin);
                const auto t = pinMap.find(link.TargetPin);
                if (s == pinMap.end() || t == pinMap.end())
                {
                    continue;
                }
                if (!graph.ConnectUnchecked(s->second, t->second))
                {
                    error = "関数の展開に失敗しました（接続の上限）。";
                    return false;
                }
            }
            // ---- 呼び出しの入力 → Function Input の Reroute（未接続なら関数の既定値）
            for (size_t i = 0; i < signature.Inputs.size(); ++i)
            {
                auto* reroute = graph.FindNode(inputReroutes[i]);
                if (!reroute)
                {
                    continue;
                }
                const auto* link = i < callCopy.InputPins.size() ? graph.RawInputLink(callCopy.InputPins[i]) : nullptr;
                if (link)
                {
                    if (!graph.ConnectUnchecked(link->SourcePin, reroute->InputPins[0]))
                    {
                        error = "関数の入力を接続できません。";
                        return false;
                    }
                    reroute = graph.FindNode(inputReroutes[i]);
                    continue;
                }
                static const Kind constants[] = {Kind::Constant, Kind::Constant2Vector, Kind::Constant3Vector, Kind::Constant4Vector};
                const int dims = signature.Inputs[i].Dims;
                const uint64_t constant = graph.AddNode(constants[dims - 1], 0, 0);
                auto* constantNode = graph.FindNode(constant);
                for (int c = 0; c < 4; ++c)
                {
                    constantNode->Value[static_cast<size_t>(c)] = signature.Inputs[i].Default[static_cast<size_t>(c)];
                }
                if (!graph.ConnectUnchecked(constantNode->OutputPin, graph.FindNode(inputReroutes[i])->InputPins[0]))
                {
                    error = "関数の既定値を接続できません。";
                    return false;
                }
            }
            // ---- 呼び出しの出力 → Function Output の Reroute
            std::vector<uint64_t> outputPins(signature.Outputs.size(), 0);
            for (size_t i = 0; i < signature.Outputs.size(); ++i)
            {
                outputPins[i] = graph.FindNode(outputReroutes[i])->OutputPin;
            }
            {
                const auto* call = graph.FindNode(callId);
                std::unordered_map<uint64_t, uint64_t> remap; // 呼び出しの出力ピン → 展開後の出力ピン
                for (size_t i = 0; i < call->SplitPins.size() && i < outputPins.size(); ++i)
                {
                    remap[call->SplitPins[i]] = outputPins[i];
                }
                remap[call->OutputPin] = outputPins.empty() ? 0 : outputPins[0];
                for (auto& link : graph.Links)
                {
                    const auto found = remap.find(link.SourcePin);
                    if (found != remap.end() && found->second != 0)
                    {
                        link.SourcePin = found->second;
                    }
                }
            }
            // ---- 呼び出しノードを Reroute にする（ID を保ち、ノードのプレビューが最初の出力を見られるようにする）
            {
                auto* call = graph.FindNode(callId);
                const uint64_t keepInput = call->InputPins.empty() ? 0 : call->InputPins[0];
                const std::unordered_set<uint64_t> dropped(call->InputPins.begin() + 1, call->InputPins.end());
                std::erase_if(graph.Links, [&](const MaterialGraphLink& link) {
                    return link.TargetPin == keepInput || dropped.contains(link.TargetPin);
                });
                call->Kind = Kind::Reroute;
                call->InputPins = {keepInput};
                call->SplitPins.clear();
                call->Function = 0;
                call->Params.clear();
                call->PinNames.clear();
                call->PinValues.clear();
                if (!outputPins.empty() && keepInput != 0)
                {
                    if (!graph.ConnectUnchecked(outputPins[0], keepInput))
                    {
                        error = "関数の出力を接続できません。";
                        return false;
                    }
                }
            }
            return true;
        }
    } // namespace

    bool NeedsLowering(const MaterialGraph& graph)
    {
        for (const auto& node : graph.Nodes)
        {
            if (node.Kind == Kind::MaterialFunctionCall)
            {
                return true;
            }
            if (node.Kind == Kind::TextureParameter2D)
            {
                for (const auto& [name, handle] : graph.InstanceTextures)
                {
                    if (name == node.Text && handle != 0)
                    {
                        return true;
                    }
                }
                if (node.Texture != 0)
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool Lower(const MaterialGraph& source, MaterialGraph& lowered, std::string& error)
    {
        error.clear();
        lowered = source;
        if (!MaterialFunctionLibrary::ValidateReferences(source, 0, error))
        {
            return false;
        }
        // Texture Parameter 2D：解決できるものは Texture Sample（同じピン構成）にする。無いものは各バックエンドがフォールバック色を使う
        for (auto& node : lowered.Nodes)
        {
            if (node.Kind != Kind::TextureParameter2D)
            {
                continue;
            }
            const AssetHandle resolved = ResolveParameter(lowered, node);
            if (resolved != 0)
            {
                node.Kind = Kind::TextureSample;
                node.Texture = resolved;
                node.Params.clear();
            }
        }
        for (int pass = 0; pass < 16; ++pass)
        {
            uint64_t call = 0;
            for (const auto& node : lowered.Nodes)
            {
                if (node.Kind == Kind::MaterialFunctionCall)
                {
                    call = node.Id;
                    break;
                }
            }
            if (call == 0)
            {
                ++lowered.Revision;
                return lowered.Validate(error);
            }
            if (!InlineCall(lowered, call, error))
            {
                return false;
            }
        }
        error = "Material Function の入れ子が深すぎます。";
        return false;
    }

    const MaterialGraph* Prepare(const MaterialGraph& source, MaterialGraph& storage, std::string& error)
    {
        if (!NeedsLowering(source))
        {
            error.clear();
            return &source;
        }
        return Lower(source, storage, error) ? &storage : nullptr;
    }

    std::vector<Diagnostic> Diagnose(const MaterialGraph& graph, AssetHandle self)
    {
        std::vector<Diagnostic> result;
        const auto add = [&](uint64_t node, std::string message, bool warning) {
            result.push_back({node, std::move(message), warning});
        };
        const int mode = graph.Settings.Mode;
        std::unordered_map<std::string, AssetHandle> parameterDefaults;
        for (const auto& node : graph.Nodes)
        {
            std::string error;
            switch (node.Kind)
            {
                case Kind::MaterialFunctionCall:
                    if (node.Function == 0)
                    {
                        add(node.Id, "関数が設定されていません。", false);
                    }
                    else if (!MaterialFunctionLibrary::ValidateReferences(graph, self, error) || !MaterialFunctionLibrary::ValidateCall(graph, node, error))
                    {
                        add(node.Id, error, false);
                    }
                    break;
                case Kind::CustomHlsl: {
                    const int inputCount = std::clamp(static_cast<int>(node.Value[0]), 0, MaterialGraphNodes::kMaxCustomInputs);
                    std::vector<std::string> names;
                    std::vector<int> dims;
                    for (int i = 0; i < inputCount; ++i)
                    {
                        names.push_back(static_cast<size_t>(i) < node.PinNames.size() ? node.PinNames[static_cast<size_t>(i)] : std::string(1, static_cast<char>('a' + i)));
                        const auto* link = static_cast<size_t>(i) < node.InputPins.size() ? graph.InputLink(node.InputPins[static_cast<size_t>(i)]) : nullptr;
                        dims.push_back(link ? std::clamp(static_cast<int>(graph.PinOutputType(link->SourcePin)), 1, 4) : 1);
                    }
                    const auto compiled = MaterialGraphCustomHlsl::Compile(node.Text, names, dims, static_cast<int>(node.Value[1]));
                    for (const auto& diagnostic : compiled.Errors)
                    {
                        add(node.Id, "Custom HLSL " + diagnostic.Format(), false);
                    }
                    break;
                }
                case Kind::SceneColor:
                case Kind::SceneDepth:
                case Kind::DepthFade:
                    if (mode >= 0 && mode < static_cast<int>(MaterialGraphMode::Translucent))
                    {
                        add(node.Id,
                            std::string(MaterialGraph::Definition(node.Kind).Name) +
                                " は Translucent / Glass で使います（不透明パスは自分の深度 / 色を書いている最中のため、読み書きが競合します）。",
                            false);
                    }
                    break;
                case Kind::TextureParameter2D: {
                    bool resolved = node.Texture != 0;
                    for (const auto& [name, handle] : graph.InstanceTextures)
                    {
                        resolved |= name == node.Text && handle != 0;
                    }
                    if (!resolved)
                    {
                        add(node.Id, "テクスチャが未設定です。フォールバック色を使います。", true);
                    }
                    const auto found = parameterDefaults.find(node.Text);
                    if (found != parameterDefaults.end() && found->second != node.Texture)
                    {
                        add(node.Id, "パラメーター名「" + node.Text + "」が別の既定テクスチャで重複しています。", true);
                    }
                    parameterDefaults[node.Text] = node.Texture;
                    break;
                }
                case Kind::AudioSpectrum:
                case Kind::BeatPhase:
                    add(node.Id, "CPU プレビューは擬似値です。実行中は音声クロック（AudioMaterialBridge）の値を使います。", true);
                    break;
                case Kind::FunctionInput:
                case Kind::FunctionOutput:
                    if (!graph.Settings.IsFunction)
                    {
                        add(node.Id, "Function Input / Output は Material Function の中だけで使えます。", false);
                    }
                    break;
                case Kind::Output: {
                    // Toon / 陰影の追加ピンは、対応するノードを繋いだときだけ意味がある
                    struct Pair
                    {
                        MaterialOutputPin Pin;
                        Kind Expected;
                    };
                    static const Pair pairs[] = {{MaterialOutputPin::ToonShade, Kind::ToonLightRamp},
                                                 {MaterialOutputPin::ToonRim, Kind::DirectionalRimLight},
                                                 {MaterialOutputPin::ToonHair, Kind::HairAnisotropicHighlight},
                                                 {MaterialOutputPin::ToonFaceShadow, Kind::FaceSdfShadow},
                                                 {MaterialOutputPin::MekakureMaskOut, Kind::MekakureMask},
                                                 {MaterialOutputPin::ClearCoat, Kind::ClearCoatControl},
                                                 {MaterialOutputPin::SubsurfaceTint, Kind::SubsurfaceColor},
                                                 {MaterialOutputPin::AnisotropyDir, Kind::AnisotropyDirection}};
                    for (const auto& pair : pairs)
                    {
                        const size_t index = static_cast<size_t>(pair.Pin);
                        if (index >= node.InputPins.size())
                        {
                            continue;
                        }
                        const auto* link = graph.RawInputLink(node.InputPins[index]);
                        const auto* source = link ? graph.FindPinNode(link->SourcePin) : nullptr;
                        while (source && source->Kind == Kind::Reroute && !source->InputPins.empty())
                        {
                            const auto* up = graph.RawInputLink(source->InputPins[0]);
                            source = up ? graph.FindPinNode(up->SourcePin) : nullptr;
                        }
                        if (source && source->Kind != pair.Expected)
                        {
                            add(node.Id, std::string(MaterialGraph::Definition(pair.Expected).Name) +
                                             " のノードを繋いだときだけ、ライトごとの / 専用の評価になります（今は値として使われます）。",
                                true);
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
        return result;
    }

    Dependencies AnalyzeDependencies(const MaterialGraph& graph)
    {
        Dependencies d;
        // Output から辿れるノードだけを数える（未使用ノードは除去される）
        std::unordered_set<uint64_t> reachable;
        std::vector<uint64_t> pending;
        for (const auto& node : graph.Nodes)
        {
            if (node.Kind == Kind::Output)
            {
                pending.push_back(node.Id);
            }
        }
        while (!pending.empty())
        {
            const uint64_t id = pending.back();
            pending.pop_back();
            if (!reachable.insert(id).second)
            {
                continue;
            }
            const auto* node = graph.FindNode(id);
            if (!node)
            {
                continue;
            }
            for (size_t i = 0; i < node->InputPins.size(); ++i)
            {
                if (node->Kind == Kind::StaticSwitch && i != (node->Value[0] > 0.5f ? 0u : 1u))
                {
                    continue; // 選ばれなかった側は評価しない
                }
                if (const auto* link = graph.InputLink(node->InputPins[i]))
                {
                    if (const auto* source = graph.FindPinNode(link->SourcePin))
                    {
                        pending.push_back(source->Id);
                    }
                }
            }
        }
        for (const auto& node : graph.Nodes)
        {
            if (!reachable.contains(node.Id))
            {
                continue;
            }
            switch (node.Kind)
            {
                case Kind::SceneDepth:
                case Kind::DepthFade:
                    d.SceneDepth = true;
                    break;
                case Kind::SceneColor:
                    d.SceneColor = true;
                    break;
                case Kind::TwoSidedSign:
                    d.FrontFacing = true;
                    break;
                case Kind::CurvatureMask:
                    d.Curvature = true;
                    break;
                case Kind::AudioSpectrum:
                    d.Audio = true;
                    break;
                case Kind::BeatPhase:
                    d.Beat = true;
                    break;
                case Kind::Time:
                    d.Time = true;
                    break;
                case Kind::CustomHlsl:
                    d.CustomHlsl = true;
                    break;
                case Kind::DynamicSwitch:
                    ++d.DynamicSwitchCount;
                    break;
                case Kind::StaticSwitch:
                    ++d.StaticSwitchFolded;
                    break;
                case Kind::Output:
                    for (size_t pin = static_cast<size_t>(MaterialOutputPin::ToonShade); pin <= static_cast<size_t>(MaterialOutputPin::ToonFaceShadow) && pin < node.InputPins.size(); ++pin)
                    {
                        d.PerLightToon |= graph.InputLink(node.InputPins[pin]) != nullptr;
                    }
                    break;
                default:
                    break;
            }
        }
        return d;
    }
} // namespace MaterialGraphLower
