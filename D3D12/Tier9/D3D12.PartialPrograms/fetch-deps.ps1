# Downloads the preview Agility SDK (headers + D3D12Core), the preview DXC and the
# preview WARP software rasterizer from NuGet and stages them under third_party\
# (git-ignored). build-all.ps1 then adds third_party\agility\include to the include
# path and copies the DLLs next to Out\CubeD3D12.PartialPrograms.exe:
#   agility\bin\x64\*.dll -> Out\D3D12\   (found via the exported D3D12SDKPath)
#   dxc\bin\x64\*.dll     -> Out\         (loaded explicitly by main.cpp)
#   warp\bin\x64\*.dll    -> Out\         (d3d10warp.dll picked up by DLL search order)
param([switch]$Force)

$ErrorActionPreference = "Stop"
$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ThirdParty = Join-Path $SampleDir "third_party"
$Work = Join-Path $SampleDir "Out\deps"

$AgilityVersion = "1.721.2-preview"
$DxcVersion = "1.10.2605.24-preview"
$WarpVersion = "1.65535.20-preview"

function Test-StagedDeps {
    $required = @(
        "third_party\agility\include\d3d12.h",
        "third_party\agility\include\d3dx12\d3dx12.h",
        "third_party\agility\bin\x64\D3D12Core.dll",
        "third_party\agility\bin\x64\D3D12SDKLayers.dll",
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
$agilityNupkg = Join-Path $Work "microsoft.direct3d.d3d12.$AgilityVersion.nupkg"
$dxcNupkg = Join-Path $Work "microsoft.direct3d.dxc.$DxcVersion.nupkg"
$warpNupkg = Join-Path $Work "microsoft.direct3d.warp.$WarpVersion.nupkg"
$agilityExpand = Join-Path $Work "agility"
$dxcExpand = Join-Path $Work "dxc"
$warpExpand = Join-Path $Work "warp"

if ($Force) {
    Remove-Item $agilityExpand, $dxcExpand, $warpExpand -Recurse -Force -ErrorAction SilentlyContinue
}

Download-NuGetPackage "Microsoft.Direct3D.D3D12" $AgilityVersion $agilityNupkg
Download-NuGetPackage "Microsoft.Direct3D.DXC" $DxcVersion $dxcNupkg
Download-NuGetPackage "Microsoft.Direct3D.WARP" $WarpVersion $warpNupkg

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
Copy-Item (Join-Path $dxcExpand "build\native\include\*") $dxcInclude -Recurse -Force
Copy-Item (Join-Path $dxcExpand "build\native\bin\x64\dxcompiler.dll") $dxcBin -Force
Copy-Item (Join-Path $dxcExpand "build\native\bin\x64\dxil.dll") $dxcBin -Force
Copy-Item (Join-Path $warpExpand "build\native\bin\x64\d3d10warp.dll") $warpBin -Force

Write-Host "Preview Agility SDK $AgilityVersion, DXC $DxcVersion and WARP $WarpVersion staged under third_party."
