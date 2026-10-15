param([ValidateSet('Debug','Release')][string]$Configuration='Debug', [switch]$Gpu,
      [switch]$Runtime, [long]$StressTriangles=0, [switch]$SkipRuntimeBuild, [switch]$KeepStressFiles)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = Join-Path $workspace 'bin/validation/virtualgeometry-phase7'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$build = Join-Path $workspace 'build/virtualgeometry'
Push-Location -LiteralPath $workspace
try {
    & $cmake -S tests/virtualgeometry -B $build -A x64 *> (Join-Path $output 'configure.log')
    if ($LASTEXITCODE -ne 0) { throw 'Phase 7 CPU configure failed.' }
    & $cmake --build $build --config $Configuration --parallel 4 *> (Join-Path $output "build-$Configuration.log")
    if ($LASTEXITCODE -ne 0) {
        Get-Content (Join-Path $output "build-$Configuration.log") -Tail 20
        throw 'Phase 7 CPU build failed.'
    }
    & (Join-Path $build "$Configuration/virtualgeometry_tests.exe") --streaming-cpu 2>&1 |
        Tee-Object -FilePath (Join-Path $output "cpu-$Configuration.log")
    if ($LASTEXITCODE -ne 0) { throw 'Phase 7 CPU validation failed.' }
    if ($Gpu -or $Runtime -or $StressTriangles) {
        $developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
        $executable = Join-Path $output 'virtualgeometry_streaming_gpu_tests.exe'
        $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl.exe /nologo /std:c++20 /EHsc /O2 /I source tests\virtualgeometry_streaming_gpu_tests.cpp source\virtualgeometry\virtualgeometrypagepool.cpp source\virtualgeometry\virtualgeometryresidency.cpp /Fe:"{1}" /Fo:"{2}\\" /link d3d12.lib dxgi.lib' -f $developerCommand,$executable,$output
        & $env:ComSpec /d /c $command *> (Join-Path $output 'gpu-build.log')
        if ($LASTEXITCODE -ne 0) {
            Get-Content (Join-Path $output 'gpu-build.log') -Tail 20
            throw 'Streaming GPU harness build failed.'
        }
        & $executable 2>&1 | Tee-Object -FilePath (Join-Path $output 'gpu-copy.log')
        if ($LASTEXITCODE -ne 0) { throw 'Streaming GPU Copy validation failed.' }
    }
    if ($Runtime -or $StressTriangles) {
        if (-not $StressTriangles) { $StressTriangles = 30000000 }
        & $executable --admission $StressTriangles 2>&1 | Tee-Object -FilePath (Join-Path $output "admission-$StressTriangles.log")
        if ($LASTEXITCODE -eq 78) { exit 78 }
        if ($LASTEXITCODE -ne 0) { throw 'Stress scene admission check failed.' }
        $stressDirectory = Join-Path $workspace "build/virtualgeometry-phase7/stress-$StressTriangles"
        New-Item -ItemType Directory -Force -Path $stressDirectory | Out-Null
        & $cmake --build $build --config Release --parallel 4 *> (Join-Path $output 'generator-build.log')
        if ($LASTEXITCODE -ne 0) { throw 'Stress generator build failed.' }
        & (Join-Path $build 'Release/virtualgeometry_tests.exe') --generate-streaming $stressDirectory $StressTriangles *> (Join-Path $output "generate-$StressTriangles.log")
        if ($LASTEXITCODE -ne 0) {
            Get-Content (Join-Path $output "generate-$StressTriangles.log") -Tail 5
            throw 'Stress Page generation failed.'
        }
        if (-not $SkipRuntimeBuild) {
            $buildCommand = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && MSBuild.exe DirectX12.sln /t:Build /p:Configuration={1} /p:Platform=x64 /m:4 /v:quiet /clp:ErrorsOnly /nologo' -f $developerCommand,$Configuration
            $previousCompilerOptions = $env:_CL_
            try {
                $env:_CL_ = '/Z7'
                & $env:ComSpec /d /c $buildCommand *> (Join-Path $output "runtime-build-$Configuration.log")
                if ($LASTEXITCODE -ne 0) {
                    Get-Content (Join-Path $output "runtime-build-$Configuration.log") -Tail 20
                    throw 'Streaming runtime build failed.'
                }
            } finally { $env:_CL_ = $previousCompilerOptions }
        }
        $names = @('DX12_VIRTUAL_GEOMETRY_STREAMING_TEST','DX12_VIRTUAL_GEOMETRY_STRESS_TEST','DX12_STARTUP_SMOKE_FRAMES',
            'DX12_FEATURE_SMOKE_TEST','DX12_VALIDATION_MODE','DX12_DISABLE_DXR','DX12_DISABLE_DLSS',
            'DX12_VG_STRESS_DIRECTORY','DX12_VG_STRESS_TRIANGLES','DX12_VG_STREAMING_IO_DELAY_MS','DX12_VG_STREAMING_IO_FAIL_ONCE')
        $saved = @{}
        foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
        $settingsPath = Join-Path $workspace 'Save/project_environment.cfg'
        $settingsBytes = if (Test-Path -LiteralPath $settingsPath) { [IO.File]::ReadAllBytes($settingsPath) } else { $null }
        try {
            $env:DX12_VIRTUAL_GEOMETRY_STREAMING_TEST='1'
            $env:DX12_VIRTUAL_GEOMETRY_STRESS_TEST='0'
            $env:DX12_STARTUP_SMOKE_FRAMES='240'
            $env:DX12_FEATURE_SMOKE_TEST='1'
            $env:DX12_VALIDATION_MODE='1'
            $env:DX12_DISABLE_DXR='1'
            $env:DX12_DISABLE_DLSS='1'
            $env:DX12_VG_STRESS_DIRECTORY=$stressDirectory
            $env:DX12_VG_STRESS_TRIANGLES=[string]$StressTriangles
            $env:DX12_VG_STREAMING_IO_DELAY_MS='10'
            $env:DX12_VG_STREAMING_IO_FAIL_ONCE='1'
            $log = Join-Path $workspace 'init_log.txt'
            $offset = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
            $process = Start-Process -FilePath (Join-Path $workspace "bin/x64/$Configuration/DirectX12.exe") -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
            $deadline = [DateTime]::UtcNow.AddMinutes(10)
            while (-not $process.WaitForExit(1000)) {
                if ([DateTime]::UtcNow -ge $deadline) { $process.Kill(); throw 'Streaming runtime validation timed out.' }
            }
            $stream = [IO.File]::OpenRead($log)
            try {
                $null = $stream.Seek($offset,[IO.SeekOrigin]::Begin)
                $reader = [IO.StreamReader]::new($stream)
                $runLog = $reader.ReadToEnd()
            } finally { $stream.Dispose() }
            Set-Content -LiteralPath (Join-Path $output "runtime-$Configuration-$StressTriangles.log") -Value $runLog -Encoding utf8
            if ($process.ExitCode -ne 0 -or $runLog -notmatch 'Geometry streaming validation failures = 0') {
                throw "Streaming runtime failed; see runtime-$Configuration-$StressTriangles.log"
            }
            Write-Output "PASS: $Configuration streaming runtime, requested source triangles=$StressTriangles"
        } finally {
            foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
            if ($null -ne $settingsBytes) { [IO.File]::WriteAllBytes($settingsPath,$settingsBytes) }
        }
        if (-not $KeepStressFiles) {
            $resolvedStress = [IO.Path]::GetFullPath($stressDirectory)
            $expectedRoot = [IO.Path]::GetFullPath((Join-Path $workspace 'build/virtualgeometry-phase7')) + [IO.Path]::DirectorySeparatorChar
            if (-not $resolvedStress.StartsWith($expectedRoot,[StringComparison]::OrdinalIgnoreCase)) { throw 'Stress cleanup path escaped validation directory.' }
            Get-ChildItem -LiteralPath $resolvedStress -File | Where-Object { $_.Name -match '^(asset-\d+\.vgp2|fallback-\d+\.obj)$' } | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
        }
    }
} finally { Pop-Location }
