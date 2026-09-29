# Builds a 32-bit copy of a legacy sample (D3D7.Basic / D3D8.Basic) into
# <sample>\Out\x86\. Modern Windows ships the D3D7/D3D8 runtimes only as 32-bit
# DLLs (SysWOW64), so the x64 build from build-all.ps1 reports "init failed".
# Dot-source this file, then: $exe = Build-Legacy32 (Get-Item <sampleDir>)

function Build-Legacy32($sample) {
    # Rebuild only when main.cpp is newer than the cached x86 exe.
    $src = Join-Path $sample.FullName "main.cpp"
    $outX86 = Join-Path $sample.FullName "Out\x86"
    $exe = Join-Path $outX86 ("Cube" + $sample.Name + ".exe")
    if ((Test-Path $exe) -and (Get-Item $exe).LastWriteTime -ge (Get-Item $src).LastWriteTime) { return $exe }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -property installationPath
    $vcvars32 = Join-Path $vs "VC\Auxiliary\Build\vcvars32.bat"
    if (-not (Test-Path $vcvars32)) { Write-Host "  (no vcvars32.bat; using x64 build)" -ForegroundColor Yellow; return $null }
    New-Item -ItemType Directory -Force -Path $outX86 | Out-Null
    $cmd = "call `"$vcvars32`" >nul 2>&1 && cd /d `"$($sample.FullName)`" && cl /nologo /EHsc /W0 /I. main.cpp user32.lib /Fo`"$outX86\\`" /Fd`"$outX86\\`" /Fe:`"$exe`" >nul"
    & $env:ComSpec /c $cmd | Out-Null
    if (Test-Path $exe) { return $exe }
    Write-Host "  (x86 build failed; using x64 build)" -ForegroundColor Yellow
    return $null
}
