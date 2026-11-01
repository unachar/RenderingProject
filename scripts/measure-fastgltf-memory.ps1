$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    Copy-Item External/dll/assimp/assimp-vc143-mt.dll bin/validation/fastgltf -Force
    $command = 'cl /nologo /utf-8 /EHsc /std:c++20 /O2 /MD /Z7 /I . /I assimp tests/fastgltf_memory_probe.cpp /Fo:bin/validation/fastgltf/ /Fe:bin/validation/fastgltf/memory-probe.exe /link psapi.lib External/lib/assimp-vc143-mt.lib'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'Memory probe compilation failed.' }
    $variants = [ordered]@{
        native = 'native'
        before = 'bin/validation/fastgltf/optimization-before/FastGltfAdapter.dll'
        shared = 'External/fastgltf/install-dx12-md/Release/bin/FastGltfAdapter.dll'
    }
    $results = @()
    foreach ($trial in 1..3) {
        foreach ($entry in $variants.GetEnumerator()) {
            $data = & ./bin/validation/fastgltf/memory-probe.exe $entry.Value 'External/Draco/upstream/testdata/CesiumMan/glTF_Binary/CesiumMan.glb'
            if ($LASTEXITCODE) { throw "Memory worker failed: $($entry.Key)" }
            $record = $data | ConvertFrom-Json
            $record | Add-Member mode $entry.Key
            $record | Add-Member trial $trial
            $results += $record
        }
    }
    $results | ConvertTo-Json -Depth 5 | Set-Content bin/validation/fastgltf/memory-result.json
    $results | Group-Object mode | ForEach-Object {
        [pscustomobject]@{ Mode=$_.Name; MedianPeakCommitBytes=($_.Group.peakCommitBytes | Sort-Object)[1] }
    } | Format-Table -AutoSize
}
finally { Pop-Location }
