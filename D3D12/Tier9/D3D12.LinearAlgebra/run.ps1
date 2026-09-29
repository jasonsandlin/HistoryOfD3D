# Runs the LinearAlgebra sample from Out\ (the layout build-all produces).
# Extra arguments are forwarded: .\run.ps1 --warp | --hw | --debug
# Re-stages the preview DLLs first so the exe also works after a manual cl build.
$ErrorActionPreference = "Stop"
$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutDir = Join-Path $SampleDir "Out"
$AgilityOut = Join-Path $OutDir "D3D12"
$Exe = Join-Path $OutDir "CubeD3D12.LinearAlgebra.exe"

if (-not (Test-Path (Join-Path $SampleDir "third_party\warp\bin\x64\d3d10warp.dll"))) {
    throw "Run .\fetch-deps.ps1 first (preview Agility SDK, DXC and WARP)."
}
if (-not (Test-Path $Exe)) {
    throw "Build first: $Exe not found (build-all.ps1 -Only D3D12.LinearAlgebra)."
}

New-Item -ItemType Directory -Force -Path $AgilityOut | Out-Null
Copy-Item (Join-Path $SampleDir "third_party\agility\bin\x64\*.dll") $AgilityOut -Force
Copy-Item (Join-Path $SampleDir "third_party\dxc\bin\x64\*.dll") $OutDir -Force
Copy-Item (Join-Path $SampleDir "third_party\warp\bin\x64\*.dll") $OutDir -Force

# Working directory = Out\, so the log lands in Out\CubeD3D12.LinearAlgebra.log.
Push-Location $OutDir
try {
    & $Exe @args
} finally {
    Pop-Location
}
