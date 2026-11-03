$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$dxc = Join-Path $workspace 'packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64/dxc.exe'
$output = Join-Path $workspace 'build/material-graph-gpu'
New-Item -ItemType Directory -Force -Path $output | Out-Null
foreach ($bindless in @(0, 1)) {
    & $dxc -T cs_6_0 -E main -O3 -WX -D "RP_BINDLESS=$bindless" -Fo "$output/startup-preview-$bindless.cso" "$workspace/shader/hlsl/MaterialGraphGpuCS.hlsl"
    if ($LASTEXITCODE -ne 0) { throw "Material graph preview shader failed (global RP_BINDLESS=$bindless)." }
}
Write-Output 'Production material graph preview shader compiled with both global binding settings.'
