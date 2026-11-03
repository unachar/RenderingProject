#include "../source/editor/materialgraphpreview.cpp"
#include "materialgraphcompiler.h"
#include "materialgraphbake.h"
#include "proceduralio.h"
#include <iostream>
#include <cstdlib>

// Inject deterministic samples to isolate evaluation from asynchronous texture file IO.
namespace MaterialGraphTextures
{
    bool Sample(AssetHandle, float, float, bool, float out[4])
    {
        out[0] = 0.2f;
        out[1] = 0.4f;
        out[2] = 0.8f;
        out[3] = 0.5f;
        return true;
    }

    bool IsReady(AssetHandle)
    {
        return true;
    }

    uint64_t Generation()
    {
        return 1;
    }

    std::shared_ptr<const CpuTexture> LoadForBake(const std::filesystem::path&, int)
    {
        std::abort();
    }

    void SampleTexture(const CpuTexture&, float, float, bool, float[4])
    {
        std::abort();
    }
}

namespace ProceduralIO
{
    bool SaveImageFile(const std::filesystem::path&, const ProcStudio::Image&, Encoding, std::string&)
    {
        std::abort();
    }

    std::string ToUtf8(const std::filesystem::path&)
    {
        std::abort();
    }
}

namespace
{
    int failures = 0;

    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }

    void Connect(MaterialGraph& graph, uint64_t source, uint64_t target, size_t input)
    {
        std::string error;
        Check(graph.Connect(graph.FindNode(source)->OutputPin, graph.FindNode(target)->InputPins[input], error),
              error.c_str());
    }

    void WriteShader(const MaterialGraphProgram& program, const std::string& name)
    {
        std::ofstream file("build/material-graph-tests/" + name + ".hlsl");
        file
            << program.Hlsl
            << "\nfloat4 main(float4 position : SV_Position) : SV_Target { MaterialGraphContext c = (MaterialGraphContext)0; return EvaluateMaterialNode(c); }\n";
    }

    void WriteRuntimeShader(const MaterialGraphProgram& program, const std::string& name)
    {
        std::ofstream file("build/material-graph-tests/runtime-" + name + ".hlsl");
        file << "Texture2D<float4> RuntimeHeap[512] : register(t0);\n"
                "SamplerState RuntimeSampler : register(s0);\n"
                "Texture2D<float4> LoadMaterialTexture(uint index) { return RuntimeHeap[index]; }\n"
                "namespace RuntimeGraph {\n"
             << program.Hlsl << "\n}\n"
                "float4 main(float4 position : SV_Position) : SV_Target {\n"
                "RuntimeGraph::MaterialGraphContext c = (RuntimeGraph::MaterialGraphContext)0;\n"
                "c.UV = position.xy / 512.0f; c.WorldNormal = float3(0,0,1); c.CameraVector = float3(0,0,1);\n"
                "c.Tangent = float3(1,0,0); c.Bitangent = float3(0,-1,0);\n";
        if (program.MaterialOutput)
        {
            file << "return RuntimeGraph::EvaluateMaterialGraph(c,RuntimeSampler).BaseColor; }\n";
        }
        else
        {
            file << "return RuntimeGraph::EvaluateMaterialNode(c,RuntimeSampler); }\n";
        }
    }
}

