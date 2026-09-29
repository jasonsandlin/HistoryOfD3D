# Launches every built sample, grabs a still screenshot plus a short animation
# strip (a horizontal sprite sheet of frames) of its window, then closes it.
# Output feeds the interactive HTML book (tools\build_book.py):
#   screenshots\<Sample>.png        full-resolution still
#   screenshots\<Sample>.anim.jpg   N-frame sprite strip for hover animation
#   screenshots\shots.json          per-sample capture status
#
# Usage (from HistoryOfD3D\):
#   powershell -ExecutionPolicy Bypass -File .\tools\capture-screenshots.ps1
#   powershell -ExecutionPolicy Bypass -File .\tools\capture-screenshots.ps1 -Only D3D12.RayTracing
#
# The legacy D3D7/D3D8 runtimes only ship as 32-bit DLLs (SysWOW64) on modern
# Windows, so their x64 builds show "init failed". This script therefore builds
# an x86 copy of those two into <sample>\Out\x86\ (vcvars32) and captures that.
# -ExeOverride <path> captures a specific exe instead (use with -Only).
#
# Frames are grabbed on a fixed clock (~4 s by default) so the strips loop
# forward seamlessly enough for the gallery's auto-play.
#
# NOTE: windows are briefly brought to the foreground so the desktop compositor
# output can be copied - don't use the machine while this runs (~8 minutes).
param(
    [string]$Only = "",
    [string]$ExeOverride = "",
    [int]$WarmupMs = 3500,
    [int]$Frames = 36,
    [int]$FrameIntervalMs = 110,
    [int]$StripFrameWidth = 320
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$outDir = Join-Path $root "screenshots"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Collections.Generic;
public static class Win {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr v);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    public static IntPtr[] WindowsOf(uint pid) {
        var list = new List<IntPtr>();
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p == pid && IsWindowVisible(h)) list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }
    public static string ClassOf(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
    public static string TitleOf(IntPtr h) { var s = new StringBuilder(512); GetWindowText(h, s, 512); return s.ToString(); }
}
"@
[void][Win]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))   # per-monitor v2: physical pixels

function Grab-Client([IntPtr]$h) {
    $r = New-Object Win+RECT
    [void][Win]::GetClientRect($h, [ref]$r)
    $p = New-Object Win+POINT
    [void][Win]::ClientToScreen($h, [ref]$p)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return $null }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($p.X, $p.Y, 0, 0, (New-Object System.Drawing.Size $w, $hh))
    $g.Dispose()
    return $bmp
}


. (Join-Path $PSScriptRoot "legacy32.ps1")

function Save-Jpeg($bmp, $path, $quality) {
    $codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object MimeType -eq "image/jpeg"
    $ep = New-Object System.Drawing.Imaging.EncoderParameters 1
    $ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), ([long]$quality)
    $bmp.Save($path, $codec, $ep)
}

$samples = Get-ChildItem -Path $root -Directory -Recurse |
    Where-Object { $_.FullName -notmatch '\\(third_party|book|screenshots|Out)(\\|$)' } |
    Where-Object { (Test-Path (Join-Path $_.FullName "main.cpp")) -or (Test-Path (Join-Path $_.FullName "main.c")) } |
    Sort-Object FullName
if ($Only) { $samples = @($samples | Where-Object Name -eq $Only) }

$statusPath = Join-Path $outDir "shots.json"
$status = @{}
if (Test-Path $statusPath) {
    (Get-Content $statusPath -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $status[$_.Name] = $_.Value }
}

