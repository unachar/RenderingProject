$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    foreach ($mode in @('disabled', 'enabled')) {
        $output = "bin/validation/tracy/$mode"
        New-Item -ItemType Directory -Path $output -Force | Out-Null
        $options = ''
        if ($mode -eq 'enabled') {
            $options = '/DTRACY_ENABLE /DTRACY_ON_DEMAND External/tracy/public/TracyClient.cpp'
        }
        $command = 'cl /nologo /utf-8 /EHsc /std:c++17 /O2 /MT /Z7 {0} tests/tracy_cpu_probe.cpp /Fe:{1}/probe.exe /Fo:{1}/ /link ws2_32.lib dbghelp.lib advapi32.lib user32.lib' -f $options, $output
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
        if ($LASTEXITCODE) { throw "Tracy $mode compilation failed." }
        & "./$output/probe.exe"
        if ($LASTEXITCODE) { throw "Tracy $mode smoke failed." }
    }
}
finally {
    Pop-Location
}
