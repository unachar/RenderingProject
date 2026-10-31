# Remove all upstream folders from git index
$folders = @(
    "External/ACL/upstream",
    "External/KTX-Software/upstream",
    "External/basis_universal/upstream",
    "External/fastgltf/upstream",
    "External/STBN/upstream",
    "External/RTXDI/upstream",
    "External/D3D12MemoryAllocator/upstream",
    "External/tracy/upstream",
    "External/DirectStorage/upstream",
    "External/Draco/upstream"
)

foreach ($folder in $folders) {
    if (Test-Path $folder) {
        git rm -r --cached $folder 2>$null
        Write-Host "Removed $folder from index"
    }
}

# Remove tar.gz files
$tars = Get-ChildItem *.tar.gz, *.zip -ErrorAction SilentlyContinue
foreach ($tar in $tars) {
    git rm --cached $tar.Name 2>$null
    Write-Host "Removed $($tar.Name) from index"
}

git status --short