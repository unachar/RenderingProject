param([switch]$SkipMillion)

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'Visual Studio C++ tools are required.'
}
$cmake = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) {
    $cmake = (Get-Command cmake.exe).Source
}
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$buildDirectory = Join-Path $workspace 'build\virtualgeometry'
$resultsDirectory = Join-Path $workspace 'bin\validation\virtualgeometry'

function Invoke-Checked([string]$Executable, [string[]]$Arguments, [string]$Log)
{
    & $Executable @Arguments *> $Log
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $Log -Tail 35
        throw "Virtual geometry validation failed. See $Log"
    }
    Write-Output "PASS: $Log"
}

Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Path $resultsDirectory -Force | Out-Null
    Invoke-Checked $cmake @('-S','tests/virtualgeometry','-B',$buildDirectory,'-A','x64') (Join-Path $resultsDirectory 'configure.log')
    foreach ($configuration in @('Debug','Release')) {
        Invoke-Checked $cmake @('--build',$buildDirectory,'--config',$configuration,'--parallel','4') (Join-Path $resultsDirectory "build-$configuration.log")
        $testArguments = @('--test-dir',$buildDirectory,'-C',$configuration,'--output-on-failure','-V')
        if ($configuration -eq 'Debug' -or $SkipMillion) {
            $testArguments += @('-LE','stress')
        }
        Invoke-Checked $ctest $testArguments (Join-Path $resultsDirectory "tests-$configuration.log")
    }
}
finally {
    Pop-Location
}
