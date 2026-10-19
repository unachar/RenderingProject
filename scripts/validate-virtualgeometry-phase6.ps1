param([switch]$Warp, [switch]$SkipCpu, [switch]$Runtime, [switch]$SkipRuntimeBuild,
      [switch]$GpuValidation, [ValidateSet('Debug','Release')][string]$Configuration='Debug')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location -LiteralPath $workspace
try {
    if (-not $SkipCpu) {
        & (Join-Path $PSScriptRoot 'validate-virtualgeometry.ps1') -SkipMillion
        if ($LASTEXITCODE -ne 0) {
            throw 'VirtualGeometry CPU tests failed.'
        }
    }
    $output = Join-Path $workspace 'bin/validation/virtualgeometry-phase6'
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $dxc = Join-Path $workspace 'packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64/dxc.exe'
    foreach ($item in @(@('Reset','VirtualGeometryResetCS'),@('Traverse','VirtualGeometryTraversalCS'),@('BuildArguments','VirtualGeometryArgumentsCS'))) {
        & $dxc -T cs_6_5 -E $item[0] -O3 -Fo "shader/hlsl/build/$($item[1]).cso" shader/hlsl/VirtualGeometryTraversalCS.hlsl
        if ($LASTEXITCODE -ne 0) {
            throw 'Traversal shader compilation failed.'
        }
    }
    & $dxc -T ms_6_5 -E main -O3 -Fo shader/hlsl/build/VirtualGeometryMS.cso shader/hlsl/VirtualGeometryMS.hlsl
    if ($LASTEXITCODE -ne 0) {
        throw 'Native Mesh Shader compilation failed.'
    }
    & $dxc -T ms_6_5 -E main -O3 -Fo shader/hlsl/build/VirtualGeometryVelocityMS.cso shader/hlsl/VirtualGeometryVelocityMS.hlsl
    if ($LASTEXITCODE -ne 0) {
        throw 'Native velocity Mesh Shader compilation failed.'
    }
    & $dxc -T ps_6_0 -E main -O3 -Fo shader/hlsl/build/VirtualGeometryVelocityValidationPS.cso tests/virtualgeometry_velocity_validation_ps.hlsl
    if ($LASTEXITCODE -ne 0) {
        throw 'Velocity validation pixel shader compilation failed.'
    }
    & $dxc -T ps_6_0 -E main -O3 -Fo shader/hlsl/build/VirtualGeometryValidationPS.cso tests/virtualgeometry_raster_validation_ps.hlsl
    if ($LASTEXITCODE -ne 0) {
        throw 'Validation pixel shader compilation failed.'
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) {
        throw 'Visual Studio C++ tools required.'
    }
    $developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    $executable = Join-Path $output 'virtualgeometry_traversal_gpu_tests.exe'
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl.exe /nologo /utf-8 /std:c++20 /EHsc /O2 /I source tests\virtualgeometry_traversal_gpu_tests.cpp source\virtualgeometry\virtualgeometryselection.cpp /Fe:"{1}" /Fo:"{2}\\" /link d3d12.lib dxgi.lib' -f $developerCommand,$executable,$output
    & $env:ComSpec /d /c $command *> (Join-Path $output 'build.log')
    if ($LASTEXITCODE -ne 0) {
        Get-Content (Join-Path $output 'build.log') -Tail 20
        throw 'GPU traversal harness compilation failed.'
    }
    $arguments = @()
    if ($Warp) {
        $arguments += '--warp'
    }
    & $executable @arguments | Tee-Object -FilePath (Join-Path $output 'gpu-tests.log')
    if ($LASTEXITCODE -ne 0) {
        throw 'Production GPU traversal/reference comparison failed.'
    }
    if ($Runtime) {
        if (-not $SkipRuntimeBuild) {
            $buildCommand = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && MSBuild.exe DirectX12.sln /t:Build /p:Configuration={1} /p:Platform=x64 /m /v:quiet /clp:ErrorsOnly /nologo' -f $developerCommand,$Configuration
            $previousCompilerOptions = $env:_CL_
            try {
                $env:_CL_ = '/Z7'
                & $env:ComSpec /d /c $buildCommand
                if ($LASTEXITCODE -ne 0) {
                    throw 'Native runtime build failed.'
                }
            } finally {
                $env:_CL_ = $previousCompilerOptions
            }
        }
        $names = @('DX12_VIRTUAL_GEOMETRY_STRESS_TEST','DX12_STARTUP_SMOKE_FRAMES','DX12_FEATURE_SMOKE_TEST',
            'DX12_VALIDATION_MODE','DX12_DISABLE_DXR','DX12_DISABLE_DLSS','DX12_GPU_BASED_VALIDATION')
        $saved = @{}
        foreach ($name in $names) {
            $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process')
        }
        try {
            $env:DX12_VIRTUAL_GEOMETRY_STRESS_TEST='1'
            $env:DX12_STARTUP_SMOKE_FRAMES='180'
            $env:DX12_FEATURE_SMOKE_TEST='1'
            $env:DX12_VALIDATION_MODE='1'
            $env:DX12_DISABLE_DXR='1'
            $env:DX12_DISABLE_DLSS='1'
            $env:DX12_GPU_BASED_VALIDATION=[int][bool]$GpuValidation
            $log = Join-Path $workspace 'init_log.txt'
            $offset = if (Test-Path -LiteralPath $log) {
                (Get-Item -LiteralPath $log).Length
            } else {
                0
            }
            $process = Start-Process -FilePath (Join-Path $workspace "bin/x64/$Configuration/DirectX12.exe") -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
            $runtimeTimeout = if ($GpuValidation) {
                900000
            } else {
                180000
            }
            if (-not $process.WaitForExit($runtimeTimeout)) {
                $process.Kill()
                throw 'Native runtime validation exceeded its timeout.'
            }
            $stream = [IO.File]::OpenRead($log)
            try {
                $null = $stream.Seek($offset,[IO.SeekOrigin]::Begin)
                $reader = [IO.StreamReader]::new($stream)
                $runLog = $reader.ReadToEnd()
            } finally {
                $stream.Dispose()
            }
            $label = if ($GpuValidation) {
                "$Configuration-gbv"
            } else {
                $Configuration
            }
            Set-Content -LiteralPath (Join-Path $output "runtime-$label.log") -Value $runLog -Encoding utf8
            if ($process.ExitCode -ne 0 -or $runLog -notmatch 'VirtualGeometry validation failures = 0') {
                throw "Native runtime validation failed. See $output/runtime-$label.log"
            }
            Write-Output "PASS: native $label frame-pipeline acceptance"
        } finally {
            foreach ($name in $names) {
                [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')
            }
        }
    }
}
finally {
    Pop-Location
}
