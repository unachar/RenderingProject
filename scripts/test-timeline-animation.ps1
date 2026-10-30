$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools required.' }
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$testDirectory = Join-Path $workspace 'build/timeline-animation-tests'
New-Item -ItemType Directory -Force -Path $testDirectory | Out-Null

function Export-TestSection([string]$Source, [string]$Start, [string]$End, [string]$Output) {
    $contents = Get-Content -Raw -LiteralPath (Join-Path $workspace $Source)
    $first = $contents.IndexOf($Start)
    $last = $contents.IndexOf($End, $first)
    if ($first -lt 0 -or $last -lt 0) { throw "Cannot locate test section: $Start" }
    Set-Content -LiteralPath (Join-Path $testDirectory $Output) -Value $contents.Substring($first, $last - $first) -Encoding UTF8
}

Export-TestSection 'source/ecs/ecs.cpp' 'unordered_map<type_index, ComponentTypeID>& ComponentTypeRegistry::TypeIds()' 'void ComponentTypeRegistry::CreateComponent(' 'component-type-registry.inc'
Export-TestSection 'source/systems/timelinesystem.cpp' 'bool TimeLineSystem::PrepareTrackAnimations(' 'void TimeLineSystem::EvaluateComponent(' 'timeline-animation-selection.inc'
Export-TestSection 'source/models/animator.cpp' 'void Animator::Play(AnimationModelComponent& animation, bool restart)' 'void Animator::CrossFade(AnimationModelComponent& animation, float blendRate,' 'animator-model-play.inc'
Export-TestSection 'source/models/animator.cpp' 'void Animator::Update(AnimationModelComponent& animation, float deltaTime)' 'void Animator::Update(AnimationModelComponent& animation, const string& currentAnimation,' 'animator-model-update.inc'

Push-Location -LiteralPath $workspace
try {
    $jsonRoot = 'External/RTXNS/external/donut/thirdparty/jsoncpp'
    $includes = '/I build/timeline-animation-tests /I source /I source/assets /I source/core /I source/ecs /I source/editor /I source/entities /I source/models /I source/physics /I source/procedural /I source/rendering /I source/scene /I source/systems /I assimp /I External/ImGUI /I External/DirectXTex/Common'
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W3 /utf-8 {1} /I {2}/include tests/timeline_animation_tests.cpp source/models/vmdanimationimpoter.cpp {2}/src/lib_json/json_reader.cpp {2}/src/lib_json/json_value.cpp {2}/src/lib_json/json_writer.cpp /Fe:build/timeline-animation-tests/tests.exe /Fo:build/timeline-animation-tests/ /link /NODEFAULTLIB:DirectXTex.lib /NODEFAULTLIB:assimp-vc143-mt.lib && build\timeline-animation-tests\tests.exe' -f $developerCommand, $includes, $jsonRoot
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Timeline animation tests failed.' }
}
finally { Pop-Location }
