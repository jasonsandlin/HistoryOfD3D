# Packs "The History of Direct3D" into one shareable zip:
#   dist\HistoryOfD3D-<date>.zip  ->  HistoryOfD3D\ (extract anywhere)
#
#   powershell -ExecutionPolicy Bypass -File .\tools\pack-share.ps1
#   powershell -ExecutionPolicy Bypass -File .\tools\pack-share.ps1 -Build   # rebuild samples first
#
# Contents: the interactive book (incl. the read-along m4b), the course text, every
# sample's source AND its prebuilt exe + runtime DLLs (so readers can press "Run
# sample" without Visual Studio), setup.ps1 and the tools to rebuild everything.
# Left out: build intermediates (obj/pdb/ilk), logs, the narration WAV cache
# (audio\work), and the preview SDK downloads (each sample's fetch-deps.ps1 gets
# them again if someone wants to rebuild).
#
# Readers: extract, open "START HERE.html". To enable the Run buttons, run
#   powershell -ExecutionPolicy Bypass -File .\setup.ps1
param(
    [switch]$Build,
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$stamp = Get-Date -Format "yyyy-MM-dd"
if (-not $Output) { $Output = Join-Path $root "dist\HistoryOfD3D-$stamp.zip" }

if ($Build) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "build-all.ps1")
    . (Join-Path $root "tools\legacy32.ps1")
    foreach ($n in "D3D7.Basic", "D3D8.Basic") { [void](Build-Legacy32 (Get-Item (Join-Path $root $n))) }
}

# ---- sanity checks: everything a reader needs must exist -------------------
$samples = Get-ChildItem -Path $root -Directory -Recurse |
    Where-Object { $_.FullName -notmatch '\\(third_party|book|screenshots|audio|tools|docs|dist|Out)(\\|$)' } |
    Where-Object { (Test-Path (Join-Path $_.FullName "main.cpp")) -or (Test-Path (Join-Path $_.FullName "main.c")) }
$missing = @($samples | Where-Object { -not (Test-Path (Join-Path $_.FullName ("Out\Cube" + $_.Name + ".exe"))) } | ForEach-Object Name)
foreach ($n in "D3D7.Basic", "D3D8.Basic") {
    if (-not (Test-Path (Join-Path $root "$n\Out\x86\Cube$n.exe"))) { $missing += "$n (x86)" }
}
if ($missing) { throw "Samples not built: $($missing -join ', '). Re-run with -Build." }
# Always pack the offline edition (Run buttons, local m4b) - book\ may hold a --web build.
& python (Join-Path $root "tools\build_book.py") | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build_book.py failed" }
if (-not (Test-Path (Join-Path $root "audio\TheHistoryOfDirect3D.m4b"))) {
    Write-Host "WARN: no audiobook (audio\TheHistoryOfDirect3D.m4b) - run tools\narrate_book.py." -ForegroundColor Yellow
}

# ---- choose files ---------------------------------------------------------
function Rel($p) { $p.Substring($root.Length + 1) }
function Include($f) {
    $r = Rel $f.FullName
    # audio\*.m4b/.srt: the book already carries a copy in book\assets\audio.
    if ($r -match '^audio\\[^\\]+\.(m4b|srt)$') { return $false }
    if ($r -match '^(dist|audio\\work)(\\|$)' -or $r -match '(^|\\)(__pycache__|\.git)(\\|$)') { return $false }
    if ($f.Name -match '\.(log|obj|pdb|ilk|exp|lib|partial\.\w+)$' -or $f.Name -eq "DirectStorageTexture.bin") { return $false }
    if ($r -match '\\Out\\') {
        # Only what the sample needs to run: its exe, DLLs beside it, the Agility
        # SDK runtime in Out\D3D12\, and the 32-bit legacy builds in Out\x86\.
        $sample = ($r -split '\\Out\\')[0].Split('\')[-1]
        $inOut = ($r -split '\\Out\\', 2)[1]
        return ($inOut -eq "Cube$sample.exe") -or ($inOut -match '^[^\\]+\.dll$') -or
               ($inOut -match '^D3D12\\[^\\]+\.dll$') -or ($inOut -eq "x86\Cube$sample.exe")
    }
    # Sample-local third_party\ holds preview SDK downloads (re-fetched by
    # fetch-deps.ps1). DirectStorage has no fetch script, so its copy ships.
    if ($r -match '^D3D12\\.*\\third_party\\' -and $r -notmatch 'D3D12\.DirectStorage\\third_party\\') { return $false }
    return $true
}
$files = Get-ChildItem -Path $root -Recurse -File -Force | Where-Object { Include $_ }

# ---- write the zip --------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null
if (Test-Path $Output) { Remove-Item $Output -Force }
$tmp = "$Output.partial"
if (Test-Path $tmp) { Remove-Item $tmp -Force }
$zip = [System.IO.Compression.ZipFile]::Open($tmp, [System.IO.Compression.ZipArchiveMode]::Create)
$raw = 0
try {
    $i = 0
    foreach ($f in $files) {
        $i++
        if ($i % 100 -eq 0) { Write-Progress -Activity "Packing" -Status (Rel $f.FullName) -PercentComplete ($i * 100 / $files.Count) }
        # Audio/images are already compressed; storing them is much faster.
        $level = if ($f.Extension -in ".m4b", ".m4a", ".jpg", ".png", ".zip") { "NoCompression" } else { "Optimal" }
        $entry = "HistoryOfD3D/" + ((Rel $f.FullName) -replace '\\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, $entry, $level)
        $raw += $f.Length
    }
    $start = $zip.CreateEntry("HistoryOfD3D/START HERE.html")
    $w = New-Object System.IO.StreamWriter($start.Open(), (New-Object System.Text.UTF8Encoding $false))
    $w.Write(@"
<!DOCTYPE html><html><head><meta charset="utf-8"><meta http-equiv="refresh" content="0; url=book/index.html">
<title>The History of Direct3D</title></head><body style="background:#080b08;color:#e6ede6;font:16px Segoe UI,sans-serif;padding:40px">
<p>Opening <a style="color:#9BF00B" href="book/index.html">The History of Direct3D</a>&hellip;</p>
<p style="color:#98a898">To use the <b>Run sample</b> buttons, run this once in this folder (no Visual Studio needed, samples are prebuilt):<br>
<code>powershell -ExecutionPolicy Bypass -File .\setup.ps1</code></p></body></html>
"@)
    $w.Dispose()
}
finally { $zip.Dispose() }
Move-Item $tmp $Output -Force
Write-Progress -Activity "Packing" -Completed

$zipMB = (Get-Item $Output).Length / 1MB
Write-Host ("Packed {0:N0} files ({1:N0} MB) -> {2}  ({3:N0} MB)" -f $files.Count, ($raw / 1MB), $Output, $zipMB) -ForegroundColor Green
Write-Host "Includes $($samples.Count) prebuilt samples. Readers: extract, open 'START HERE.html', run setup.ps1 for the Run buttons."
