# Downloads the DirectStorage SDK (NuGet Microsoft.Direct3D.DirectStorage) into
# third_party\dstorage\ - dstorage.h isn't part of the Windows SDK. Run once
# before building this sample; -Force refreshes.
param([switch]$Force, [string]$Version = "1.3.0")

$ErrorActionPreference = "Stop"
$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Dest = Join-Path $SampleDir "third_party\dstorage"
$need = @("include\dstorage.h", "include\dstorageerr.h", "bin\x64\dstorage.dll", "bin\x64\dstoragecore.dll", "lib\x64\dstorage.lib")
if (-not $Force -and -not ($need | Where-Object { -not (Test-Path (Join-Path $Dest $_)) })) {
    Write-Host "third_party DirectStorage SDK is already staged. Use -Force to refresh."
    return
}

$work = Join-Path $SampleDir "Out\deps"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$pkg = Join-Path $work "microsoft.direct3d.directstorage.$Version.zip"
if ($Force -or -not (Test-Path $pkg)) {
    Write-Host "Downloading Microsoft.Direct3D.DirectStorage $Version"
    Invoke-WebRequest -Uri "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.directstorage/$Version/microsoft.direct3d.directstorage.$Version.nupkg" -OutFile $pkg
}
$x = Join-Path $work "dstorage"
Remove-Item $x -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive -Path $pkg -DestinationPath $x

foreach ($rel in $need) {
    $leaf = Split-Path -Leaf $rel
    $arch = if ($rel -match '\\x64\\') { '\\x64\\' } else { '' }
    $src = Get-ChildItem -Path $x -Recurse -File -Filter $leaf | Where-Object { -not $arch -or $_.FullName -match $arch } | Select-Object -First 1
    if (-not $src) { throw "$leaf not found in the DirectStorage package" }
    $to = Join-Path $Dest $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
    Copy-Item $src.FullName $to -Force
}
Write-Host "DirectStorage SDK $Version staged under third_party\dstorage."
