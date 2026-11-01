$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$source = Join-Path $workspace 'External/KTX-Software/upstream'
$build = Join-Path $workspace 'External/KTX-Software/build-dx12'
$destination = Join-Path $workspace 'External/KTX-Software/install-dx12'
$previousCompilerOptions = $env:_CL_
Push-Location $workspace
try {
    # This installed toolchain cannot generate compiler PDBs (C1902); keep debug data in objects.
    $env:_CL_ = (($previousCompilerOptions + ' /Z7').Trim())
    # Separate build: preserve the supplied SDK and omit API upload backends unused by this D3D12 renderer.
    $configure = 'cmake -S "{0}" -B "{1}" -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="{2}" -DBUILD_SHARED_LIBS=ON -DKTX_FEATURE_TESTS=OFF -DKTX_FEATURE_TOOLS=OFF -DKTX_FEATURE_DOC=OFF -DKTX_FEATURE_JNI=OFF -DKTX_FEATURE_PY=OFF -DKTX_FEATURE_JS=OFF -DLIBKTX_FEATURE_GL_UPLOAD=OFF -DLIBKTX_FEATURE_VK_UPLOAD=OFF -DLIBKTX_FEATURE_ETC_UNPACK=OFF' -f $source, $build, $destination
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1} && cmake --build "{2}" --parallel 4 && cmake --install "{2}"' -f $developer, $configure, $build
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'KTX D3D12 SDK build failed.' }
}
finally {
    $env:_CL_ = $previousCompilerOptions
    Pop-Location
}
