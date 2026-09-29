# Launches one sample from the HTML book's "Run sample" button.
# Invoked by the per-user d3dbook: URL protocol (see register-run-handler.ps1)
# with the raw URL, e.g. "d3dbook:run/D3D10.Basic", or directly:
#   powershell -ExecutionPolicy Bypass -File .\tools\run-sample.ps1 D3D10.Basic
#
# Safety: the URL comes from a web page, so it is only ever used to look up a
# sample FOLDER NAME inside this repo (strict charset, must contain main.cpp /
# main.c). Nothing from the URL is passed to a shell or used as a path.
param([Parameter(Mandatory = $true)][string]$Target)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Show-Error([string]$msg) {
    Add-Type -AssemblyName PresentationFramework
    [void][System.Windows.MessageBox]::Show($msg, "History of Direct3D - Run sample", "OK", "Warning")
}

try {
    $name = $Target -replace '^d3dbook:(//)?', '' -replace '^run/', '' -replace '/+$', ''
    $name = [Uri]::UnescapeDataString($name)
    if ($name -notmatch '^[A-Za-z0-9]+(\.[A-Za-z0-9]+)*$') { throw "Not a sample name: '$name'" }

    $dir = Get-ChildItem -Path $root -Directory -Recurse -Filter $name |
        Where-Object { $_.FullName -notmatch '\\(third_party|book|screenshots|Out)(\\|$)' } |
        Where-Object { (Test-Path (Join-Path $_.FullName "main.cpp")) -or (Test-Path (Join-Path $_.FullName "main.c")) } |
        Select-Object -First 1
    if (-not $dir) { throw "No sample folder named '$name' under $root" }

    $exe = Join-Path $dir.FullName "Out\Cube$name.exe"
    # D3D7/D3D8 only run as 32-bit on modern Windows (legacy runtimes live in SysWOW64).
    $x86 = Join-Path $dir.FullName "Out\x86\Cube$name.exe"
    if (Test-Path $x86) { $exe = $x86 }

    if (-not (Test-Path $exe)) {
        # Not built yet: fetch preview deps if the sample needs them, then build,
        # in a visible console so the presenter can see what's happening.
        $steps = @()
        $fetch = Join-Path $dir.FullName "fetch-deps.ps1"
        if ((Test-Path $fetch) -and -not (Test-Path (Join-Path $dir.FullName "third_party"))) { $steps += "& '$fetch'" }
        $steps += "& '$(Join-Path $root "build-all.ps1")' -Only '$name'"
        $cmd = ($steps -join "; ") + "; Start-Sleep -Seconds 2"
        Start-Process powershell -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", $cmd) -Wait
        if (-not (Test-Path $exe)) { throw "Build failed for $name - run .\build-all.ps1 -Only $name to see the errors." }
    }
    if (-not (Test-Path $x86) -and $name -in "D3D7.Basic", "D3D8.Basic") {
        . (Join-Path $PSScriptRoot "legacy32.ps1")
        $built = Build-Legacy32 $dir
        if ($built) { $exe = $built }
    }

    # Run from the exe's folder: preview samples load .\D3D12\, dxcompiler.dll and
    # d3d10warp.dll from there, and every sample writes its .log next to it.
    Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe)
} catch {
    Show-Error $_.Exception.Message
    exit 1
}
