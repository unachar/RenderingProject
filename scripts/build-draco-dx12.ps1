param([ValidateSet('Static', 'Shared')][string]$Runtime = 'Shared')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$source = Join-Path $workspace 'External/Draco/upstream'
$suffix = if ($Runtime -eq 'Shared') { '-md' } else { '' }
$build = Join-Path $workspace "External/Draco/build-dx12$suffix"
$destination = Join-Path $workspace "External/Draco/install-dx12$suffix"
$runtimeLibrary = if ($Runtime -eq 'Shared') { 'MultiThreadedDLL' } else { 'MultiThreaded' }
$previousCompilerOptions = $env:_CL_
Push-Location $workspace
try {
    $env:_CL_ = (($previousCompilerOptions + ' /Z7').Trim())
    $configure = 'cmake -S "{0}" -B "{1}" -G Ninja -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="{2}" -DCMAKE_MSVC_RUNTIME_LIBRARY={3} -DBUILD_SHARED_LIBS=OFF -DDRACO_TESTS=OFF -DDRACO_BUILD_EXECUTABLES=OFF -DDRACO_TRANSCODER_SUPPORTED=OFF -DDRACO_GLTF_BITSTREAM=ON' -f $source, $build, $destination, $runtimeLibrary
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1} && cmake --build "{2}" --parallel 4 && cmake --install "{2}"' -f $developer, $configure, $build
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Draco SDK build failed.' }
}
finally {
    $env:_CL_ = $previousCompilerOptions
    Pop-Location
}
