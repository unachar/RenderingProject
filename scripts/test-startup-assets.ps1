param([Parameter(Mandatory)][string]$Log, [switch]$RequireGraphGpu)
$ErrorActionPreference = 'Stop'
$content = Get-Content -LiteralPath $Log -Raw
$milestones = @('Step: startup assets prepared', 'Step: startup assets ready', 'Step: window shown', 'Step: first frame complete')
$previous = -1
foreach ($milestone in $milestones) {
    $position = $content.IndexOf($milestone)
    if ($position -lt 0 -or $position -le $previous) {
        throw "Missing or out-of-order startup milestone: $milestone"
    }
    $previous = $position
}
if ($content -match 'startup asset preparation failed|startup assets timed out') {
    throw 'Startup asset preparation failed.'
}
if ($RequireGraphGpu -and $content -match 'Startup material graph preview: CPU fallback') {
    throw 'The GPU material graph preview must be ready before opening a material.'
}
Write-Output 'Startup assets were prepared before the editor became visible.'
