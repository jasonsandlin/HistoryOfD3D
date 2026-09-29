# Builds the web edition of the book and commits it to the gh-pages branch of the
# public repo checkout, which GitHub Pages serves as-is (no GitHub Actions).
#
#   powershell -ExecutionPolicy Bypass -File .\tools\publish-site.ps1          # -> C:\git\HistoryOfD3D, branch gh-pages
#   powershell -ExecutionPolicy Bypass -File .\tools\publish-site.ps1 -Push    # ...and push it
#
# Afterwards the local book\ is rebuilt as the offline edition (with Run buttons).
# One-time GitHub setting: Settings > Pages > Source = "Deploy from a branch",
# branch gh-pages, folder / (root).
param([string]$Dest = "C:\git\HistoryOfD3D", [switch]$Push)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not (Test-Path (Join-Path $Dest ".git"))) { throw "$Dest is not a git checkout." }
$site = Join-Path $env:TEMP "HistoryOfD3D-gh-pages"

function Git { & git -C $Dest @args; if ($LASTEXITCODE -ne 0) { throw "git $args failed" } }

Write-Host "== Building the web edition"
& python (Join-Path $root "tools\build_book.py") --web
if ($LASTEXITCODE -ne 0) { throw "build_book.py --web failed" }

try {
    Write-Host "== Updating the gh-pages branch"
    & git -C $Dest worktree remove --force $site 2>$null
    Remove-Item $site -Recurse -Force -ErrorAction SilentlyContinue
    & git -C $Dest fetch -q origin gh-pages 2>$null
    & git -C $Dest show-ref -q --verify refs/heads/gh-pages
    $haveLocal = $LASTEXITCODE -eq 0
    & git -C $Dest show-ref -q --verify refs/remotes/origin/gh-pages
    $haveRemote = $LASTEXITCODE -eq 0
    if ($haveLocal) { Git worktree add -q $site gh-pages }
    elseif ($haveRemote) { Git worktree add -q -b gh-pages $site origin/gh-pages }
    else { Git worktree add -q --orphan -b gh-pages $site }

    Get-ChildItem $site -Force | Where-Object Name -ne ".git" | Remove-Item -Recurse -Force
    Copy-Item (Join-Path $root "book\*") $site -Recurse -Force
    New-Item -ItemType File -Force (Join-Path $site ".nojekyll") | Out-Null   # serve files as-is
    Set-Content (Join-Path $site ".gitattributes") "* -text`n"                  # byte-exact site files

    & git -C $site add -A
    & git -C $site diff --cached --quiet
    if ($LASTEXITCODE -eq 0) { Write-Host "   site unchanged" }
    else {
        & git -C $site commit -q -m "Publish site $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
        if ($LASTEXITCODE -ne 0) { throw "commit failed" }
        Write-Host "   committed to gh-pages"
    }
    if ($Push) {
        & git -C $site push -u origin gh-pages
        if ($LASTEXITCODE -ne 0) { throw "push failed" }
    }
}
finally {
    & git -C $Dest worktree remove --force $site 2>$null
    Write-Host "== Rebuilding the local offline edition"
    & python (Join-Path $root "tools\build_book.py") | Out-Null
}
Write-Host "Site: https://jasonsandlin.github.io/HistoryOfD3D/" -ForegroundColor Green
