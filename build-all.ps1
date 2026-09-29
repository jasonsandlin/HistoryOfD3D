# Builds each RotatingCubes/<ver>/main.cpp with MSVC, invoking cl.exe directly.
# Imports the VC x64 environment into this session first. Pass -Only <name> to
# build a single subfolder (e.g. -Only D3D11). D3D8 uses vendored headers (/I.).
param([string]$Only = "")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$tmp = New-TemporaryFile
cmd /c "call `"$vcvars`" >nul 2>&1 && set" > $tmp
Get-Content $tmp | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') { Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue }
}
Remove-Item $tmp
Remove-Item Env:CL, Env:_CL_, Env:LINK, Env:_LINK_ -ErrorAction SilentlyContinue
$cl = (Get-Command cl.exe | Select-Object -First 1).Source

# Auto-discover every subfolder (at ANY depth) that has a main.cpp or main.c:
# base cubes and flat shader showcases at the root, plus the tiered D3D12
# learning samples nested under D3D12\Tier*\. Recurse so nested samples are
# found (Out\ and third_party\ dirs have no main.* and are skipped).
$projectDirs = Get-ChildItem -Path $root -Directory -Recurse |
    Where-Object { (Test-Path (Join-Path $_.FullName "main.cpp")) -or (Test-Path (Join-Path $_.FullName "main.c")) } |
    Sort-Object FullName
if ($Only) { $projectDirs = @($projectDirs | Where-Object { $_.Name -eq $Only }) }

$results = @()
foreach ($proj in $projectDirs) {
    $p = $proj.Name
    $dir = $proj.FullName
    $isC = $false
    $src = "main.cpp"
    if (Test-Path (Join-Path $dir "main.cpp")) { $src = "main.cpp" }
    elseif (Test-Path (Join-Path $dir "main.c")) { $src = "main.c"; $isC = $true }
    else { continue }
    Push-Location $dir
    Write-Host "==================== BUILD $p ===================="
    # Build products (exe + intermediates) go in a per-sample Out\ folder so
    # they never clutter the source tree (Out\ is .gitignored).
    New-Item -ItemType Directory -Force -Path "Out" | Out-Null
    $exe = "Out\Cube$p.exe"
    Remove-Item $exe -ErrorAction SilentlyContinue
    if ($isC) {
        # C sources (e.g. Nuklear backend uses C-style COM) compile as C via
        # per-file /Tc so that libs on the command line aren't treated as C.
        $args = @("/nologo", "/W3", "/Zi", "/Fe:$exe", "/FoOut\", "/FdOut\")
    } else {
        $args = @("/nologo", "/EHsc", "/W3", "/Zi", "/Fe:$exe", "/FoOut\", "/FdOut\")
    }
    if ($p -like "D3D8*") { $args += "/I." }   # vendored d3d8 headers
    # Preview (Agility SDK) samples vendor their own headers under third_party\;
    # put them first so <d3d12.h> / d3dx12 resolve to the preview versions.
    $agilityInc = Join-Path $dir "third_party\agility\include"
    if (Test-Path $agilityInc) { $args += ("/I" + $agilityInc) }
    # Let nested samples find shared headers via a root-relative path, e.g.
    # #include "third_party/nuklear/nuklear.h" resolves from any tier depth.
    $args += ("/I" + $root)
    if ($isC) { $args += @("/Tc$src", "user32.lib") }
    else { $args += @($src, "user32.lib") }
    & $cl @args
    if (Test-Path $exe) {
        # Stage vendored preview runtimes next to the exe so it runs straight
        # from Out\: Agility SDK -> Out\D3D12\, preview DXC + WARP -> Out\.
        $tp = Join-Path $dir "third_party"
        if (Test-Path (Join-Path $tp "agility\bin\x64\D3D12Core.dll")) {
            New-Item -ItemType Directory -Force -Path "Out\D3D12" | Out-Null
            Copy-Item (Join-Path $tp "agility\bin\x64\*.dll") "Out\D3D12" -Force
        }
        foreach ($d in @("dxc\bin\x64", "warp\bin\x64")) {
            if (Test-Path (Join-Path $tp $d)) { Copy-Item (Join-Path $tp "$d\*.dll") "Out" -Force }
        }
        $len = (Get-Item $exe).Length
        Write-Host "RESULT ${p}: OK ($len bytes)" -ForegroundColor Green
        $results += [pscustomobject]@{ Project = $p; Built = $true; Bytes = $len }
    } else {
        Write-Host "RESULT ${p}: FAILED" -ForegroundColor Red
        $results += [pscustomobject]@{ Project = $p; Built = $false; Bytes = 0 }
    }
    Pop-Location
}
Write-Host "`n===== SUMMARY ====="
$results | Format-Table -AutoSize
