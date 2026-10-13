param([switch]$CpuOnly, [switch]$SkipBuild, [switch]$GpuValidation, [switch]$ForceFallback,
      [ValidateSet('Debug','Release')][string]$Configuration='Debug')
$ErrorActionPreference='Stop'
$workspace=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
New-Item -ItemType Directory -Force -Path (Join-Path $workspace 'build/meshlets') | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools required.' }
$developerCommand=Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
$previousCL=$env:_CL_
$env:_CL_='/Z7'
try {
    $command='"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /I source tests\meshlet_tests.cpp source\rendering\meshlets\meshletbuilder.cpp /Fe:build\meshlets\tests.exe /Fo:build\meshlets\ && build\meshlets\tests.exe' -f $developerCommand
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Meshlet CPU tests failed.' }
    if ($CpuOnly) { return }
    if (-not $SkipBuild) {
        $command='"{0}" -no_logo -arch=amd64 -host_arch=amd64 && MSBuild.exe DirectX12.sln /t:Build /p:Configuration={1} /p:Platform=x64 /m /v:quiet /clp:ErrorsOnly /nologo /fl "/flp:logfile=build\meshlets\build-{1}.log;verbosity=minimal"' -f $developerCommand,$Configuration
        & $env:ComSpec /d /c $command
        if ($LASTEXITCODE) { throw 'Meshlet engine build failed.' }
    }
    if ($GpuValidation -and $Configuration -ne 'Debug') { throw 'GBV requires Debug.' }
    $names=@('DX12_MESHLET_STRESS_TEST','DX12_PHASE5_MESH_SHADER','DX12_PHASE5_FORCE_UNSUPPORTED','DX12_GPU_VISIBILITY_STRESS_TEST','DX12_GPU_DRIVEN_STRESS_TEST','DX12_GPU_BASED_VALIDATION','DX12_STARTUP_SMOKE_FRAMES','DX12_VALIDATION_MODE','DX12_FEATURE_SMOKE_TEST','DX12_DISABLE_DXR','DX12_DISABLE_DLSS')
    $saved=@{}
    foreach($name in $names) { $saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process') }
    try {
        $env:DX12_PHASE5_FORCE_UNSUPPORTED='0'
        if($ForceFallback) {
            $env:DX12_PHASE5_FORCE_UNSUPPORTED='1'; $env:DX12_PHASE5_MESH_SHADER='1'; $env:DX12_MESHLET_STRESS_TEST='0'
            & (Join-Path $PSScriptRoot 'validate-gpudriven.ps1') -SkipBuild -Configuration $Configuration -GpuValidation:$GpuValidation
            $fallbackLabel=if($GpuValidation) { "$Configuration-gbv" } else { $Configuration }
            $fallbackLog=Get-Content -Raw -LiteralPath (Join-Path $workspace "build/gpudriven/runtime-$fallbackLabel.log")
            Set-Content -LiteralPath (Join-Path $workspace "build/meshlets/fallback-$fallbackLabel.log") -Value $fallbackLog -Encoding utf8
            if($fallbackLog -notmatch 'Phase 5 Mesh Shader support=0' -or $fallbackLog -match 'Phase 5 Mesh Shader PSO and indirect signature ready') { throw 'Forced unsupported fallback did not execute correctly.' }
            Write-Output 'Mesh Shader unsupported feature fallback PASS'
            return
        }
        $env:DX12_MESHLET_STRESS_TEST='1'; $env:DX12_PHASE5_MESH_SHADER='0'
        $env:DX12_GPU_VISIBILITY_STRESS_TEST='0'; $env:DX12_GPU_DRIVEN_STRESS_TEST='0'
        $env:DX12_GPU_BASED_VALIDATION=[int][bool]$GpuValidation
        $env:DX12_STARTUP_SMOKE_FRAMES='112'; $env:DX12_VALIDATION_MODE='1'; $env:DX12_FEATURE_SMOKE_TEST='1'
        $env:DX12_DISABLE_DXR='1'; $env:DX12_DISABLE_DLSS='1'
        $log=Join-Path $workspace 'init_log.txt'
        $offset=if(Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
        $process=Start-Process -FilePath (Join-Path $workspace "bin/x64/$Configuration/DirectX12.exe") -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
        $timeout=if($GpuValidation) { 1800 } else { 600 }
        $timedOut=-not $process.WaitForExit($timeout*1000)
        if($timedOut) { $process.Kill(); $process.WaitForExit() }
        $process.Refresh()
        $stream=[IO.File]::OpenRead($log)
        try { $null=$stream.Seek($offset,[IO.SeekOrigin]::Begin); $reader=[IO.StreamReader]::new($stream); $runLog=$reader.ReadToEnd(); $reader.Dispose() } finally { $stream.Dispose() }
        $label=if($GpuValidation) { "$Configuration-gbv" } else { $Configuration }
        $destination=Join-Path $workspace "build/meshlets/runtime-$label.log"
        Set-Content -LiteralPath $destination -Value $runLog -Encoding utf8
        $comparison=Join-Path $workspace 'build/meshlets/comparison.csv'
        if(Test-Path -LiteralPath $comparison) { Copy-Item -LiteralPath $comparison -Destination (Join-Path $workspace "build/meshlets/comparison-$label.csv") -Force }
        if($timedOut -or $process.ExitCode -ne 0 -or $runLog -notmatch 'Meshlet validation failures = 0' -or $runLog -match 'Meshlet validation: FAIL' -or ($GpuValidation -and $runLog -notmatch 'GPU Based Validation enabled')) { throw "Meshlet GPU validation failed. See $destination" }
        Write-Output "Meshlet $label GPU validation PASS"
    } finally { foreach($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') } }
} finally { $env:_CL_=$previousCL; Pop-Location }