foreach ($s in $samples) {
    $name = $s.Name
    $exe = Join-Path $s.FullName "Out\Cube$name.exe"
    if ($ExeOverride) { $exe = (Resolve-Path $ExeOverride).Path }
    elseif ($name -in "D3D7.Basic", "D3D8.Basic") {
        $x86 = Build-Legacy32 $s
        if ($x86) { $exe = $x86 }
    }
    if (-not (Test-Path $exe)) { Write-Host "SKIP $name (not built)" -ForegroundColor Yellow; $status[$name] = @{ ok = $false; reason = "not built" }; continue }

    # Preview (Agility SDK) samples: stage vendored runtimes next to the exe
    # (build-all.ps1 does this too; repeated here so a stale Out\ still runs).
    $tp = Join-Path $s.FullName "third_party"
    if (Test-Path (Join-Path $tp "agility\bin\x64\D3D12Core.dll")) {
        $ag = Join-Path $s.FullName "Out\D3D12"; New-Item -ItemType Directory -Force $ag | Out-Null
        Copy-Item (Join-Path $tp "agility\bin\x64\*.dll") $ag -Force
    }
    foreach ($d in @("dxc\bin\x64", "warp\bin\x64")) {
        if (Test-Path (Join-Path $tp $d)) { Copy-Item (Join-Path $tp "$d\*.dll") (Join-Path $s.FullName "Out") -Force }
    }

    Write-Host "RUN  $name" -NoNewline
    $proc = Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) -PassThru
    try {
        $hwnd = [IntPtr]::Zero
        $deadline = (Get-Date).AddSeconds(15)
        while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
            $wins = [Win]::WindowsOf([uint32]$proc.Id)
            if ($wins.Count -gt 0) { $hwnd = $wins[0]; break }
            Start-Sleep -Milliseconds 200
        }
        if ($hwnd -eq [IntPtr]::Zero) { Write-Host "  -> no window" -ForegroundColor Red; $status[$name] = @{ ok = $false; reason = "no window" }; continue }
        Start-Sleep -Milliseconds $WarmupMs
        # Re-query: the app may have replaced its window with an error dialog.
        $wins = [Win]::WindowsOf([uint32]$proc.Id)
        if ($wins.Count -gt 0) { $hwnd = $wins[0] }
        $cls = [Win]::ClassOf($hwnd); $title = [Win]::TitleOf($hwnd)
        $isDialog = ($cls -eq "#32770")

        [void][Win]::SetWindowPos($hwnd, [IntPtr]::new(-1), 0, 0, 0, 0, 0x0003)   # TOPMOST, NOSIZE|NOMOVE
        [void][Win]::SetForegroundWindow($hwnd)
        Start-Sleep -Milliseconds 400

        $still = Grab-Client $hwnd
        if (-not $still) { Write-Host "  -> empty client" -ForegroundColor Red; $status[$name] = @{ ok = $false; reason = "empty client" }; continue }
        $still.Save((Join-Path $outDir "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)

        if (-not $isDialog) {
            $fw = $StripFrameWidth; $fh = [int]($still.Height * $fw / $still.Width)
            $strip = New-Object System.Drawing.Bitmap ($fw * $Frames), $fh
            $sg = [System.Drawing.Graphics]::FromImage($strip)
            $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            # Grab on a fixed clock (not sleep-after-grab) so frames are evenly
            # spaced; the measured span drives playback speed in the book.
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            for ($i = 0; $i -lt $Frames; $i++) {
                $wait = $i * $FrameIntervalMs - $sw.ElapsedMilliseconds
                if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }
                $f = Grab-Client $hwnd
                if ($f) { $sg.DrawImage($f, $i * $fw, 0, $fw, $fh); $f.Dispose() }
            }
            $spanMs = [int]($sw.ElapsedMilliseconds * $Frames / [Math]::Max(1, $Frames - 1))
            $sg.Dispose()
            Save-Jpeg $strip (Join-Path $outDir "$name.anim.jpg") 80
            $strip.Dispose()
        }
        $status[$name] = @{ ok = (-not $isDialog); dialog = $isDialog; title = $title; width = $still.Width; height = $still.Height; frames = $Frames; ms = $spanMs }
        $still.Dispose()
        if ($isDialog) { Write-Host "  -> dialog: $title" -ForegroundColor Yellow } else { Write-Host "  -> OK" -ForegroundColor Green }
    } finally {
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 300
    }
}

$status | ConvertTo-Json -Depth 4 | Set-Content -Path $statusPath -Encoding UTF8
Write-Host "`nWrote $statusPath"
