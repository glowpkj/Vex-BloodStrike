# -----------------------------------------------------------------------------
#  watch_log.ps1 -- live view of the armoury's own log file.
#
#  The log is written by the process itself, with an fflush after every line, to
#  a file next to the executable (or to %TEMP% if that path is not writable).
#  Nothing here reads the process's stdout: the earlier arrangement redirected
#  stdout through a cmd, cmd re-buffered it, and a perfectly healthy run looked
#  like it had stopped because the file stopped growing.
#
#  This window follows the file with Get-Content -Wait, so lines appear as they
#  are written.  It is started without -Wait and without activation, so it never
#  takes the foreground from the game and never touches the clipped mouse.
#
#  Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File watch_log.ps1
# -----------------------------------------------------------------------------

param(
    [string]$Path = '',
    [int]$Tail = 200
)

$ErrorActionPreference = 'Continue'

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ([string]::IsNullOrEmpty($Path)) { $Path = Join-Path $root 'out\vex_log.txt' }

# The process writes beside the exe first and falls back to TEMP; follow whichever
# exists, preferring the one that was modified most recently.
$candidates = @(
    $Path,
    (Join-Path $env:TEMP 'vex_log.txt')
) | Where-Object { Test-Path $_ }

if (-not $candidates) {
    Write-Host "no log file yet at:" -ForegroundColor Yellow
    $candidates = @($Path, (Join-Path $env:TEMP 'vex_log.txt'))
    $candidates | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
}

foreach ($p in $candidates) {
    if (Test-Path $p) {
        $f = Get-Item $p
        Write-Host ("watching {0}  ({1} bytes, modified {2})" -f `
            $p, $f.Length, $f.LastWriteTime) -ForegroundColor Cyan
        Write-Host "--------------------------------------------------------------"
        Get-Content $p -Tail $Tail -Wait
        break
    }
}
