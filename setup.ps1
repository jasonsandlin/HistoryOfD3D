# One-time setup for readers of "The History of Direct3D".
#
# Just want to READ the book?  No setup: open book\index.html in any browser.
# Want the "Run sample" buttons to work?  Run this once from this folder:
#
#   powershell -ExecutionPolicy Bypass -File .\setup.ps1
#
# From a shared zip (samples come prebuilt) this only unblocks the downloaded
# files and registers the per-user d3dbook: link handler the book's Run buttons
# use - no Visual Studio needed. From a source checkout it also checks your
# toolchain, downloads the preview SDKs and builds every sample. Safe to re-run.
# Options:
#   -Rebuild       build the samples even if prebuilt exes are present
#   -SkipPreview   don't download the Agility SDK / DXC / WARP preview packages
#   -SkipBuild     don't build the samples now (the Run button builds on demand)
#   -NoHandler     don't register the d3dbook: handler (Run buttons won't work)
#   -Open          open the book when done
#   -Uninstall     remove the d3dbook: handler and exit
param([switch]$Rebuild, [switch]$SkipPreview, [switch]$SkipBuild, [switch]$NoHandler, [switch]$Open, [switch]$Uninstall)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$problems = @()
function Step($t) { Write-Host "`n== $t" -ForegroundColor Green }
function Ok($t) { Write-Host "   OK   $t" }
function Warn($t) { Write-Host "   WARN $t" -ForegroundColor Yellow; $script:problems += $t }

if ($Uninstall) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\register-run-handler.ps1") -Unregister
    return
}

# Files extracted from a downloaded zip carry the "from the internet" mark, which
# makes Windows prompt before every sample exe. Clear it for this folder only.
Get-ChildItem -Path $root -Recurse -File | Unblock-File -ErrorAction SilentlyContinue

$sampleDirs = Get-ChildItem -Path $root -Directory -Recurse |
    Where-Object { $_.FullName -notmatch '\\(third_party|book|screenshots|audio|tools|docs|Out)(\\|$)' } |
    Where-Object { (Test-Path (Join-Path $_.FullName "main.cpp")) -or (Test-Path (Join-Path $_.FullName "main.c")) }
$missing = @($sampleDirs | Where-Object { -not (Test-Path (Join-Path $_.FullName ("Out\Cube" + $_.Name + ".exe"))) })
$prebuilt = $sampleDirs.Count -gt 0 -and $missing.Count -eq 0
if ($prebuilt -and -not $Rebuild) {
    Step "Prebuilt samples found"
    Ok "$($sampleDirs.Count) samples are ready to run - skipping the toolchain check and build (use -Rebuild to build anyway)"
    $SkipPreview = $true; $SkipBuild = $true
    $dev = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" -ErrorAction SilentlyContinue).AllowDevelopmentWithoutDevLicense
    if ($dev -ne 1) { Warn "Developer Mode is off: the preview Tier 9 samples (RayTracing12, LinearAlgebra, PartialPrograms) will fall back to a plain cube. Turn it on in Settings > System > For developers." }
}
else {
Step "Checking prerequisites"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
if (-not $vs) {
    Write-Host "   Visual Studio 2022 or later (or Build Tools) with 'Desktop development with C++' is required." -ForegroundColor Red
    Write-Host "   Install it, then re-run setup.ps1:  winget install Microsoft.VisualStudio.2022.BuildTools --override `"--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive`""
    exit 1
}
Ok "Visual Studio C++ tools: $vs"

$kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include"
$sdk = if (Test-Path $kits) { Get-ChildItem $kits -Directory | Where-Object Name -match '^10\.0\.\d+\.\d+$' | Sort-Object { [version]$_.Name } | Select-Object -Last 1 } else { $null }
if (-not $sdk) { Warn "No Windows 10/11 SDK found; install one from the Visual Studio Installer." }
elseif ([version]$sdk.Name -lt [version]"10.0.22000.0") { Warn "Windows SDK $($sdk.Name) is older than 10.0.22000; some D3D12 samples need a newer one." }
else { Ok "Windows SDK $($sdk.Name)" }

$dev = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" -ErrorAction SilentlyContinue).AllowDevelopmentWithoutDevLicense
if ($dev -eq 1) { Ok "Developer Mode is on (needed by the preview samples)" }
else { Warn "Developer Mode is off: the preview Tier 9 samples (RayTracing12, LinearAlgebra, PartialPrograms) will fall back to a plain cube. Turn it on in Settings > System > For developers." }
}

if (-not $SkipPreview) {
    Step "Downloading preview SDKs for the frontier samples (NuGet)"
    Get-ChildItem -Path (Join-Path $root "D3D12") -Recurse -Filter fetch-deps.ps1 | ForEach-Object {
        Write-Host "   $($_.Directory.Name)"
        & powershell -NoProfile -ExecutionPolicy Bypass -File $_.FullName
        if ($LASTEXITCODE -ne 0) { Warn "fetch-deps failed for $($_.Directory.Name) (network?). That sample will fall back or fail to build." }
    }
}

if (-not $SkipBuild) {
    Step "Building all samples (a few minutes)"
    $log = Join-Path $env:TEMP "HistoryOfD3D-build.log"
    # Stream the build live (like running build-all.ps1 directly) while also
    # saving it, so the failure summary below can be computed from the log.
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "build-all.ps1") 2>&1 |
        Tee-Object -FilePath $log |
        ForEach-Object {
            $line = "$_"
            if ($line -match '^RESULT .*: OK') { Write-Host $line -ForegroundColor Green }
            elseif ($line -match '^RESULT .*: FAILED|error C\d+|error LNK\d+') { Write-Host $line -ForegroundColor Red }
            elseif ($line -match '^=+ BUILD ') { Write-Host $line -ForegroundColor Cyan }
            else { Write-Host $line }
        }
    $results = Select-String -Path $log -Pattern '^RESULT (\S+): (OK|FAILED)'
    $failed = @($results | Where-Object { $_.Matches[0].Groups[2].Value -eq "FAILED" } | ForEach-Object { $_.Matches[0].Groups[1].Value })
    Ok "$(@($results).Count - $failed.Count) of $(@($results).Count) samples built (full log: $log)"
    if ($failed) { Warn "Failed to build: $($failed -join ', ')" }

    # D3D7/D3D8 need 32-bit builds on modern Windows (legacy runtimes are SysWOW64-only).
    . (Join-Path $root "tools\legacy32.ps1")
    foreach ($n in "D3D7.Basic", "D3D8.Basic") {
        if (Build-Legacy32 (Get-Item (Join-Path $root $n))) { Ok "$n x86 build" } else { Warn "$n x86 build failed; it will show 'init failed'." }
    }
}

if (-not $NoHandler) {
    Step "Registering the d3dbook: handler for the Run sample buttons (current user only)"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\register-run-handler.ps1") | Out-Null
    Ok "d3dbook: -> $(Join-Path $root 'tools\run-sample.ps1')  (undo: .\setup.ps1 -Uninstall)"
}

Step "Done"
$book = Join-Path $root "book\index.html"
Write-Host "   Open $book"
Write-Host "   On a sample page, click 'Run sample' (or press R). Your browser asks once before opening d3dbook: links."
if ($problems) { Write-Host "`n   $($problems.Count) warning(s) above - the book still works; affected samples fall back." -ForegroundColor Yellow }
if ($Open) { Start-Process $book }
