param([ValidateSet('Debug', 'Release', 'Both')][string]$Configuration = 'Both')

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sdkDirectory = Join-Path $workspace 'External\NRD\sdk'
$nrdCommit = '792eff196afdd350fd9c3f862119017ccb438a0e'
$shaderMakeCommit = '18f5a344e7ca8fa65daaf079d07bc8ce38453e05'
$mathLibCommit = 'aa432ad9a80691eb8c3ec4d576bbad5ac100fdc8'

function Get-PinnedSource([string]$Path, [string]$Repository, [string]$Commit) {
    if (-not (Test-Path -LiteralPath (Join-Path $Path '.git'))) {
        if (Test-Path -LiteralPath $Path) {
            if (Get-ChildItem -LiteralPath $Path -Force | Select-Object -First 1) {
                throw "Refusing to replace the existing SDK directory: $Path"
            }
        }
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
        & git -C $Path init --quiet
        if ($LASTEXITCODE -ne 0) { throw "Cannot initialize SDK checkout: $Path" }
        & git -C $Path remote add origin $Repository
        if ($LASTEXITCODE -ne 0) { throw "Cannot configure SDK repository: $Path" }
        & git -C $Path fetch --depth 1 origin $Commit
        if ($LASTEXITCODE -ne 0) { throw "Cannot download official SDK source: $Repository" }
        & git -C $Path checkout --quiet --detach $Commit
        if ($LASTEXITCODE -ne 0) { throw "Cannot select the pinned SDK commit: $Path" }
    }
    $actualCommit = & git -C $Path rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $Commit) {
        throw "SDK commit mismatch at $Path. Expected $Commit; found $actualCommit."
    }
}

Get-PinnedSource $sdkDirectory 'https://github.com/NVIDIA-RTX/NRD.git' $nrdCommit
Get-PinnedSource (Join-Path $sdkDirectory 'Dependencies\ShaderMake') 'https://github.com/NVIDIA-RTX/ShaderMake.git' $shaderMakeCommit
Get-PinnedSource (Join-Path $sdkDirectory 'Dependencies\MathLib') 'https://github.com/NVIDIA-RTX/MathLib.git' $mathLibCommit

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$cmake = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$dxc = Join-Path $workspace 'packages\Microsoft.Direct3D.DXC.1.9.2602.24\build\native\bin\x64\dxc.exe'
if (-not (Test-Path -LiteralPath $cmake) -or -not (Test-Path -LiteralPath $dxc)) {
    throw 'The bundled CMake and restored DXC package are required.'
}
$buildDirectory = Join-Path $sdkDirectory '_Build'
$logsDirectory = Join-Path $sdkDirectory '_InstallLogs'
New-Item -ItemType Directory -Path $logsDirectory -Force | Out-Null

function Invoke-SdkBuild([string]$Arguments, [string]$LogName) {
    $logPath = Join-Path $logsDirectory $LogName
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && "{1}" {2}' -f $developerCommand, $cmake, $Arguments
    & $env:ComSpec /d /s /c $command *> $logPath
    if ($LASTEXITCODE -ne 0) { throw "NRD SDK operation failed. Diagnostics: $logPath" }
}

$previousCompilerOptions = $env:_CL_
try {
    $env:_CL_ = '/Z7'
    $arguments = '-S "{0}" -B "{1}" -G "Ninja Multi-Config" -DNRD_STATIC_LIBRARY=ON -DNRD_NRI=OFF -DNRD_EMBEDS_SPIRV_SHADERS=OFF -DNRD_EMBEDS_DXBC_SHADERS=OFF -DNRD_EMBEDS_DXIL_SHADERS=ON -DNRD_NORMAL_ENCODING=2 -DNRD_ROUGHNESS_ENCODING=1 "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>" -DSHADERMAKE_FIND_DXC=OFF -DSHADERMAKE_FIND_DXC_VK=OFF "-DSHADERMAKE_DXC_PATH={2}" "-DFETCHCONTENT_SOURCE_DIR_SHADERMAKE={3}" "-DFETCHCONTENT_SOURCE_DIR_MATHLIB={4}"' -f $sdkDirectory, $buildDirectory, $dxc, (Join-Path $sdkDirectory 'Dependencies\ShaderMake'), (Join-Path $sdkDirectory 'Dependencies\MathLib')
    Invoke-SdkBuild $arguments 'configure.log'
    $configurations = @($Configuration)
    if ($Configuration -eq 'Both') { $configurations = @('Release', 'Debug') }
    foreach ($buildConfiguration in $configurations) {
        Invoke-SdkBuild ('--build "{0}" --config {1} --target NRD --parallel 4' -f $buildDirectory, $buildConfiguration) "build-$buildConfiguration.log"
        $libraryPath = Join-Path $sdkDirectory "_Bin\$buildConfiguration\NRD.lib"
        if (-not (Test-Path -LiteralPath $libraryPath)) { throw "NRD library was not produced: $libraryPath" }
        Write-Output "NRD 4.17.3 $buildConfiguration library: $libraryPath"
    }
    @("NRD=$nrdCommit", "ShaderMake=$shaderMakeCommit", "MathLib=$mathLibCommit",
        'DXIL=enabled; DXBC=disabled; SPIRV=disabled; NRI=disabled',
        'CRT=MT/MTd; NormalEncoding=2; RoughnessEncoding=1') |
        Set-Content -LiteralPath (Join-Path $sdkDirectory 'installation-provenance.txt') -Encoding utf8
}
finally { $env:_CL_ = $previousCompilerOptions }
