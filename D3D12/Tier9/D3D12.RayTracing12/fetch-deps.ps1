param([switch]$Force)

# Stages the preview Agility SDK (headers + D3D12Core), the preview DXC (lib_6_9 / SER HLSL API)
# and the preview WARP software rasterizer (DXR 1.2: OMM + SER) under third_party\.
# build-all.ps1 then copies agility\bin -> Out\D3D12\ and dxc\bin + warp\bin -> Out\.

$ErrorActionPreference = "Stop"
$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ThirdParty = Join-Path $SampleDir "third_party"
$Work = Join-Path $SampleDir "Out\deps"

function Test-StagedDeps {
    $required = @(
        "third_party\agility\include\d3d12.h",
        "third_party\agility\bin\x64\D3D12Core.dll",
        "third_party\agility\bin\x64\D3D12SDKLayers.dll",
        "third_party\dxc\include\dx\linalg.h",
        "third_party\dxc\bin\x64\dxcompiler.dll",
        "third_party\dxc\bin\x64\dxil.dll",
        "third_party\warp\bin\x64\d3d10warp.dll"
    )
    foreach ($rel in $required) {
        if (-not (Test-Path (Join-Path $SampleDir $rel))) { return $false }
    }
    return $true
}

function Download-NuGetPackage($Id, $Version, $Destination) {
    $lowerId = $Id.ToLowerInvariant()
    $url = "https://api.nuget.org/v3-flatcontainer/$lowerId/$Version/$lowerId.$Version.nupkg"
    if ((Test-Path $Destination) -and -not $Force) { return }
    Write-Host "Downloading $Id $Version"
    Invoke-WebRequest -Uri $url -OutFile $Destination
}

function Expand-NuGetPackage($Package, $Destination) {
    $zip = [IO.Path]::ChangeExtension($Package, ".zip")
    if (-not (Test-Path $zip) -or $Force) {
        Copy-Item $Package $zip -Force
    }
    Expand-Archive -Path $zip -DestinationPath $Destination
}

if ((Test-StagedDeps) -and -not $Force) {
    Write-Host "third_party preview dependencies are already staged. Use -Force to refresh."
    return
}

New-Item -ItemType Directory -Force -Path $Work | Out-Null
$agilityNupkg = Join-Path $Work "microsoft.direct3d.d3d12.1.721.2-preview.nupkg"
$dxcNupkg = Join-Path $Work "microsoft.direct3d.dxc.1.10.2605.24-preview.nupkg"
$warpNupkg = Join-Path $Work "microsoft.direct3d.warp.1.65535.20-preview.nupkg"
$agilityExpand = Join-Path $Work "agility"
$dxcExpand = Join-Path $Work "dxc"
$warpExpand = Join-Path $Work "warp"

if ($Force) {
    Remove-Item $agilityExpand, $dxcExpand, $warpExpand -Recurse -Force -ErrorAction SilentlyContinue
}

Download-NuGetPackage "Microsoft.Direct3D.D3D12" "1.721.2-preview" $agilityNupkg
Download-NuGetPackage "Microsoft.Direct3D.DXC" "1.10.2605.24-preview" $dxcNupkg
Download-NuGetPackage "Microsoft.Direct3D.WARP" "1.65535.20-preview" $warpNupkg

if (-not (Test-Path $agilityExpand)) { Expand-NuGetPackage $agilityNupkg $agilityExpand }
if (-not (Test-Path $dxcExpand)) { Expand-NuGetPackage $dxcNupkg $dxcExpand }
if (-not (Test-Path $warpExpand)) { Expand-NuGetPackage $warpNupkg $warpExpand }

$agilityInclude = Join-Path $ThirdParty "agility\include"
$agilityBin = Join-Path $ThirdParty "agility\bin\x64"
$dxcInclude = Join-Path $ThirdParty "dxc\include"
$dxcBin = Join-Path $ThirdParty "dxc\bin\x64"
$warpBin = Join-Path $ThirdParty "warp\bin\x64"
New-Item -ItemType Directory -Force -Path $agilityInclude, $agilityBin, $dxcInclude, $dxcBin, $warpBin | Out-Null

Copy-Item (Join-Path $agilityExpand "build\native\include\*") $agilityInclude -Recurse -Force
Copy-Item (Join-Path $agilityExpand "build\native\bin\x64\D3D12Core.dll") $agilityBin -Force
Copy-Item (Join-Path $agilityExpand "build\native\bin\x64\D3D12SDKLayers.dll") $agilityBin -Force
Copy-Item (Join-Path $dxcExpand "build\native\include\hlsl\*") $dxcInclude -Recurse -Force
Copy-Item (Join-Path $dxcExpand "build\native\bin\x64\dxcompiler.dll") $dxcBin -Force
Copy-Item (Join-Path $dxcExpand "build\native\bin\x64\dxil.dll") $dxcBin -Force
# [LEARN] The preview WARP is just d3d10warp.dll. Placed next to the exe it wins the normal
# DLL search order over System32's in-box WARP when EnumWarpAdapter + D3D12CreateDevice load it.
Copy-Item (Join-Path $warpExpand "build\native\bin\x64\d3d10warp.dll") $warpBin -Force

Write-Host "Preview Agility SDK, DXC and WARP staged under third_party."
