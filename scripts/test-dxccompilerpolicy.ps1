$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force bin/validation/technology | Out-Null
    [xml]$packages = Get-Content packages.config
    $version = ($packages.packages.package | Where-Object id -eq 'Microsoft.Direct3D.DXC').version
    $dxcRoot = Join-Path $workspace "packages/Microsoft.Direct3D.DXC.$version/build/native"
    $command = 'cl /nologo /utf-8 /EHsc /W4 /std:c++17 /I "{0}/include" tests/dxccompilerpolicy_tests.cpp /Fe:bin/validation/technology/dxccompilerpolicy_tests.exe /Fo:bin/validation/technology/dxccompilerpolicy_tests.obj /link /LIBPATH:"{0}/lib/x64" d3dcompiler.lib d3d12.lib dxgi.lib' -f $dxcRoot
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'DXC policy test compilation failed.' }
    $previousPath = $env:PATH
    try {
        $env:PATH = (Join-Path $dxcRoot 'bin/x64') + ';' + $env:PATH
        # The child process owns environment overrides; the caller's settings are preserved.
        & ./bin/validation/technology/dxccompilerpolicy_tests.exe
        if ($LASTEXITCODE) { throw 'DXC policy tests failed.' }
    }
    finally { $env:PATH = $previousPath }
}
finally { Pop-Location }
