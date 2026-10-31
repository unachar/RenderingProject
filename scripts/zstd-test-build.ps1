function Get-ZstdTestObject([string] $DeveloperCommand) {
    $root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $source = Join-Path $root 'External/zstd/zstd.c'
    $object = Join-Path $root 'bin/validation/technology/zstd-mt.obj'
    New-Item -ItemType Directory -Path (Split-Path $object) -Force | Out-Null
    $dependencies = @($source, (Join-Path $root 'External/zstd/zstd.h'))
    $rebuild = -not (Test-Path -LiteralPath $object)
    if (-not $rebuild) {
        foreach ($dependency in $dependencies) {
            if ((Get-Item $dependency).LastWriteTimeUtc -gt (Get-Item $object).LastWriteTimeUtc) { $rebuild = $true }
        }
    }
    if ($rebuild) {
        $command = 'cl.exe /nologo /TC /std:c17 /O2 /MT /Z7 /W0 /c "{0}" /Fo:"{1}"' -f $source, $object
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $DeveloperCommand, $command) | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'Static zstd test compilation failed.' }
    }
    return $object
}

function Get-ZstdAllocationTestObjects([string] $DeveloperCommand) {
    $root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $source = Join-Path $root 'External/zstd/zstd.c'
    $shim = Join-Path $root 'tests/zstd_allocator_probe.h'
    $allocator = Join-Path $root 'tests/zstd_allocator_probe.c'
    $object = Join-Path $root 'bin/validation/technology/zstd-allocation-test.obj'
    $allocatorObject = Join-Path $root 'bin/validation/technology/zstd-allocator-probe.obj'
    New-Item -ItemType Directory -Path (Split-Path $object) -Force | Out-Null
    $rebuild = -not (Test-Path $object) -or -not (Test-Path $allocatorObject)
    if (-not $rebuild) {
        foreach ($dependency in @($source, $shim, $allocator, (Join-Path $root 'External/zstd/zstd.h'))) {
            if ((Get-Item $dependency).LastWriteTimeUtc -gt (Get-Item $object).LastWriteTimeUtc) { $rebuild = $true }
        }
    }
    if ($rebuild) {
        $command = 'cl /nologo /TC /std:c17 /O2 /MT /Z7 /W0 /DTEXTURE_ZSTD_ALLOCATOR_SHIM /FI:"{0}" /c "{1}" /Fo:"{2}"' -f $shim, $source, $object
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $DeveloperCommand, $command) | Out-Host
        if ($LASTEXITCODE) { throw 'zstd allocator-injection build failed.' }
        $command = 'cl /nologo /TC /std:c17 /O2 /MT /Z7 /W4 /c "{0}" /Fo:"{1}"' -f $allocator, $allocatorObject
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $DeveloperCommand, $command) | Out-Host
        if ($LASTEXITCODE) { throw 'zstd allocator shim build failed.' }
    }
    return @($object, $allocatorObject)
}
