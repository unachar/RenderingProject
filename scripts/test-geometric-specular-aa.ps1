param([switch]$Shaders, [switch]$Gpu)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force bin/validation/technology | Out-Null
    $command = 'cl /nologo /utf-8 /EHsc /W4 /WX /std:c++17 /O2 tests/geometric_specular_aa_tests.cpp /Fe:bin/validation/technology/geometric_specular_aa_tests.exe /Fo:bin/validation/technology/geometric_specular_aa_tests.obj'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'Geometric specular AA test compilation failed.' }
    & ./bin/validation/technology/geometric_specular_aa_tests.exe
    if ($LASTEXITCODE) { throw 'Geometric specular AA math tests failed.' }
    [xml]$packages = Get-Content packages.config
    $version = ($packages.packages.package | Where-Object id -eq 'Microsoft.Direct3D.DXC').version
    $dxcRoot = Join-Path $workspace "packages/Microsoft.Direct3D.DXC.$version/build/native"
    if ($Gpu) {
        $command = 'cl /nologo /utf-8 /EHsc /W4 /std:c++17 /I "{0}/include" tests/geometric_specular_aa_gpu_tests.cpp /Fe:bin/validation/technology/geometric_specular_aa_gpu_tests.exe /Fo:bin/validation/technology/geometric_specular_aa_gpu_tests.obj /link /LIBPATH:"{0}/lib/x64" d3dcompiler.lib d3d12.lib dxgi.lib' -f $dxcRoot
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
        if ($LASTEXITCODE) { throw 'Geometric specular AA GPU fixture compilation failed.' }
        $previousPath = $env:PATH
        try {
            $env:PATH = (Join-Path $dxcRoot 'bin/x64') + ';' + $env:PATH
            & ./bin/validation/technology/geometric_specular_aa_gpu_tests.exe
            if ($LASTEXITCODE) { throw 'Geometric specular AA native D3D12 tests failed.' }
        }
        finally { $env:PATH = $previousPath }
    }
    if ($Shaders) {
        $compiler = Join-Path $dxcRoot 'bin/x64/dxc.exe'
        foreach ($enabled in @(0, 1)) {
            foreach ($shader in @('DeferredPS', 'GPUDrivenOpaquePS', 'GPUDrivenVisibilityResolveCS', 'VirtualGeometryOpaquePS')) {
                $profile = 'ps_6_5'
                if ($shader -eq 'GPUDrivenVisibilityResolveCS') { $profile = 'cs_6_5' }
                & $compiler -T $profile -E main -HV 2021 -O3 -D "GEOMETRIC_SPECULAR_AA=$enabled" -Fo "bin/validation/technology/$shader-aa$enabled.cso" "shader/hlsl/$shader.hlsl"
                if ($LASTEXITCODE) { throw "Shader failed: $shader AA=$enabled" }
                Write-Output "$shader AA=$enabled : PASS"
            }
        }
    }
}
finally { Pop-Location }
