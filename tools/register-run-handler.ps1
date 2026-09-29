# Registers the per-user "d3dbook:" URL protocol so the HTML book's "Run sample"
# buttons can launch sample executables. Writes only under
# HKCU\Software\Classes\d3dbook (no admin needed). Undo with -Unregister.
#
#   powershell -ExecutionPolicy Bypass -File .\tools\register-run-handler.ps1
#   powershell -ExecutionPolicy Bypass -File .\tools\register-run-handler.ps1 -Unregister
#
# The handler runs tools\run-sample.ps1 from THIS checkout; re-run after moving the repo.
# Browsers ask for confirmation the first time a page opens a d3dbook: link.
param([switch]$Unregister)

$ErrorActionPreference = "Stop"
$key = "HKCU:\Software\Classes\d3dbook"

if ($Unregister) {
    if (Test-Path $key) { Remove-Item $key -Recurse -Force; Write-Host "Removed d3dbook: protocol handler." }
    else { Write-Host "d3dbook: protocol handler was not registered." }
    return
}

$script = Join-Path $PSScriptRoot "run-sample.ps1"
$ps = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
$command = "`"$ps`" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$script`" `"%1`""

New-Item -Path $key -Force | Out-Null
Set-ItemProperty -Path $key -Name "(default)" -Value "URL:History of Direct3D sample launcher"
New-ItemProperty -Path $key -Name "URL Protocol" -Value "" -PropertyType String -Force | Out-Null
New-Item -Path "$key\DefaultIcon" -Force | Out-Null
Set-ItemProperty -Path "$key\DefaultIcon" -Name "(default)" -Value "`"$ps`",0"
New-Item -Path "$key\shell\open\command" -Force | Out-Null
Set-ItemProperty -Path "$key\shell\open\command" -Name "(default)" -Value $command

Write-Host "Registered d3dbook: -> $script"
Write-Host "Test: Start-Process 'd3dbook:run/D3D10.Basic'"
