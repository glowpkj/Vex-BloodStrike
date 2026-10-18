# -----------------------------------------------------------------------------
#  launch.ps1 -- start bs_esp elevated AND open a live log terminal beside it.
#
#  Two windows, two purposes:
#
#    bs_esp.exe   elevated, --ulw (UpdateLayeredWindow present path), persistent
#                 (comes up immediately, waits in the menu, sweeps on the first
#                 live position).  Runs headless of any console of its own --
#                 it allocates one internally but does not take focus.
#
#    watch_log    a separate, non-elevated window following the log file.  It is
#                 started first, so the first line is captured.  It does not
#                 follow the log through stdout redirection: the program writes
#                 the file itself and flushes per line, so nothing is lost to a
#                 cmd re-buffering what it reads.
#
#  Nothing here activates a window.  The game must keep the foreground and its
#  clipped mouse; a console taking focus is what broke input in the earliest
#  builds of the overlay.
#
#  Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File launch.ps1
# -----------------------------------------------------------------------------

param(
    [string]$Exe = '',
    [string]$Log = '',
    [switch]$NoLog,
    [string[]]$Rest = @('--ulw')
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here

# Default paths are derived from this script's location, not hardcoded, so the
# repo works from any directory. Override -Exe / -Log to point elsewhere.
if ([string]::IsNullOrEmpty($Exe)) { $Exe = Join-Path $root 'out\bs_esp.exe' }
if ([string]::IsNullOrEmpty($Log)) { $Log = Join-Path $root 'out\vex_log.txt' }

# -----------------------------------------------------------------------------
#  ORDER: THE TOOL IS LAUNCHED FIRST, THE LOG WINDOW SECOND.
#
#  This used to be the other way round, and it was the reason "the launcher does
#  nothing but the direct command works" -- a bug that cost two sessions because
#  every explanation for it was about the program, and the program was fine.
#
#  The sequence was: truncate the log, Start-Process the watcher, sleep, then
#  launch the tool.  With $ErrorActionPreference = 'Stop', ANY failure in the
#  watcher launch -- and a windowed Start-Process of powershell can fail for
#  reasons that have nothing to do with this project -- aborts the script at that
#  line.  The elevated cmd below it never runs, or runs with an empty command,
#  and the observed result is exactly: "elevated pid N" printed, no process, no
#  log, no watcher, no error.
#
#  Launching the tool first removes the dependency entirely.  The watcher is a
#  convenience; the tool is the thing being run.  A convenience must not be able
#  to prevent the thing.
# -----------------------------------------------------------------------------

$argString = ''
if ($Rest) { $argString = ($Rest -join ' ') }

Write-Host "launching $Exe $argString  (accept the UAC prompt)" -ForegroundColor Cyan

# Elevate through a hidden cmd, exactly as run_hidden.ps1 does, so the elevated
# console never appears over the game.
$inner = Join-Path $env:TEMP ("vex_launch_{0}.cmd" -f (Get-Random))
$outDir = Split-Path -Parent $Exe
@(
  '@echo off',
  ('cd /d "{0}"' -f $outDir),
  ('"{0}" {1}' -f $Exe, $argString),
  'exit %ERRORLEVEL%'
) | Set-Content -Path $inner -Encoding ASCII

$launched = $false
try {
    $p = Start-Process -FilePath $env:ComSpec `
                       -ArgumentList '/c', "`"$inner`"" `
                       -Verb RunAs `
                       -PassThru `
                       -WindowStyle Hidden
    $launched = $true
    Write-Host ("elevated pid {0}" -f $p.Id) -ForegroundColor Green
} catch {
    Write-Host ("launch failed: {0}" -f $_.Exception.Message) -ForegroundColor Red
} finally {
    Remove-Item $inner -ErrorAction SilentlyContinue
}

if (-not $launched) {
    Write-Host "the tool was not started; nothing else to do" -ForegroundColor Red
    exit 1
}

# -----------------------------------------------------------------------------
#  The log window, now that the tool is already running.
#
#  Everything here is best-effort and wrapped: if the watcher cannot start, the
#  operator still has a running tool and the log file on disk, which is the
#  outcome that matters.  A failure is reported, not raised.
# -----------------------------------------------------------------------------
if (-not $NoLog) {
    foreach ($p2 in @($Log, (Join-Path $env:TEMP 'vex_log.txt'))) {
        if (Test-Path $p2) {
            try { Clear-Content -Path $p2 -Force -ErrorAction Stop }
            catch { Remove-Item $p2 -Force -ErrorAction SilentlyContinue }
        }
    }

    # Wait for the tool to create its log, so the watcher does not start on a
    # missing file and print "no log file yet" before one exists.  Bounded, so a
    # tool that fails to start does not hang the launcher for ever.
    $deadline = (Get-Date).AddSeconds(8)
    while ((Get-Date) -lt $deadline -and -not (Test-Path $Log)) {
        Start-Sleep -Milliseconds 200
    }

    try {
        Start-Process -FilePath 'powershell' `
            -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass',
                          '-File', (Join-Path $here 'watch_log.ps1'),
                          '-Path', $Log `
            -WindowStyle Normal
        Write-Host ("log terminal is watching {0}" -f $Log) -ForegroundColor Green
    } catch {
        Write-Host ("log terminal could not start: {0}" -f $_.Exception.Message) `
            -ForegroundColor Yellow
        Write-Host ("the tool is running; read the log directly at {0}" -f $Log) `
            -ForegroundColor Yellow
    }
}

