param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
      [ValidateSet('Static', 'Shared')][string]$Runtime = 'Shared')

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$source = Join-Path $workspace 'External/fastgltf/upstream'
$suffix = if ($Runtime -eq 'Shared') { '-md' } else { '' }
$build = Join-Path $workspace "External/fastgltf/build-dx12-$Configuration$suffix"
$destination = Join-Path $workspace "External/fastgltf/install-dx12$suffix/$Configuration"
$runtimeLibrary = 'MultiThreaded$<$<CONFIG:Debug>:Debug>'
if ($Runtime -eq 'Shared') { $runtimeLibrary += 'DLL' }
$previousCompilerOptions = $env:_CL_
Push-Location $workspace
try {
    $otherConfiguration = if ($Configuration -eq 'Debug') { 'Release' } else { 'Debug' }
    foreach ($dependency in @('simdjson', 'simdutf')) {
        $cacheConfiguration = if ($Runtime -eq 'Shared') { $Configuration } else { $otherConfiguration }
        $cached = Join-Path $workspace "External/fastgltf/build-dx12-$cacheConfiguration/$dependency"
        $target = Join-Path $build $dependency
        if (Test-Path "$cached/$dependency.h") {
            New-Item -ItemType Directory $target -Force | Out-Null
            foreach ($extension in @('h', 'cpp')) {
                if (-not (Test-Path "$target/$dependency.$extension")) {
                    Copy-Item "$cached/$dependency.$extension" $target
                }
            }
        }
    }
    # Keep debug information in objects: the installed PDB compiler currently fails with C1902.
    $env:_CL_ = (($previousCompilerOptions + ' /Z7').Trim())
    # Upstream verifies exact SHA256 hashes for its simdjson/simdutf downloads.
    # Keep runtime variants separate; shared CRT is required for direct ownership with Assimp.
    $configure = 'cmake -S "{0}" -B "{1}" -G Ninja -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_BUILD_TYPE={2} -DCMAKE_INSTALL_PREFIX="{3}" -DCMAKE_MSVC_RUNTIME_LIBRARY="{4}" -DCMAKE_CXX_FLAGS= -DBUILD_SHARED_LIBS=OFF -DFASTGLTF_ENABLE_TESTS=OFF -DFASTGLTF_ENABLE_EXAMPLES=OFF -DFASTGLTF_ENABLE_DOCS=OFF' -f $source, $build, $Configuration, $destination, $runtimeLibrary
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1} && cmake --build "{2}" --parallel 4 && cmake --install "{2}"' -f $developer, $configure, $build
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'fastgltf D3D12 SDK build failed.' }
}
finally {
    $env:_CL_ = $previousCompilerOptions
    Pop-Location
}
