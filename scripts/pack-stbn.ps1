# Pack the uniform Float2 slices from the vendored NVIDIA STBN archive as linear RG8.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Compression.FileSystem
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$staging = Join-Path $workspace 'build/stbn-assets'
New-Item -ItemType Directory -Force $staging | Out-Null
& tar -xf (Join-Path $workspace 'stbn.tar.gz') -C $staging STBN-main/Assets/STBN.zip
if ($LASTEXITCODE) {
    throw 'Could not extract NVIDIA STBN archive.'
}
$zipPath = Join-Path $staging 'STBN-main/Assets/STBN.zip'
$expectedHash = 'f262aaa79704b913ad1ac22b11674931c5c16a788688f8ce49ec43d59eb5c747'
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash -ne $expectedHash) {
    throw 'The STBN archive differs from External/STBN/manifest.json.'
}
$archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
$packed = [byte[]]::new(128 * 128 * 64 * 2)
$index = 0
try {
    for ($frame = 0; $frame -lt 64; ++$frame) {
        $entry = $archive.GetEntry("STBN/stbn_vec2_2Dx1D_128x128x64_$frame.png")
        $stream = $entry.Open()
        $image = [Drawing.Bitmap]::new($stream)
        try {
            if ($image.Width -ne 128 -or $image.Height -ne 128) {
                throw 'Invalid STBN extent.'
            }
            for ($y = 0; $y -lt 128; ++$y) {
                for ($x = 0; $x -lt 128; ++$x) {
                    $pixel = $image.GetPixel($x, $y)
                    $packed[$index++] = $pixel.R
                    $packed[$index++] = $pixel.G
                }
            }
        } finally {
            $image.Dispose()
            $stream.Dispose()
        }
    }
} finally {
    $archive.Dispose()
}
$destination = Join-Path $workspace 'asset/Noise/stbn-vec2-128x128x64.rg8'
New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
[IO.File]::WriteAllBytes($destination, $packed)
Get-FileHash -Algorithm SHA256 -LiteralPath $destination
