$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $root
try {
    New-Item -ItemType Directory -Force build/pom-tests | Out-Null
    $source = [IO.File]::ReadAllText("$root/shader/hlsl/MaterialSurface.hlsli")
    $structure = [regex]::Match($source, '(?s)struct AssetParallaxResult\s*\{.*?\};').Value
    $start = $source.IndexOf('float AssetParallaxSelfShadow(')
    $end = $source.IndexOf('// セルフシャドウを計算する主光源', $start)
    $function = $source.Substring($start, $end - $start)
    $shader = @'
namespace ProceduralTexture { struct Parameters { uint Width; }; }
SamplerState MaterialSampler : register(s0);
RWTexture2D<float> Output : register(u0);
static bool Blocker;
float SampleParallaxDepth(ProceduralTexture::Parameters noise, uint index, float2 uv,
                         float2 uvDx, float2 uvDy, SamplerState materialSampler)
{
    return Blocker && uv.x > 0.00001f ? 0.0f : 0.5f;
}
'@ + "`n" + $structure + "`n" + $function + @'

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    AssetParallaxResult pom = (AssetParallaxResult)0;
    pom.Active = true;
    pom.HitDepth = 0.5f;
    // Intersection interpolation can lie below the sampled height on a steep texel.
    if (id.x >= 4 && id.x < 8) pom.HitDepth = 0.60f;
    pom.DepthScale = (id.x % 2 == 0) ? 0.005f : 0.2f;
    pom.MaxSteps = (id.x % 4 < 2) ? 8 : 128;
    pom.Tangent = float3(1, 0, 0);
    pom.Bitangent = float3(0, 1, 0);
    pom.Normal = float3(0, 0, 1);
    Blocker = id.x >= 8;
    Output[id.xy] = AssetParallaxSelfShadow(pom, normalize(float3(1, 0, 1)), MaterialSampler);
}
'@
    [IO.File]::WriteAllText("$root/build/pom-tests/shadow-under-test.hlsl", $shader)
    $rays = [IO.File]::ReadAllText("$root/shader/hlsl/RaytracingCS.hlsl")
    $start = $rays.IndexOf('    float3 normal = normalize(Normal.Load')
    if ($start -lt 0) { $start = $rays.IndexOf('    float4 normalSample = Normal.Load') }
    $end = $rays.IndexOf('    float3 tangent = Tangent(normal);', $start)
    $receiver = @'
Texture2D<float4> Normal : register(t0);
RWTexture2D<float> Output : register(u0);
static float4x4 InvViewProjection = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
static float4 Distances = 0;
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;
    float2 uv = 0.5f;
    float distance = exp2((float)(id.x % 12) - 10);
    float3 position = float3(0, 0, -distance);
    if (id.x >= 12)
    {
        // Infinite reverse-Z perspective, looking through an off-center pixel.
        InvViewProjection = float4x4(1,0,0,0, 0,1,0,0, 0,0,0,10, 0,0,1,0);
        uv = float2(0.75f, 0.25f);
        position = normalize(float3(0.5f, 0.5f, 1)) * distance;
    }
'@ + "`n" + $rays.Substring($start, $end - $start) + "`n    Output[id.xy] = length(origin);`n}`n"
    [IO.File]::WriteAllText("$root/build/pom-tests/receiver-under-test.hlsl", $receiver)
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    $dxc = 'packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native'
    Copy-Item "$dxc/bin/x64/dxcompiler.dll", "$dxc/bin/x64/dxil.dll" build/pom-tests/
    $command = 'cl.exe /nologo /utf-8 /EHsc /W4 /std:c++17 /O2 /I source /I "{0}/include" tests/pom_shadow_gpu_tests.cpp /Fe:build/pom-tests/pom_shadow_gpu_tests.exe /Fo:build/pom-tests/pom_shadow_gpu_tests.obj /link d3d12.lib dxgi.lib d3dcompiler.lib' -f $dxc
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'POM GPU test build failed.' }
    & ./build/pom-tests/pom_shadow_gpu_tests.exe
    if ($LASTEXITCODE) { throw 'POM GPU regression failed.' }
}
finally { Pop-Location }
