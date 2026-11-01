param([switch]$DisableDraco, [ValidateSet('Static', 'Shared')][string]$Runtime = 'Shared')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    $suffix = if ($Runtime -eq 'Shared') { '-md' } else { '' }
    $sdk = "External/fastgltf/install-dx12$suffix/Release"
    $dracoSdk = "External/Draco/install-dx12$suffix"
    $objects = "External/fastgltf/build-adapter$suffix"
    $compilerRuntime = if ($Runtime -eq 'Shared') { '/MD /DFASTGLTF_SHARED_CRT_SCENE=1' } else { '/MT' }
    if ($Runtime -eq 'Shared') {
        $toolVersion = (Get-Content (Join-Path $installation 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
        $dumpbin = Join-Path $installation "VC/Tools/MSVC/$toolVersion/bin/Hostx64/x64/dumpbin.exe"
        $dependencies = (& $dumpbin /dependents External/dll/assimp/assimp-vc143-mt.dll) -join "`n"
        if ($LASTEXITCODE -or $dependencies -notmatch 'MSVCP140\.dll' -or
            $dependencies -notmatch 'VCRUNTIME140\.dll' -or
            $dependencies -notmatch '(api-ms-win-crt-heap-l1-1-0|ucrtbase)\.dll' -or
            $dependencies -match '(MSVCP140D|VCRUNTIME140D)\.dll') {
            throw 'Direct scene ownership requires Assimp linked to the same shared Release CRT; use -Runtime Static for a copied scene.'
        }
    }
    if (-not (Test-Path "$sdk/lib/fastgltf.lib")) {
        throw "Build the Release SDK with scripts/build-fastgltf-dx12.ps1 -Runtime $Runtime first."
    }
    New-Item -ItemType Directory "$sdk/bin" -Force | Out-Null
    New-Item -ItemType Directory $objects -Force | Out-Null
    $draco = ''
    $dracoLibrary = ''
    $outputName = 'FastGltfAdapter'
    if ($DisableDraco) { $outputName += 'NoDraco' }
    if (-not $DisableDraco -and (Test-Path "$dracoSdk/lib/draco.lib")) {
        $draco = "/DRENDERINGPROJECT_USE_DRACO=1 /I $dracoSdk/include"
        $dracoLibrary = "$dracoSdk/lib/draco.lib"
    }
    # Keep Release STL/Assimp ABI in its own DLL, isolated from the application's Debug STL.
    $command = 'cl /nologo /utf-8 /EHsc /std:c++20 /O2 {3} /Z7 /LD /DRENDERINGPROJECT_USE_FASTGLTF=1 /DFASTGLTF_IMPORT_BUILD_DLL {0} /I assimp /I {4}/include source/models/fastgltfimport.cpp source/models/gltfdraco.cpp /Fo:{5}/ /Fe:{4}/bin/{2}.dll /link /IMPLIB:{4}/lib/{2}.lib /LIBPATH:{4}/lib fastgltf.lib fastgltf_simdjson.lib fastgltf_simdutf.lib {1} External/lib/assimp-vc143-mt.lib' -f $draco, $dracoLibrary, $outputName, $compilerRuntime, $sdk, $objects
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'fastgltf adapter DLL build failed.' }
}
finally { Pop-Location }
