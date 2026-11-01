param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release', [switch]$Disabled, [switch]$Benchmark,
      [ValidateSet('Static', 'Shared')][string]$Runtime = 'Shared')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    New-Item -ItemType Directory bin/validation/fastgltf -Force | Out-Null
    python tests/generate_fastgltf_fixtures.py
    if ($LASTEXITCODE) { throw 'glTF fixture generation failed.' }
    $options = '/O2 /MT'
    if ($Configuration -eq 'Debug') { $options = '/Od /MTd /RTC1' }
    $sdkLibraries = ''
    $implementation = 'source/models/fastgltfimport.cpp'
    $enabled = [int](-not $Disabled)
    $suffix = $Configuration
    $test = 'fastgltf_import_tests'
    if ($Benchmark) { $test = 'fastgltf_import_benchmark'; $suffix += '-benchmark' }
    if ($Disabled) { $suffix += '-disabled' }
    else {
        $implementation = ''
        $runtimeSuffix = if ($Runtime -eq 'Shared') { '-md' } else { '' }
        $sdkLibraries = "/LIBPATH:External/fastgltf/install-dx12$runtimeSuffix/Release/lib FastGltfAdapter.lib"
        Copy-Item "External/fastgltf/install-dx12$runtimeSuffix/Release/bin/FastGltfAdapter.dll" bin/validation/fastgltf -Force
    }
    $command = 'cl /nologo /utf-8 /EHsc /std:c++20 {0} /Z7 /DRENDERINGPROJECT_USE_FASTGLTF={1} /I . /I assimp tests/{5}.cpp {2} /Fe:bin/validation/fastgltf/import-{3}.exe /Fo:bin/validation/fastgltf/ /link {4} External/lib/assimp-vc143-mt.lib' -f $options, $enabled, $implementation, $suffix, $sdkLibraries, $test
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'fastgltf import compilation failed.' }
    Copy-Item External/dll/assimp/assimp-vc143-mt.dll bin/validation/fastgltf -Force
    & "./bin/validation/fastgltf/import-$suffix.exe"
    if ($LASTEXITCODE) { throw 'fastgltf import validation failed.' }
}
finally { Pop-Location }
