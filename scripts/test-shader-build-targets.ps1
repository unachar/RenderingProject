param([string]$TargetsPath = (Join-Path $PSScriptRoot '..\Directory.Build.targets'))
$ErrorActionPreference = 'Stop'
[xml]$document = Get-Content -LiteralPath $TargetsPath
$targets = @($document.Project.Target)
$shaderTargets = @($targets | Where-Object { $_.Inputs -and $_.Name -ne 'CompileShadowCacheShaders' })
$shadowCache = $targets | Where-Object Name -eq 'CompileShadowCacheShaders'
if (@($shadowCache.Exec).Count -ne 2 -or @($shadowCache.Outputs.Split(';')).Count -ne 2) {
    throw 'Shadow cache must track both vertex and pixel shader outputs.'
}
if ($shaderTargets.Count -ne 44) {
    throw 'Expected 44 individually tracked DXC shader outputs.'
}
if (@($targets | Group-Object Name | Where-Object Count -gt 1).Count) {
    throw 'Duplicate target names.'
}
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')) + '\'
$namespaces = [System.Xml.XmlNamespaceManager]::new($document.NameTable)
$namespaces.AddNamespace('build', $document.DocumentElement.NamespaceURI)
$rayTelemetryInputs = $document.SelectSingleNode('/build:Project/build:PropertyGroup/build:RayTelemetryShaderInputs', $namespaces)
if (-not $rayTelemetryInputs -or [string]::IsNullOrWhiteSpace($rayTelemetryInputs.InnerText)) {
    throw 'Ray telemetry shader dependencies must be defined.'
}
foreach ($target in $shaderTargets) {
    if (@($target.Exec).Count -ne 1 -or $target.Outputs.Contains(';')) {
        throw "Shader compilation is not independently tracked: $($target.Name)"
    }
    if (-not $target.Inputs.Contains('$(MSBuildThisFileFullPath)')) {
        throw "Build settings are missing from dependencies: $($target.Name)"
    }
    if ($target.Exec.Command -notmatch ' -O3 ') {
        throw 'Release shader optimization must be preserved.'
    }
    $expandedInputs = $target.Inputs.Replace('$(RayTelemetryShaderInputs)', $rayTelemetryInputs.InnerText)
    foreach ($input in $expandedInputs.Split(';')) {
        $path = $input.Replace('$(MSBuildThisFileDirectory)', $root).Replace('$(MSBuildThisFileFullPath)', [IO.Path]::GetFullPath($TargetsPath))
        if (-not (Test-Path -LiteralPath $path)) {
            if ($path.Contains('External\NRD\sdk\') -and $target.Condition.Contains('$(EnableNrd)')) {
                continue
            }
            throw "Missing dependency: $path"
        }
    }
}
$culling = $shaderTargets | Where-Object Name -eq 'CompilePhase5ShadersClusterCullingCS'
if ($culling.Inputs.Contains('common.hlsl') -or $culling.Inputs.Contains('MaterialSurface.hlsli')) {
    throw 'Unrelated lighting dependencies would rebuild culling shaders.'
}
$opaque = $shaderTargets | Where-Object Name -eq 'CompilePhase5ShadersMeshletOpaquePS'
if (($shaderTargets | Where-Object Name -eq 'CompileRaytracingShaderRaytracingCS').Inputs.Contains('NRD.hlsli')) {
    throw 'The non-NRD shader must remain incremental when the optional NRD SDK is absent.'
}
if (-not $opaque.Inputs.Contains('MaterialSurface.hlsli')) {
    throw 'Opaque material dependency is missing.'
}
foreach ($group in @('CompileDlssHelperShaders', 'CompileRaytracingShader', 'CompileNrdRaytracingShaders', 'CompilePhase5Shaders')) {
    $target = $targets | Where-Object Name -eq $group
    foreach ($dependency in $target.DependsOnTargets.Split(';')) {
        if (-not ($targets | Where-Object Name -eq $dependency)) {
            throw "Missing child target: $dependency"
        }
    }
}
Write-Output 'Per-shader build dependency tests: PASS'
