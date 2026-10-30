$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force bin/validation/technology | Out-Null
    foreach ($configuration in @('debug', 'release')) {
        $flags = '/MT'
        if ($configuration -eq 'debug') { $flags = '/D_DEBUG /MTd' }
        $command = 'cl.exe /nologo /utf-8 /EHsc /W4 /std:c++17 {0} /external:I External/PIXEvents/include /external:W0 tests/gpumarkers_tests.cpp /Fe:bin/validation/technology/gpumarkers-{1}.exe /Fo:bin/validation/technology/gpumarkers-{1}.obj /link d3d12.lib dxgi.lib' -f $flags, $configuration
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
        if ($LASTEXITCODE) { throw "GPU marker compilation failed: $configuration" }
        & "./bin/validation/technology/gpumarkers-$configuration.exe"
        if ($LASTEXITCODE) { throw "GPU marker tests failed: $configuration" }
    }
}
finally { Pop-Location }
