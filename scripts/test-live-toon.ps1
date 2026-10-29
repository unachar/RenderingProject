param([switch]$Shaders)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $root
try {
    New-Item -ItemType Directory -Force bin/validation/live-toon | Out-Null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
    $developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    foreach ($test in @('stage_lighting', 'toon_lighting', 'toon_outline_projection', 'skinning_work')) {
        # Keep assertions active: these regression executables intentionally use assert.
        $command = 'cl.exe /nologo /utf-8 /EHsc /W4 /std:c++17 /O2 /I source tests/{0}_tests.cpp /Fe:bin/validation/live-toon/{0}.exe /Fo:bin/validation/live-toon/{0}.obj' -f $test
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
        if ($LASTEXITCODE) { throw "Build failed: $test" }
        & "./bin/validation/live-toon/$test.exe"
        if ($LASTEXITCODE) { throw "Test failed: $test" }
    }
    if ($Shaders) {
        [xml]$packages = Get-Content packages.config
        $version = ($packages.packages.package | Where-Object { $_.id -eq 'Microsoft.Direct3D.DXC' }).version
        $compiler = "packages/Microsoft.Direct3D.DXC.$version/build/native/bin/x64/dxc.exe"
        $variants = @(
            @('toonOutlineVS', 'vs_6_0', 'main', ''),
            @('skinning_cs', 'cs_6_0', 'main', ''),
            @('DeferredLightingPS', 'ps_6_0', 'main', ''),
            @('modelshaderPS', 'ps_6_0', 'main', ''),
            @('DeferredLightingPS', 'ps_6_5', 'main', 'SHADER_RAYTRACING=1'),
            @('modelshaderPS', 'ps_6_5', 'main', 'SHADER_RAYTRACING=1'),
            @('DeferredLightingPS', 'ps_6_5', 'MegaLightsShadePS', 'SHADER_RAYTRACING=1'),
            @('DeferredPS', 'ps_6_0', 'main', ''),
            @('modelshaderPS_MRT', 'ps_6_0', 'main', ''),
            @('GeometryPS', 'ps_6_0', 'main', ''),
            @('GPUDrivenOpaquePS', 'ps_6_0', 'main', ''),
            @('GBufferFromVisibilityCS', 'cs_6_0', 'main', ''),
            @('GPUDrivenVisibilityResolveCS', 'cs_6_0', 'main', '')
        )
        $index = 0
        foreach ($variant in $variants) {
            $arguments = @('-T', $variant[1], '-E', $variant[2], '-O3', '-Fo', "bin/validation/live-toon/shader-$index.cso")
            if ($variant[3]) { $arguments += @('-D', $variant[3]) }
            & $compiler @arguments "shader/hlsl/$($variant[0]).hlsl"
            if ($LASTEXITCODE) { throw "Shader failed: $variant" }
            $index++
        }
    }
}
finally { Pop-Location }
