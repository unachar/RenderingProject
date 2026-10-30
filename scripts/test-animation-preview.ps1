$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools required.' }
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$testDirectory = Join-Path $workspace 'build/animation-preview-tests'
New-Item -ItemType Directory -Force -Path $testDirectory | Out-Null
$poseSource = Get-Content -Raw -LiteralPath (Join-Path $workspace 'source/models/animationlookat.cpp')
$poseStart = $poseSource.IndexOf('bool AnimationModelResource::LoadPoseInstance(')
$poseEnd = $poseSource.IndexOf('bool AnimationModelResource::SetupLookAtRig(', $poseStart)
if ($poseStart -lt 0 -or $poseEnd -lt 0) { throw 'Cannot locate pose-instance loading.' }
Set-Content -LiteralPath (Join-Path $testDirectory 'pose-instance-loading.inc') -Value $poseSource.Substring($poseStart, $poseEnd - $poseStart) -Encoding UTF8
$ecsSource = Get-Content -Raw -LiteralPath (Join-Path $workspace 'source/ecs/ecs.cpp')
$registryStart = $ecsSource.IndexOf('unordered_map<type_index, ComponentTypeID>& ComponentTypeRegistry::TypeIds()')
$registryEnd = $ecsSource.IndexOf('void ComponentTypeRegistry::CreateComponent(', $registryStart)
if ($registryStart -lt 0 -or $registryEnd -lt 0) { throw 'Cannot locate component type registry storage.' }
Set-Content -LiteralPath (Join-Path $testDirectory 'component-type-registry.inc') -Value $ecsSource.Substring($registryStart, $registryEnd - $registryStart) -Encoding UTF8
Push-Location -LiteralPath $workspace
try {
    foreach ($test in @('animation_preview_tests', 'animation_resource_lifetime_tests', 'pose_instance_loading_tests')) {
        $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 /I build\animation-preview-tests tests\{1}.cpp /Fe:build\animation-preview-tests\{1}.exe /Fo:build\animation-preview-tests\ && build\animation-preview-tests\{1}.exe' -f $developerCommand, $test
        & $env:ComSpec /d /c $command
        if ($LASTEXITCODE) { throw "Animation preview tests failed: $test" }
    }
    $jsonRoot = 'External\RTXNS\external\donut\thirdparty\jsoncpp'
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W3 /utf-8 /I build\animation-preview-tests /I source /I source/assets /I source/core /I source/ecs /I source/editor /I source/entities /I source/models /I source/physics /I source/procedural /I source/rendering /I source/scene /I source/systems /I assimp /I External/ImGUI /I External/DirectXTex/Common /I {1}/include tests\animation_scene_settings_tests.cpp {1}\src\lib_json\json_reader.cpp {1}\src\lib_json\json_value.cpp {1}\src\lib_json\json_writer.cpp /Fe:build\animation-preview-tests\scene.exe /Fo:build\animation-preview-tests\ /link /NODEFAULTLIB:DirectXTex.lib && build\animation-preview-tests\scene.exe' -f $developerCommand, $jsonRoot
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Animation scene settings tests failed.' }
}
finally { Pop-Location }