int main()
{
    {
        MaterialGraph graph;
        const auto texture = graph.AddNode(MaterialNodeKind::TextureSample, 0, 0);
        graph.FindNode(texture)->Texture = 1;
        const auto output = graph.AddNode(MaterialNodeKind::Output, 0, 0);
        Connect(graph, texture, output, 0);
        const auto runtime = MaterialGraphCompiler::GenerateRuntime(graph);
        Check(runtime.Success, "production graph generation succeeds");
        Check(runtime.Hlsl.find("register(") == std::string::npos,
              "production graph does not claim preview shader registers");
        Check(runtime.Hlsl.find("ParameterTexture") != std::string::npos,
              "production graph reads its material's runtime parameters");
        WriteRuntimeShader(runtime, "output");
        const auto normal = graph.AddNode(MaterialNodeKind::TextureSample, 0, 0);
        graph.FindNode(normal)->Texture = 1;
        graph.FindNode(normal)->Value[0] = static_cast<float>(MaterialSamplerType::Normal);
        Connect(graph, normal, output, static_cast<size_t>(MaterialOutputPin::Normal));
        const auto mixed = MaterialGraphCompiler::GenerateRuntime(graph);
        Check(mixed.Textures.size() == 2 && mixed.TextureSamplers.size() == 2,
              "color and normal samples of one asset retain distinct resource views");
        WriteRuntimeShader(mixed, "mixed-samplers");
    }
    const auto color = MaterialGraphEval::Promote({{0.2f, 0.4f, 0.6f, 0.0f}, 3}, 4);
    Check(color.V[3] == 1.0f, "RGB promotion matches HLSL opaque alpha");
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
    ImGui::NewFrame();
    ImGui::Begin("test");
    {
        MaterialGraph graph;
        const auto output = graph.AddNode(MaterialNodeKind::Output, 0, 0);
        const auto time = graph.AddNode(MaterialNodeKind::Time, 0, 0);
        const auto chosen = graph.AddNode(MaterialNodeKind::Constant3Vector, 0, 0);
        const auto select = graph.AddNode(MaterialNodeKind::StaticSwitch, 0, 0);
        graph.FindNode(select)->Value[0] = 1;
        Connect(graph, chosen, select, 0);
        Connect(graph, time, select, 1);
        Connect(graph, select, output, 0);
        MaterialGraphEval::Topology topology;
        MaterialGraphEval::BuildTopology(graph, topology);
        Check(!topology.TimeDependent[topology.Index.at(select)], "static switch ignores inactive time branch");
        graph.FindNode(select)->Value[0] = 0;
        Check(graph.ResolveOutputType(select) == MaterialGraphType::Scalar,
              "static switch type follows selected branch");
    }
    {
        MaterialGraph graph;
        const auto constant = graph.AddNode(MaterialNodeKind::Constant, 0, 0);
        graph.FindNode(constant)->Value[0] = 0.75f;
        MaterialGraphPreview::DrawNodePreview(graph, *graph.FindNode(constant), 64);
        auto& image = MaterialGraphPreview::g_Caches.at(&graph).Images.at(constant);
        Check(image.Pixels[0] == IM_COL32(191, 191, 191, 255), "constant preview has expected pixels");
        Check(image.Texture && static_cast<unsigned char*>(image.Texture->GetPixels())[0] == 191,
              "preview pixels reach texture upload");
        Check(ImGui::GetCurrentContext()->UserTextures.contains(image.Texture), "preview texture is registered");
        bool referencesTexture = false;
        for (const auto& command : ImGui::GetWindowDrawList()->CmdBuffer)
        {
            referencesTexture |= command.TexRef._TexData == image.Texture;
        }
        Check(referencesTexture, "draw command resolves preview GPU descriptor through texture data");
        // Simulate the backend completing destruction. It must not request resurrection.
        auto* retired = MaterialGraphPreview::CreateTexture(16);
        retired->SetTexID(123);
        retired->SetStatus(ImTextureStatus_OK);
        MaterialGraphPreview::ReleaseTexture(retired);
        retired->SetTexID(ImTextureID_Invalid);
        retired->SetStatus(ImTextureStatus_Destroyed);
        Check(retired->Status == ImTextureStatus_Destroyed, "released previews stay destroyed");
        if (retired->Status != ImTextureStatus_Destroyed)
        {
            retired->WantDestroyNextFrame = true;
            retired->SetStatus(ImTextureStatus_Destroyed);
        }
        MaterialGraphPreview::CollectGraveyard();
        MaterialGraphPreview::Forget(graph);
    }
    ImGui::End();
    ImGui::EndFrame();

    for (const auto& definition : MaterialGraph::Definitions())
    {
        if (definition.Kind == MaterialNodeKind::Group || definition.Kind == MaterialNodeKind::Output)
        {
            continue;
        }
        MaterialGraph graph;
        const auto id = graph.AddNode(definition.Kind, 0, 0);
        graph.AddNode(MaterialNodeKind::Output, 0, 0);
        if (definition.Kind == MaterialNodeKind::TextureSample)
        {
            graph.FindNode(id)->Texture = 1;
        }
        MaterialGraphEval::Topology topology;
        MaterialGraphEval::BuildTopology(graph, topology);
        MaterialGraphEval::Evaluator evaluator(graph, topology, {});
        evaluator.Reset({});
        const auto value = evaluator.Node(0);
        Check(value.N >= 1 && value.N <= 4, definition.Name);
        for (float component : value.V)
        {
            Check(std::isfinite(component), definition.Name);
        }
        const auto program = MaterialGraphCompiler::Generate(graph, id);
        Check(program.Success, (std::string(definition.Name) + ": " + program.Error).c_str());
        if (program.Success)
        {
            WriteShader(program, std::to_string(static_cast<int>(definition.Kind)));
        }
        const auto runtime = MaterialGraphCompiler::GenerateRuntime(graph, id);
        Check(runtime.Success, definition.Name);
        if (runtime.Success)
        {
            WriteRuntimeShader(runtime, std::to_string(static_cast<int>(definition.Kind)));
        }
        const MaterialNodeKind constants[] = {MaterialNodeKind::Constant, MaterialNodeKind::Constant2Vector,
                                              MaterialNodeKind::Constant3Vector, MaterialNodeKind::Constant4Vector};
        for (size_t input = 0; input < definition.Inputs.size(); ++input)
        {
            for (int width = 0; width < 4; ++width)
            {
                auto connected = graph;
                const auto constant = connected.AddNode(constants[width], 0, 0);
                connected.FindNode(constant)->Value = {0.2f, 0.4f, 0.6f, 0.8f};
                std::string error;
                if (!connected.Connect(connected.FindNode(constant)->OutputPin,
                                       connected.FindNode(id)->InputPins[input], error))
                {
                    continue;
                }
                const auto variant = MaterialGraphCompiler::Generate(connected, id);
                const auto name = std::to_string(static_cast<int>(definition.Kind)) + "-" + std::to_string(input) +
                                  "-" + std::to_string(width + 1);
                Check(variant.Success, (name + ": " + variant.Error).c_str());
                if (variant.Success)
                {
                    WriteShader(variant, name);
                }
                const auto runtimeVariant = MaterialGraphCompiler::GenerateRuntime(connected, id);
                Check(runtimeVariant.Success, name.c_str());
                if (runtimeVariant.Success)
                {
                    WriteRuntimeShader(runtimeVariant, name);
                }
            }
        }
    }
    {
        MaterialGraph graph;
        const auto color = graph.AddNode(MaterialNodeKind::Constant3Vector, 0, 0);
        graph.FindNode(color)->Value = {0.2f, 0.4f, 0.6f, 0};
        const auto output = graph.AddNode(MaterialNodeKind::Output, 0, 0);
        Connect(graph, color, output, 0);
        MaterialAssetData material;
        material.BaseColor[3] = 0.1f;
        const auto baked = MaterialGraphPreview::BakeToMaterial(graph, material, 0);
        Check(baked.Data.BaseColor[3] == 1.0f, "RGB output resets stale material alpha to one");
        const auto mask = graph.AddNode(MaterialNodeKind::Constant, 0, 0);
        graph.FindNode(mask)->Value[0] = 0;
        Connect(graph, mask, output, static_cast<size_t>(MaterialOutputPin::OpacityMask));
        const auto masked = MaterialGraphPreview::BakeToMaterial(graph, material, 0);
        Check(masked.Alpha == MaterialGraphPreview::AlphaSource::OpacityMask && masked.TextureBakeMask != 0,
              "constant opacity masks affect engine output");
        MaterialGraphBake::Request request;
        request.Graph = graph;
        request.Resolution = 16;
        request.Threads = 1;
        request.Mask = masked.TextureBakeMask;
        request.Alpha = masked.Alpha;
        MaterialGraphBake::Images images;
        std::string error;
        Check(MaterialGraphBake::Evaluate(request, {}, images, error), "mask baking succeeds");
        Check(images.Pixels[0][0][3] == 0.0f, "constant mask reaches baked texture alpha");
        request.Alpha = MaterialGraphPreview::AlphaSource::BaseColor;
        graph.RemoveLink(graph.InputLink(graph.FindNode(output)->InputPins[6])->Id);
        const auto uv = graph.AddNode(MaterialNodeKind::UV, 0, 0);
        const auto gradient = graph.AddNode(MaterialNodeKind::LinearGradient, 0, 0);
        Connect(graph, uv, gradient, 0);
        Connect(graph, gradient, output, 1);
        request.Graph = graph;
        request.Mask = 1u << static_cast<unsigned>(MaterialTextureSlot::MetallicRoughness);
        Check(MaterialGraphBake::Evaluate(request, {}, images, error), "metallic/roughness baking succeeds");
        const auto slot = static_cast<size_t>(MaterialTextureSlot::MetallicRoughness);
        Check(fabsf(images.Pixels[slot][0][0] - 0.5f / 16.0f) < 1e-6f, "MR red stores metallic UV gradient");
        Check(fabsf(images.Pixels[slot][0][1] - 0.5f) < 1e-6f, "MR green stores roughness");
        const auto singleThread = images.Pixels;
        request.Threads = 2;
        Check(MaterialGraphBake::Evaluate(request, {}, images, error) && images.Pixels == singleThread,
              "baking matches across worker counts");
    }
    ImGui::DestroyContext();
    std::cout << "Material graph failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
