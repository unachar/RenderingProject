param([switch]$Benchmark, [switch]$AllocatorOnly, [switch]$Disabled)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    $name = 'acl_fbx_tests'
    if ($Benchmark) { $name = 'acl_fbx_benchmark' }
    New-Item -ItemType Directory -Path bin/validation/acl -Force | Out-Null
    $aclEnabled = [int](-not $Disabled)
    $aclIncludes = ''
    if (-not $Disabled) { $aclIncludes = '/I External/ACL/upstream/includes /I External/ACL/upstream/external/rtm/includes' }
    $command = 'cl /nologo /utf-8 /EHsc /std:c++17 /O2 /MT /Z7 /DRENDERINGPROJECT_USE_ACL={1} /I . /I assimp {2} tests/{0}.cpp source/models/fbxaclclip.cpp /Fe:bin/validation/acl/{0}.exe /Fo:bin/validation/acl/' -f $name, $aclEnabled, $aclIncludes
    if (-not $Benchmark) { $command += ' /FItests/acl_allocator_probe.h' }
    if ($Benchmark) { $command += ' /link External/lib/assimp-vc143-mt.lib' }
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'ACL FBX compilation failed.' }
    Copy-Item External/dll/assimp/assimp-vc143-mt.dll bin/validation/acl -Force
    if ($AllocatorOnly) { & "./bin/validation/acl/$name.exe" allocator }
    else { & "./bin/validation/acl/$name.exe" }
    if ($LASTEXITCODE) { throw 'ACL FBX validation failed.' }
    if (-not $Benchmark -and -not $Disabled) {
        $source = Get-Content source/models/animationpose.cpp -Raw
        $helperStart = $source.IndexOf('bool AnimationModelResource::TrySampleAclClip(')
        $helperEnd = $source.IndexOf('aiNodeAnim* AnimationModelResource::FindNodeAnimChannelCached(', $helperStart)
        $poseStart = $source.IndexOf('void AnimationModelResource::UpdateBoneMatrices(const char*')
        $poseEnd = $source.IndexOf('void AnimationModelResource::UpdateBoneMatrices(const vector<', $poseStart)
        if ($helperStart -lt 0 -or $helperEnd -lt 0 -or $poseStart -lt 0 -or $poseEnd -lt 0) {
            throw 'Cannot locate production ACL playback methods.'
        }
        $methods = $source.Substring($helperStart, $helperEnd - $helperStart) + $source.Substring($poseStart, $poseEnd - $poseStart)
        $methods = $methods.Replace('AnimationModelResource::', 'ResourceFixture::')
        Set-Content bin/validation/acl/acl-fbx-playback.inc -Value $methods -Encoding utf8
        $command = 'cl /nologo /utf-8 /EHsc /std:c++17 /O2 /MT /Z7 /DRENDERINGPROJECT_USE_ACL=1 /I . /I assimp /I bin/validation/acl /I External/ACL/upstream/includes /I External/ACL/upstream/external/rtm/includes tests/acl_fbx_playback_tests.cpp source/models/fbxaclclip.cpp /Fe:bin/validation/acl/playback.exe /Fo:bin/validation/acl/ /link External/lib/assimp-vc143-mt.lib'
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
        if ($LASTEXITCODE) { throw 'Production ACL playback compilation failed.' }
        foreach ($mode in @('raw', 'acl')) {
            & ./bin/validation/acl/playback.exe $mode
            if ($LASTEXITCODE) { throw "Production playback $mode failed." }
        }
    }
}
finally { Pop-Location }
