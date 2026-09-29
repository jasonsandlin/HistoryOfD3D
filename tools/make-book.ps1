# One command to regenerate the interactive HTML book (book\index.html):
#   1. build every sample (build-all.ps1)          skip with -NoBuild
#   2. capture screenshots + animation strips       skip with -NoCapture
#   3. (optional, -Narrate) re-narrate changed pages into the read-along m4b
#   4. render docs\ + samples + screenshots\ -> book\
#
#   powershell -ExecutionPolicy Bypass -File .\tools\make-book.ps1
#   powershell -ExecutionPolicy Bypass -File .\tools\make-book.ps1 -NoBuild -Only D3D12.RayTracing12
#
# Preview samples (D3D12.RayTracing12 / LinearAlgebra / PartialPrograms) need their
# third_party\ deps first: run each sample's fetch-deps.ps1 once (-FetchDeps does it).
# Needs Python with: pip install markdown pygments pillow
param(
    [string]$Only = "",
    [switch]$NoBuild,
    [switch]$NoCapture,
    [switch]$FetchDeps,
    [switch]$Narrate
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $root
try {
    if ($FetchDeps) {
        Get-ChildItem -Path (Join-Path $root "D3D12") -Recurse -Filter fetch-deps.ps1 |
            Where-Object { -not $Only -or $_.Directory.Name -eq $Only } |
            ForEach-Object { Write-Host "== fetch-deps $($_.Directory.Name)"; & powershell -ExecutionPolicy Bypass -File $_.FullName }
    }
    $sel = @(); if ($Only) { $sel = @("-Only", $Only) }
    if (-not $NoBuild) { & powershell -ExecutionPolicy Bypass -File (Join-Path $root "build-all.ps1") @sel }
    if (-not $NoCapture) { & powershell -ExecutionPolicy Bypass -File (Join-Path $root "tools\capture-screenshots.ps1") @sel }
    # narrate_book.py renders the book itself (before and after narrating).
    if ($Narrate) { & python (Join-Path $root "tools\narrate_book.py") }
    else { & python (Join-Path $root "tools\build_book.py") }
    if ($LASTEXITCODE -ne 0) { throw "building the book failed" }
    Write-Host "`nOpen $root\book\index.html" -ForegroundColor Green
} finally {
    Pop-Location
}
