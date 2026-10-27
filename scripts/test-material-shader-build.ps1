$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$namespace = @{ b = 'http://schemas.microsoft.com/developer/msbuild/2003' }
[xml]$project = Get-Content -LiteralPath (Join-Path $root 'DirectX12.vcxproj')
[xml]$materials = Get-Content -LiteralPath (Join-Path $root 'MaterialShaders.targets')
[xml]$custom = Get-Content -LiteralPath (Join-Path $root 'Directory.Build.targets')

function Get-ShaderDependencies([string]$path, $visited) {
    $path = [IO.Path]::GetFullPath($path)
    if (-not $visited.Add($path)) { return }
    $source = [IO.File]::ReadAllText($path)
    foreach ($match in [regex]::Matches($source, '(?m)^\s*#include\s+"([^"]+)"')) {
        $include = Join-Path (Split-Path $path) $match.Groups[1].Value
        if (Test-Path -LiteralPath $include) {
            Get-ShaderDependencies $include $visited
        }
    }
}

$targets = @(Select-Xml -Xml $materials -Namespace $namespace -XPath '//b:Target[@Outputs]' | ForEach-Object Node)
if ($targets.Count -ne 9) { throw 'Expected nine material and lighting shader entry points.' }
foreach ($target in $targets) {
    $command = [string]$target.Exec.Command
    if ($command -notmatch 'dxc.exe' -or $command -notmatch ' -O3 ' -or $command -notmatch ' -T ps_6_0 ') {
        throw "Material optimization or compiler changed: $($target.Name)"
    }
    $inputs = @($target.Inputs.Split(';') | ForEach-Object {
        $_.Replace('$(MSBuildThisFileDirectory)', "$root\").Replace('$(MSBuildThisFileFullPath)', (Join-Path $root 'MaterialShaders.targets'))
    })
    # The command identifies the entry point; includes can contain another .hlsl entry point.
    $sourceMatch = [regex]::Match($command, 'shader\\hlsl\\([^"\s]+\.hlsl)"$')
    if (-not $sourceMatch.Success) { throw "Missing shader source: $($target.Name)" }
    $source = Join-Path $root "shader/hlsl/$($sourceMatch.Groups[1].Value)"
    $dependencies = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    Get-ShaderDependencies $source $dependencies
    foreach ($dependency in $dependencies) {
        if ($dependency -notin $inputs) { throw "Untracked shader include: $dependency" }
    }
    $relative = $source.Substring($root.Length + 1)
    $legacy = Select-Xml -Xml $project -Namespace $namespace -XPath "//b:FxCompile[@Include='$relative']"
    if (-not $legacy -or $legacy.Node.ExcludedFromBuild.'#text' -ne 'true') {
        throw "FXC would still compile $relative"
    }
}

foreach ($entry in (Select-Xml -Xml $project -Namespace $namespace -XPath '//b:FxCompile[@Include]')) {
    $dependencies = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    Get-ShaderDependencies (Join-Path $root $entry.Node.Include) $dependencies
    if ((Join-Path $root 'shader/hlsl/MaterialSurface.hlsli') -in $dependencies) {
        if ($entry.Node.ExcludedFromBuild.'#text' -ne 'true') {
            throw "A material entry point still uses FXC: $($entry.Node.Include)"
        }
    }
}

foreach ($document in @($materials, $custom)) {
    $writes = @(Select-Xml -Xml $document -Namespace $namespace -XPath '//b:FileWrites' | ForEach-Object { $_.Node.Include.Split(';') })
    foreach ($target in (Select-Xml -Xml $document -Namespace $namespace -XPath '//b:Target[@Outputs]')) {
        foreach ($output in $target.Node.Outputs.Split(';')) {
            if ($output -notin $writes) { throw "Rebuild would reuse a custom shader: $output" }
        }
    }
}
Write-Output 'Material shader compiler, transitive dependencies and full rebuild coverage: PASS'

[xml]$parallel = Get-Content -LiteralPath (Join-Path $root 'ParallelBuild.targets')
$expectedJobs = @($targets.Name) + @(Select-Xml -Xml $custom -Namespace $namespace -XPath '//b:Target[@Outputs]' | ForEach-Object { $_.Node.Name })
$jobs = @(Select-Xml -Xml $parallel -Namespace $namespace -XPath '//b:_ParallelShaderProject' | ForEach-Object {
    $_.Node.AdditionalProperties.Replace('_RequestedShaderTarget=', '')
})
if (@($jobs | Group-Object | Where-Object Count -gt 1).Count -or @(Compare-Object $expectedJobs $jobs).Count) {
    throw 'The parallel dispatch must cover every custom shader target exactly once.'
}
$dispatch = (Select-Xml -Xml $parallel -Namespace $namespace -XPath '//b:Target[@Name="CompileProjectShadersInParallel"]').Node
if ($dispatch.MSBuild.BuildInParallel -ne 'true' -or @($expectedJobs | Where-Object { $_ -notin $dispatch.BeforeTargets.Split(';') }).Count) {
    throw 'Parallel compilation must run before the original shader targets.'
}
$allOutputs = @()
foreach ($document in @($materials, $custom)) {
    $allOutputs += @(Select-Xml -Xml $document -Namespace $namespace -XPath '//b:Target[@Outputs]' | ForEach-Object { $_.Node.Outputs.Split(';') })
}
if (@($allOutputs | Group-Object | Where-Object Count -gt 1).Count) {
    throw 'Parallel shader targets must not write to the same output.'
}
$clean = (Select-Xml -Xml $parallel -Namespace $namespace -XPath '//b:Target[@Name="CleanCustomShaderOutputs"]').Node
if ($clean.BeforeTargets -ne 'CoreClean' -or -not $clean.Delete.Files) {
    throw 'Custom shader outputs outside IntDir/OutDir require an explicit Clean hook.'
}
Write-Output 'Parallel shader coverage and independent output paths: PASS'
