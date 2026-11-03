#include "pch.h"
#include "materialgraphshader.h"
#include "dxccompiler.h"
#include <mutex>

namespace
{
    std::mutex graphCompilerMutex;

    std::string PreviewSource(const MaterialGraphProgram& program)
    {
        std::string source = program.Hlsl;
        source += R"hlsl(
#line 1 "material_preview"
cbuffer GraphPreviewView : register(b1) { float PreviewTime; uint PreviewResolution; uint PreviewShape; uint PreviewNormal; };
struct GraphPreviewInput { float4 Position : SV_Position; float2 UV : TEXCOORD0; };
GraphPreviewInput GraphPreviewVS(uint vertex : SV_VertexID)
{
    GraphPreviewInput result;
    result.UV = float2((vertex << 1) & 2, vertex & 2);
    result.Position = float4(result.UV * float2(2,-2) + float2(-1,1),0,1);
    return result;
}
float4 GraphPreviewPS(GraphPreviewInput input) : SV_Target
{
    MaterialGraphContext context;
    context.UV = input.UV;
    context.WorldPosition = float3(input.UV * float2(2,-2) + float2(-1,1),0);
    context.WorldNormal = float3(0,0,1);
    context.CameraVector = float3(0,0,1);
    context.Time = PreviewTime;
    if (PreviewShape != 0)
    {
        float radius = dot(context.WorldPosition.xy,context.WorldPosition.xy);
        if (radius > 1) return float4(0,0,0,0);
        context.WorldPosition.z = sqrt(saturate(1-radius));
        context.WorldNormal = normalize(context.WorldPosition);
    }
    context.Tangent = normalize(cross(abs(context.WorldNormal.y) < 0.99 ? float3(0,1,0) : float3(1,0,0), context.WorldNormal));
    context.Bitangent = -cross(context.WorldNormal, context.Tangent);
)hlsl";
        if (program.MaterialOutput)
        {
            source += R"hlsl(
    GeneratedMaterial material = EvaluateMaterialGraph(context);
    float3 normal = context.WorldNormal;
    float3 tangent = normalize(cross(abs(normal.y) < 0.99 ? float3(0,1,0) : float3(1,0,0),normal));
    float3 bitangent = cross(normal,tangent);
    float2 scaledNormal = material.Normal.xy * max(material.NormalIntensity, 0.0);
    normal = normalize(tangent * scaledNormal.x + bitangent * scaledNormal.y + normal * material.Normal.z);
    float3 light = normalize(float3(-0.4,0.6,0.8));
    float3 view = float3(0,0,1);
    float3 halfVector = normalize(light + view);
    float nDotL = saturate(dot(normal,light));
    float nDotV = max(saturate(dot(normal,view)),0.001);
    float nDotH = saturate(dot(normal,halfVector));
    float vDotH = saturate(dot(view,halfVector));
    float roughness = clamp(material.Roughness,0.04,1);
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denominator = nDotH*nDotH*(alpha2-1)+1;
    float distribution = alpha2 / max(3.14159265*denominator*denominator,0.0001);
    float k = (roughness+1)*(roughness+1)/8;
    float geometry = (nDotV / (nDotV*(1-k)+k)) * (nDotL / max(nDotL*(1-k)+k,0.001));
    float metallic = saturate(material.Metallic);
    float3 f0 = lerp(0.04.xxx,material.BaseColor.rgb,metallic);
    float3 fresnel = f0 + (1-f0)*pow(1-vDotH,5);
    float3 specular = distribution * geometry * fresnel / max(4*nDotL*nDotV,0.001);
    float3 diffuse = (1-fresnel)*(1-metallic)*material.BaseColor.rgb/3.14159265;
    float3 color = (diffuse+specular)*nDotL*3 + material.BaseColor.rgb*0.08*saturate(material.AO)+material.Emissive;
    color = pow(saturate(color/(1+max(color,0))),1.0/2.2);
    float opacity = saturate(material.BaseColor.a * material.Opacity);
    opacity *= step(0.5,material.OpacityMask);
    float checker = fmod(floor(input.UV.x*12)+floor(input.UV.y*12),2);
    return float4(lerp(lerp(0.12.xxx,0.25.xxx,checker),color,opacity),1);
)hlsl";
        }
        else
        {
            source += R"hlsl(
    float4 color = EvaluateMaterialNode(context);
    if (PreviewNormal != 0) color.rgb = color.rgb * 0.5 + 0.5;
    float checker = fmod(floor(input.UV.x*12)+floor(input.UV.y*12),2);
    color.rgb = lerp(lerp(0.12.xxx,0.25.xxx,checker),color.rgb,saturate(color.a));
    return float4(color.rgb,1);
)hlsl";
        }
        source += "}\n";
        return source;
    }
}

MaterialGraphShader MaterialGraphShaderCompiler::Compile(const MaterialGraphProgram& program)
{
    // Keep the existing atomic cache writers single-owner for identical graph variants.
    std::lock_guard lock(graphCompilerMutex);
    MaterialGraphShader shader;
    if (!program.Success)
    {
        shader.Error = program.Error;
        return shader;
    }
    const auto source = PreviewSource(program);
    shader.Hash = 14695981039346656037ull;
    for (const unsigned char value : source)
    {
        shader.Hash = (shader.Hash ^ value) * 1099511628211ull;
    }
    const auto directory = std::filesystem::path("EditorCache/MaterialGraphs/Shaders");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        shader.Error = error.message();
        return shader;
    }
    const auto path = directory / (std::to_string(shader.Hash) + ".hlsl");
    if (!WriteAssetFile(path, source, shader.Error))
    {
        return shader;
    }
    const auto compile = [&](const char* entry, const char* profile, ComPtr<ID3DBlob>& output) {
        ComPtr<ID3DBlob> diagnostics;
        const auto result = DxcCompileFromFileCompat(
            path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &output, &diagnostics);
        if (FAILED(result))
        {
            if (diagnostics)
            {
                shader.Error.assign(static_cast<const char*>(diagnostics->GetBufferPointer()),
                                    diagnostics->GetBufferSize());
            }
            else
            {
                shader.Error = "DXC failed (HRESULT " + std::to_string(static_cast<unsigned>(result)) + ").";
            }
            return false;
        }
        return true;
    };
    if (compile("GraphPreviewVS", "vs_6_0", shader.Vertex))
    {
        compile("GraphPreviewPS", "ps_6_0", shader.Pixel);
    }
    return shader;
}

