param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('Build', 'Rebuild')][string]$Target = 'Rebuild',
    [int]$MaximumSeconds = 300,
    [ValidateRange(1, 16)][int]$BuildNodes = 4
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if (-not $installation) { throw 'Visual Studio MSBuild is required.' }
$msbuild = Join-Path $installation 'MSBuild/Current/Bin/amd64/MSBuild.exe'
$directory = Join-Path $root 'build/build-performance'
New-Item -ItemType Directory -Force $directory | Out-Null
$name = '{0}-{1}-{2}' -f $Configuration, $Target, (Get-Date -Format 'yyyyMMdd-HHmmss')
$log = Join-Path $directory "$name.log"
$binaryLog = Join-Path $directory "$name.binlog"
Push-Location -LiteralPath $root
try {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & $msbuild DirectX12.sln "/t:$Target" "/p:Configuration=$Configuration" /p:Platform=x64 "/m:$BuildNodes" /nr:false /nologo /v:minimal /clp:PerformanceSummary /fl "/flp:logfile=$log;verbosity=normal" "/bl:$binaryLog"
    $buildExitCode = $LASTEXITCODE
    $timer.Stop()
    $measurement = [pscustomobject]@{
        Configuration = $Configuration
        Platform = 'x64'
        Target = $Target
        BuildNodes = $BuildNodes
        Seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 3)
        ExitCode = $buildExitCode
        MaximumSeconds = $MaximumSeconds
        WithinTarget = $timer.Elapsed.TotalSeconds -le $MaximumSeconds
        CompilerEnvironment = $env:_CL_
        Log = $log
        BinaryLog = $binaryLog
    }
    $measurement | ConvertTo-Json | Tee-Object -FilePath (Join-Path $directory "$name.json")
    if ($buildExitCode -ne 0) { throw "Build failed: $log" }
    if (-not $measurement.WithinTarget) { throw "Build exceeded $MaximumSeconds seconds: $($measurement.Seconds) seconds." }
}
finally {
    Pop-Location
}
