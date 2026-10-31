param(
    [ValidateSet('Off', 'On')][string]$Mode = 'Off',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateRange(1, 1800)][int]$TimeoutSeconds = 900,
    [ValidateSet('None', 'AfterSource', 'AfterCso', 'Report')][string]$InjectFault = 'None'
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$resultsRoot = Join-Path $workspace 'bin/validation/technology'
New-Item -ItemType Directory -Force $resultsRoot | Out-Null

function Get-BytesHash([byte[]]$Bytes) {
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hash.ComputeHash($Bytes)).Replace('-', '') }
    finally { $hash.Dispose() }
}

function Set-AtomicBytes([string]$Path, [byte[]]$Bytes) {
    # The temporary file stays beside its verified workspace target, on the same volume.
    $temporary = $Path + '.aa-evaluation-' + [guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllBytes($temporary, $Bytes)
        [IO.File]::Replace($temporary, $Path, [NullString]::Value)
    }
    finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

function Restore-EvaluationFile([string]$Path, [byte[]]$Original, [string]$OriginalHash, [string]$CandidateHash) {
    $current = (Get-FileHash -LiteralPath $Path).Hash
    if ($current -eq $CandidateHash -and $current -ne $OriginalHash) {
        Set-AtomicBytes $Path $Original
    }
    elseif ($current -ne $OriginalHash) {
        throw "Preserved concurrent changes at $Path; original bytes are in the result directory."
    }
    if ((Get-FileHash -LiteralPath $Path).Hash -ne $OriginalHash) {
        throw "Restoration hash mismatch: $Path"
    }
}

$names = @('DX12_FEATURE_SMOKE_TEST', 'DX12_VALIDATION_MODE', 'DX12_STARTUP_SMOKE_FRAMES',
    'DX12_GEOMETRIC_AA_VALIDATION', 'DX12_GEOMETRIC_AA_EXPECTED', 'DX12_GEOMETRIC_AA_OUTPUT',
    'DX12_DISABLE_DXR', 'DX12_DISABLE_DLSS', 'DX12_KEEP_SHADER_REFLECTION', 'DX12_GPU_BASED_VALIDATION')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$evaluationLock = $null
$locationPushed = $false
$failure = $null
$cleanupErrors = @()
$output = $null
try {
    $evaluationLock = [IO.File]::Open((Join-Path $resultsRoot 'geometric-aa-evaluation.lock'),
        [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    Push-Location -LiteralPath $workspace
    $locationPushed = $true
    $output = Join-Path $resultsRoot ('aa-engine-{0}-{1}-{2}' -f $Mode.ToLowerInvariant(),
        (Get-Date -Format 'yyyyMMdd-HHmmss'), ([guid]::NewGuid().ToString('N').Substring(0, 8)))
    New-Item -ItemType Directory -Force $output | Out-Null
    $source = Join-Path $workspace 'shader/hlsl/DeferredPS.hlsl'
    $cso = Join-Path $workspace 'shader/hlsl/build/DeferredPS.cso'
    $executable = Join-Path $workspace "bin/x64/$Configuration/DirectX12.exe"
    $sourceBefore = [IO.File]::ReadAllBytes($source)
    $csoBefore = [IO.File]::ReadAllBytes($cso)
    $sourceText = [Text.Encoding]::UTF8.GetString($sourceBefore)
    $definition = [regex]::Matches($sourceText, '(?m)^#define GEOMETRIC_SPECULAR_AA 0\r?$')
    if ($definition.Count -ne 1) {
        throw 'Expected one default-OFF AA definition; preserve existing source and review before evaluation.'
    }
    if (-not (Test-Path -LiteralPath $executable)) { throw 'Build the renderer first.' }
    $expected = [int]($Mode -eq 'On')
    $candidateText = $sourceText
    if ($expected) {
        $candidateText = $sourceText.Replace($definition[0].Value, $definition[0].Value.Replace(' 0', ' 1'))
    }
    $candidateSource = [Text.Encoding]::UTF8.GetBytes($candidateText)
    [IO.File]::WriteAllBytes((Join-Path $output 'shader-default-before.hlsl'), $sourceBefore)
    [IO.File]::WriteAllBytes((Join-Path $output 'DeferredPS-before.cso'), $csoBefore)
    [IO.File]::WriteAllBytes((Join-Path $output 'shader-default-evaluation.hlsl'), $candidateSource)
    [xml]$packages = Get-Content packages.config
    $version = ($packages.packages.package | Where-Object id -eq 'Microsoft.Direct3D.DXC').version
    $compiler = Join-Path $workspace "packages/Microsoft.Direct3D.DXC.$version/build/native/bin/x64/dxc.exe"
    $candidateCsoPath = Join-Path $output 'DeferredPS-evaluation.cso'
    & $compiler -T ps_6_0 -E main -O3 -D "GEOMETRIC_SPECULAR_AA=$expected" -Fo $candidateCsoPath $source
    if ($LASTEXITCODE) { throw 'Evaluation material shader compilation failed.' }
    $candidateCso = [IO.File]::ReadAllBytes($candidateCsoPath)
    $metadata = [ordered]@{
        Mode = $Mode; Configuration = $Configuration; StartedUtc = [DateTime]::UtcNow.ToString('o')
        Output = $output; SourceBeforeSha256 = (Get-BytesHash $sourceBefore)
        CsoBeforeSha256 = (Get-BytesHash $csoBefore); CandidateSourceSha256 = (Get-BytesHash $candidateSource)
        CandidateCsoSha256 = (Get-BytesHash $candidateCso); InjectFault = $InjectFault
        SourceRestored = $false; CsoRestored = $false; Succeeded = $false; ExitCode = $null
        Scope = 'CPU Deferred PBR fixture; material readback checks AA mode. Static timings precede readbacks.'
    }
    $metadata | ConvertTo-Json | Set-Content (Join-Path $output 'run.json') -Encoding utf8
    $process = $null
    try {
        $env:DX12_FEATURE_SMOKE_TEST = '1'
        $env:DX12_VALIDATION_MODE = '1'
        $env:DX12_STARTUP_SMOKE_FRAMES = '116'
        $env:DX12_GEOMETRIC_AA_VALIDATION = '1'
        $env:DX12_GEOMETRIC_AA_EXPECTED = [string]$expected
        $env:DX12_GEOMETRIC_AA_OUTPUT = $output
        $env:DX12_DISABLE_DXR = '1'
        $env:DX12_DISABLE_DLSS = '1'
        $env:DX12_KEEP_SHADER_REFLECTION = '1'
        $env:DX12_GPU_BASED_VALIDATION = '0'
        # Atomic replacement leaves the destination entirely old or entirely new.
        # Check both destinations before mutation; cleanup checks both even if replacement fails.
        if ((Get-FileHash -LiteralPath $source).Hash -ne $metadata.SourceBeforeSha256 -or
            (Get-FileHash -LiteralPath $cso).Hash -ne $metadata.CsoBeforeSha256) {
            throw 'Shader files changed during preflight; preserving concurrent changes.'
        }
        if ($expected) { Set-AtomicBytes $source $candidateSource }
        if ($InjectFault -eq 'AfterSource') { throw 'Injected evaluation failure after source replacement' }
        Set-AtomicBytes $cso $candidateCso
        if ($InjectFault -in @('AfterCso', 'Report')) { throw 'Injected evaluation failure after CSO replacement' }
        $process = Start-Process -FilePath $executable -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
        Write-Output "AA $Mode renderer process $($process.Id), output $output"
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "AA $Mode renderer exceeded $TimeoutSeconds seconds."
        }
        $process.Refresh()
        $metadata.ExitCode = $process.ExitCode
        $log = Get-Content -LiteralPath (Join-Path $workspace 'init_log.txt') -Raw
        Set-Content -LiteralPath (Join-Path $output 'init_log.txt') -Value $log -Encoding utf8
        $marker = "Geometric AA fixture: expected=$expected output=$output"
        $start = $log.LastIndexOf($marker, [StringComparison]::Ordinal)
        if ($start -lt 0 -or $process.ExitCode -ne 0 -or
            $log.Substring($start) -notmatch 'Geometric AA validation failures = 0;') {
            throw "AA $Mode validation failed; see $output/init_log.txt"
        }
        $metadata.Succeeded = $true
    }
    catch {
        $failure = $_
        if ($process -and -not $process.HasExited) {
            try { $process.Kill(); $process.WaitForExit() }
            catch { $cleanupErrors += "Renderer termination failed: $($_.Exception.Message)" }
        }
    }
    finally {
        # Guard restoration independently so one failed operation cannot prevent the other.
        try {
            Restore-EvaluationFile $source $sourceBefore $metadata.SourceBeforeSha256 $metadata.CandidateSourceSha256
            $metadata.SourceRestored = $true
        }
        catch { $cleanupErrors += "Source restoration: $($_.Exception.Message)" }
        try {
            Restore-EvaluationFile $cso $csoBefore $metadata.CsoBeforeSha256 $metadata.CandidateCsoSha256
            $metadata.CsoRestored = $true
        }
        catch { $cleanupErrors += "CSO restoration: $($_.Exception.Message)" }
        if ($cleanupErrors.Count) { $metadata.Succeeded = $false }
        $metadata['CompletedUtc'] = [DateTime]::UtcNow.ToString('o')
        $metadata['CleanupErrors'] = $cleanupErrors
        try {
            if ($InjectFault -eq 'Report') { throw 'Injected metadata reporting failure' }
            $metadata | ConvertTo-Json | Set-Content (Join-Path $output 'run.json') -Encoding utf8
        }
        catch { $cleanupErrors += "Metadata reporting: $($_.Exception.Message)" }
    }
}
finally {
    # Environment, current directory and lock cleanup also run after preflight/restoration/reporting failures.
    try {
        foreach ($name in $names) {
            try {
                if ($null -eq $previous[$name]) {
                    [Environment]::SetEnvironmentVariable($name, [NullString]::Value, 'Process')
                }
                else { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
            }
            catch { $cleanupErrors += "Environment restoration ($name): $($_.Exception.Message)" }
        }
    }
    finally {
        try { if ($locationPushed) { Pop-Location } }
        finally { if ($evaluationLock) { $evaluationLock.Dispose() } }
    }
}
if ($cleanupErrors.Count) { throw ($cleanupErrors -join '; ') }
if ($failure) { throw $failure }
Write-Output ('AA {0}: PASS; default source and CSO restored. Results: {1}' -f $Mode, $output)
