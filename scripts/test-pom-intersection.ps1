param([switch]$Hardware)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $root
try {
    New-Item -ItemType Directory -Force build/pom-tests | Out-Null
    $source = [IO.File]::ReadAllText("$root/shader/hlsl/MaterialSurface.hlsli")
    $structure = [regex]::Match($source, '(?s)struct AssetMaterialParameters\s*\{.*?\};').Value
    $start = $source.IndexOf('float SampleParallaxDepth(')
    $end = $source.IndexOf('float2 ApplyAssetParallax(', $start)
    $shader = @'
#define SHADER_VISIBILITY_RESOLVE
namespace ProceduralTexture
{
    struct Parameters { uint Width; };
    static uint Profile;
    float Scalar(Parameters noise, float2 uv)
    {
        if (Profile == 1) return 1.0f;
        if (Profile == 2) return 0.0f;
        if (Profile == 3) return 0.63f;
        return 1.0f - saturate((uv.x - 0.45f) * 40.0f);
    }
}
Texture2D HeightTexture : register(t0);
SamplerState MaterialSampler : register(s0);
RWTexture2D<float4> Output : register(u0);
Texture2D LoadMaterialTexture(uint index) { return HeightTexture; }
float3 SafeNormalizeCommon(float3 value, float3 fallback) { return normalize(value); }
float3 SurfaceViewDirectionCommon(float3 position, float3 camera) { return normalize(camera - position); }
static float2 VisibilityUvDx = float2(0.0001f, 0);
static float2 VisibilityUvDy = float2(0, 0.0001f);
static float3 VisibilityWorldDx = float3(0.0001f, 0, 0);
static float3 VisibilityWorldDy = float3(0, 0.0001f, 0);
'@ + "`n" + $structure + "`n" + $source.Substring($start, $end - $start) + @'

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    AssetMaterialParameters material = (AssetMaterialParameters)0;
    material.Surface.z = 1;
    material.Noise[2].Width = id.y % 2;
    const float budgets[3] = {8, 26, 64};
    material.Parallax = float4(id.y % 12 < 6 ? 0.02f : 0.2f, budgets[(id.y / 2) % 3], 0, 0);
    ProceduralTexture::Profile = id.y / 12;
    if (id.y >= 48) material.Parallax.x = 0;
    if (id.y == 49) material.Parallax.x = 0.2f;
    float2 uv = float2(0.4f + 0.45f * id.x / 255.0f, 0.5f);
    float3 camera = id.y == 49 ? float3(0, 0, -1) : float3(0.8660254f, 0, 0.5f);
    AssetParallaxResult pom = ApplyAssetParallaxEx(material, uv, 0, float3(0, 0, 1), camera, MaterialSampler);
    Output[id.xy] = float4(pom.HitDepth, pom.Uv.x, pom.DepthScale, pom.Active ? 1 : 0);
}
'@
    [IO.File]::WriteAllText("$root/build/pom-tests/intersection-under-test.hlsl", $shader)
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    $dxc = 'packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native'
    Copy-Item "$dxc/bin/x64/dxcompiler.dll", "$dxc/bin/x64/dxil.dll" build/pom-tests/
    $command = 'cl.exe /nologo /utf-8 /EHsc /W4 /std:c++17 /O2 /I source /I "{0}/include" tests/pom_intersection_gpu_tests.cpp /Fe:build/pom-tests/pom_intersection_gpu_tests.exe /Fo:build/pom-tests/pom_intersection_gpu_tests.obj /link d3d12.lib dxgi.lib d3dcompiler.lib' -f $dxc
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'POM intersection GPU test build failed.' }
    $testArguments = @()
    if ($Hardware) { $testArguments += '--hardware' }
    & ./build/pom-tests/pom_intersection_gpu_tests.exe @testArguments
    if ($LASTEXITCODE) { throw 'POM intersection GPU regression failed.' }
}
finally { Pop-Location }
