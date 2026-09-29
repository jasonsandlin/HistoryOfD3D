# Mirrors the publishable part of this folder into a checkout of the public repo
# (github.com/jasonsandlin/HistoryOfD3D), ready to review and commit:
#
#   powershell -ExecutionPolicy Bypass -File .\tools\publish-repo.ps1                 # -> C:\git\HistoryOfD3D
#   powershell -ExecutionPolicy Bypass -File .\tools\publish-repo.ps1 -Dest D:\src\HistoryOfD3D
#
# Published: course text, sample sources + fetch/build scripts, tools, screenshots,
# and the narration (per-page audio, timings) - everything GitHub Actions needs to
# render the site. NOT published: build output (Out\, book\, dist\), the WAV
# narration cache, SDK downloads (third_party\ inside samples - fetch-deps.ps1 gets
# them), logs, and the whole-book m4b (a GitHub Release asset instead).
# Files in -Dest that are no longer published are deleted (.git is left alone).
param([string]$Dest = "C:\git\HistoryOfD3D")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not (Test-Path (Join-Path $Dest ".git"))) { throw "$Dest is not a git checkout." }

function Rel($p) { $p.Substring($root.Length + 1) }
function Publish($f) {
    $r = Rel $f.FullName
    if ($r -match '^(book|dist|audio\\work)(\\|$)') { return $false }
    if ($r -match '(^|\\)(Out|__pycache__|\.git)(\\|$)') { return $false }
    if ($r -match '^D3D\d+[^\\]*\\.*\\third_party\\' -or $r -match '^D3D\d+\.Basic\\third_party\\') { return $false }
    if ($r -match '^audio\\[^\\]+\.(m4b|srt)$') { return $false }
    if ($f.Name -match '\.(log|obj|pdb|ilk|exp|lib|exe|dll|partial\.\w+)$' -or $f.Name -eq "DirectStorageTexture.bin") { return $false }
    return $true
}
$files = Get-ChildItem -Path $root -Recurse -File -Force | Where-Object { Publish $_ }
$want = @{}
foreach ($f in $files) { $want[(Rel $f.FullName).ToLowerInvariant()] = $f }

# Remove what is no longer published.
$removed = 0
Get-ChildItem -Path $Dest -Recurse -File -Force | Where-Object { $_.FullName -notmatch '\\\.git(\\|$)' } | ForEach-Object {
    $r = $_.FullName.Substring($Dest.TrimEnd('\').Length + 1).ToLowerInvariant()
    if (-not $want.ContainsKey($r)) { Remove-Item $_.FullName -Force; $removed++ }
}
# Copy the rest (git decides what actually changed).
foreach ($f in $files) {
    $to = Join-Path $Dest (Rel $f.FullName)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
    Copy-Item $f.FullName $to -Force
}
Get-ChildItem -Path $Dest -Recurse -Directory -Force | Where-Object { $_.FullName -notmatch '\\\.git(\\|$)' } |
    Sort-Object { $_.FullName.Length } -Descending |
    Where-Object { -not (Get-ChildItem $_.FullName -Force) } | Remove-Item -Force

$mb = ($files | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("Mirrored {0} files ({1:N0} MB) into {2}; removed {3} stale file(s)." -f $files.Count, $mb, $Dest, $removed) -ForegroundColor Green
Write-Host "Review with: git -C `"$Dest`" status"
