param([Parameter(Mandatory)][string]$Log)

$ErrorActionPreference = 'Stop'
$content = Get-Content -LiteralPath $Log -Raw
if ($content -notmatch 'Step: first frame complete' -or
    $content -notmatch 'startup smoke test complete; shutdown complete') {
    throw 'Startup did not render and shut down successfully.'
}
if ($content -match 'OpenColorIO initialized|OCIO initialization error') {
    throw 'Unused OCIO display processing must not block editor startup.'
}
foreach ($milestone in @('DXR: Tier 1.1 inline raytracing ready',
    'Mesh shaders: visibility and MRT pipelines ready',
    'Phase 5 Mesh Shader PSO and indirect signature ready')) {
    if (-not $content.Contains($milestone)) {
        throw "Expected rendering capability was lost: $milestone"
    }
}
if ($content -match 'GPU Driven CPU fallback|Phase 5 traditional fallback|Native binned .* unavailable|PSO ERROR|DXR reflection shader failed') {
    throw 'A rendering pipeline failed or fell back during startup.'
}
Write-Output 'Startup initialization checks passed (first frame, DXR, mesh shaders, no unused OCIO initialization).'
