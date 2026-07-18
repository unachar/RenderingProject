param(
    [ValidateSet("Debug", "Release", "All")]
    [string]$Configuration = "All",
    [int]$Jobs = 4
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$externalRoot = Join-Path $projectRoot "External"
$buildRoot = Join-Path $externalRoot "Build"

function Invoke-Native {
    param([string]$Executable, [string[]]$Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable failed with exit code $LASTEXITCODE"
    }
}

function Ensure-Repository {
    param(
        [string]$Directory,
        [string]$Url,
        [string]$Tag,
        [string]$Commit
    )

    if (-not (Test-Path -LiteralPath (Join-Path $Directory ".git"))) {
        Invoke-Native "git" @(
            "clone", "--depth", "1", "--branch", $Tag, $Url, $Directory
        )
    }

    $actual = (& git -C $Directory rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect $Directory"
    }
    if ($actual -ne $Commit) {
        Invoke-Native "git" @("-C", $Directory, "fetch", "--depth", "1", "origin", $Commit)
        Invoke-Native "git" @("-C", $Directory, "checkout", "--detach", $Commit)
    }
}

function Find-VcVars64 {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "Visual Studio Installer (vswhere.exe) was not found."
    }
    $installation = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if (-not $installation) {
        throw "Visual Studio C++ x64 tools were not found."
    }
    return Join-Path $installation "VC\Auxiliary\Build\vcvars64.bat"
}

function Invoke-VcCommand {
    param([string]$Command)
    $vcvars = Find-VcVars64
    $commandLine = "call `"$vcvars`" >nul && $Command"
    & cmd.exe /d /s /c $commandLine
    if ($LASTEXITCODE -ne 0) {
        throw "Native build command failed with exit code $LASTEXITCODE"
    }
}

New-Item -ItemType Directory -Force -Path $externalRoot, $buildRoot | Out-Null

Ensure-Repository `
    (Join-Path $externalRoot "BulletPhysics") `
    "https://github.com/bulletphysics/bullet3.git" `
    "3.25" `
    "2c204c49e56ed15ec5fcfa71d199ab6d6570b3f5"
Ensure-Repository `
    (Join-Path $externalRoot "JoltPhysics") `
    "https://github.com/jrouwe/JoltPhysics.git" `
    "v5.6.0" `
    "e77f175595e64cb44218cc9d9d56fc365ad0e36a"
Ensure-Repository `
    (Join-Path $externalRoot "PhysX") `
    "https://github.com/NVIDIA-Omniverse/PhysX.git" `
    "107.3-physx-5.6.1" `
    "5ca9f472105a90d70d957c243cb0ef36fe251a9f"

$bulletSource = (Join-Path $externalRoot "BulletPhysics").Replace("\", "/")
$bulletBuild = (Join-Path $buildRoot "Bullet-vs18").Replace("\", "/")
$joltSource = (Join-Path $externalRoot "JoltPhysics\Build").Replace("\", "/")
$joltBuild = (Join-Path $buildRoot "Jolt-vs18").Replace("\", "/")
$physxRoot = (Join-Path $externalRoot "PhysX\physx").Replace("\", "/")
$physxSource = (Join-Path $physxRoot "source\compiler\cmake").Replace("\", "/")
$physxBuild = (Join-Path $buildRoot "PhysX-vs18-static").Replace("\", "/")
$physxOutput = (Join-Path $buildRoot "PhysX-output-static").Replace("\", "/")
$freeglutPlaceholder = Join-Path $buildRoot "PhysX-freeglut-placeholder\win64"
New-Item -ItemType Directory -Force -Path $freeglutPlaceholder | Out-Null
New-Item -ItemType File -Force -Path `
    (Join-Path $freeglutPlaceholder "freeglut.dll"), `
    (Join-Path $freeglutPlaceholder "freeglutd.dll") | Out-Null
$freeglutRoot = (Split-Path -Parent $freeglutPlaceholder).Replace("\", "/")

Invoke-VcCommand "cmake -S `"$bulletSource`" -B `"$bulletBuild`" -G `"Ninja Multi-Config`" -DBUILD_BULLET2_DEMOS=OFF -DBUILD_BULLET3=OFF -DBUILD_CPU_DEMOS=OFF -DBUILD_EXTRAS=OFF -DBUILD_OPENGL3_DEMOS=OFF -DBUILD_PYBULLET=OFF -DBUILD_UNIT_TESTS=OFF -DBUILD_SHARED_LIBS=OFF -DUSE_MSVC_RUNTIME_LIBRARY_DLL=OFF"
Invoke-VcCommand "cmake -S `"$joltSource`" -B `"$joltBuild`" -G `"Ninja Multi-Config`" -DTARGET_HELLO_WORLD=OFF -DTARGET_PERFORMANCE_TEST=OFF -DTARGET_SAMPLES=OFF -DTARGET_UNIT_TESTS=OFF -DTARGET_VIEWER=OFF -DUSE_STATIC_MSVC_RUNTIME_LIBRARY=ON -DDEBUG_RENDERER_IN_DEBUG_AND_RELEASE=ON -DFLOATING_POINT_EXCEPTIONS_ENABLED=ON -DPROFILER_IN_DEBUG_AND_RELEASE=ON"
Invoke-VcCommand "cmake -S `"$physxSource`" -B `"$physxBuild`" -G `"Ninja Multi-Config`" -DCMAKE_MODULE_PATH=`"$physxRoot/source/compiler/cmake/modules`" -DPHYSX_ROOT_DIR=`"$physxRoot`" -DTARGET_BUILD_PLATFORM=windows -DPX_OUTPUT_BIN_DIR=`"$physxOutput`" -DPX_OUTPUT_LIB_DIR=`"$physxOutput`" -DPX_GENERATE_STATIC_LIBRARIES=ON -DNV_USE_STATIC_WINCRT=ON -DNV_USE_DEBUG_WINCRT=ON -DPUBLIC_RELEASE=ON -DPX_BUILDSNIPPETS=OFF -DPX_BUILD_OMNI_PVD=OFF -DPX_BUILDPVDRUNTIME=OFF -DPX_GENERATE_GPU_PROJECTS=OFF -DPX_COPY_EXTERNAL_DLL=OFF -DPHYSX_SLN_FREEGLUT_PATH=`"$freeglutRoot`""

$configurations = if ($Configuration -eq "All") { @("Debug", "Release") } else { @($Configuration) }
foreach ($config in $configurations) {
    Invoke-VcCommand "cmake --build `"$bulletBuild`" --config $config --target BulletDynamics BulletCollision LinearMath -- -j $Jobs"
    Invoke-VcCommand "cmake --build `"$joltBuild`" --config $config --target Jolt -- -j $Jobs"
    Invoke-VcCommand "cmake --build `"$physxBuild`" --config $($config.ToLowerInvariant()) --target PhysXExtensions -- -j $Jobs"
}

Write-Host "Physics dependencies are ready for $Configuration."
