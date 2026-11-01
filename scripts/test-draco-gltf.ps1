param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release', [switch]$DracoDisabled,
      [ValidateSet('Static', 'Shared')][string]$Runtime = 'Shared')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    New-Item -ItemType Directory bin/validation/draco -Force | Out-Null
    if (-not $DracoDisabled) {
        $encoder = 'cl /nologo /utf-8 /EHsc /std:c++20 /O2 /MT /Z7 /I External/Draco/install-dx12/include tests/draco_fixture_encoder.cpp /Fo:bin/validation/draco/ /Fe:bin/validation/draco/fixture-encoder.exe /link External/Draco/install-dx12/lib/draco.lib'
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $encoder)
        if ($LASTEXITCODE) { throw 'Draco fixture encoder build failed.' }
        & ./bin/validation/draco/fixture-encoder.exe
        if ($LASTEXITCODE) { throw 'Draco fixture encoding failed.' }
        python tests/generate_draco_fixtures.py
        if ($LASTEXITCODE) { throw 'Draco fixture generation failed.' }
    }
    $adapter = 'FastGltfAdapter'
    if ($DracoDisabled) { $adapter += 'NoDraco' }
    $runtimeSuffix = if ($Runtime -eq 'Shared') { '-md' } else { '' }
    Copy-Item "External/fastgltf/install-dx12$runtimeSuffix/Release/bin/$adapter.dll" bin/validation/draco -Force
    Copy-Item External/dll/assimp/assimp-vc143-mt.dll bin/validation/draco -Force
    $options = '/O2 /MT'
    if ($Configuration -eq 'Debug') { $options = '/Od /MTd /RTC1' }
    $command = 'cl /nologo /utf-8 /EHsc /std:c++20 {0} /Z7 /DRENDERINGPROJECT_USE_FASTGLTF=1 /I . /I assimp tests/draco_gltf_tests.cpp /Fo:bin/validation/draco/ /Fe:bin/validation/draco/gltf-{1}-{2}.exe /link /LIBPATH:External/fastgltf/install-dx12{3}/Release/lib {2}.lib External/lib/assimp-vc143-mt.lib' -f $options, $Configuration, $adapter, $runtimeSuffix
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'Draco glTF test compilation failed.' }
    if ($DracoDisabled) { & "./bin/validation/draco/gltf-$Configuration-$adapter.exe" disabled }
    else { & "./bin/validation/draco/gltf-$Configuration-$adapter.exe" }
    if ($LASTEXITCODE) { throw "Draco glTF test failed (exit $LASTEXITCODE)." }
}
finally { Pop-Location }
