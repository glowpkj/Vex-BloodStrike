# Launch an exe elevated with no visible window and no focus steal.
#
# Why this exists, precisely:
#
#   run_elevated.cmd captures output by writing a small .cmd file and elevating
#   THAT, with the redirect done by cmd ("%EXE%" %ARGS% > "%OUT%").  A cmd window
#   therefore exists for the whole run.  For a probe that is harmless.  For the
#   overlay it was not:
#
#   bs_esp.exe is /SUBSYSTEM:WINDOWS -- verified by reading the PE header, its
#   Subsystem field is 2, not 3.  It has no console of its own.  Its printf
#   output only reaches a file because, when a GUI-subsystem exe is started from
#   a process that HAS a console, Windows attaches the child to that console and
#   the child's standard handles stay valid.  So the redirect needs a console
#   somewhere -- and run_elevated.cmd supplied one that was visible and that took
#   focus.  The reported symptoms of the last run were a black window appearing
#   over the game, the mouse buttons doing nothing, and the game appearing to
#   hang.  All three are that console: it held the focus.
#
#   This script keeps the console, because the log is worth having, but the
#   console is not opened by a shell that shows itself.  The exe is started
#   directly with -RedirectStandardOutput, and the child is a GUI subsystem
#   process, so no window is ever mapped.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File run_silent.ps1 <exe> <outfile> [args...]

param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)

$ErrorActionPreference = 'Stop'

# Resolve the exe relative to this script's repo root, not a hardcoded path.
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not (Test-Path $Exe)) {
    $candidate = Join-Path $root 'out' | Join-Path -ChildPath $Exe
    if (Test-Path $candidate) { $Exe = $candidate }
    else { Write-Error "not found: $Exe"; exit 1 }
}

$outDir = Split-Path -Parent $Out
if ($outDir -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
if (-not (Test-Path $Out)) { New-Item -ItemType File -Path $Out -Force | Out-Null }

$argList = @()
if ($Rest) { $argList += $Rest }

# -UseNewEnvironment gives the elevated child a clean environment; without it the
# elevated process can inherit a PATH from the unelevated session, which is how
# a "the exe runs but cannot find its DLLs" failure appears under UAC.
$p = Start-Process -FilePath $Exe `
                   -ArgumentList $argList `
                   -Verb RunAs `
                   -PassThru `
                   -Wait `
                   -WindowStyle Hidden `
                   -RedirectStandardOutput $Out `
                   -RedirectStandardError  $Out

exit $p.ExitCode
