# Launch an exe elevated, with the elevated console hidden and no focus steal.
#
# ---------------------------------------------------------------------------
#  Why the previous launcher put a black window on the game
#
#  run_elevated.cmd elevated a small .cmd wrapper with Start-Process -Verb RunAs
#  and passed -WindowStyle Hidden to *PowerShell*.  That hides PowerShell's own
#  window -- which is not the one that appears.  The visible window is the
#  elevated cmd, and it was never told to hide, so it stayed on screen for the
#  whole run, took the focus, and left the game unable to receive mouse buttons.
#
#  The three reported symptoms of that run were a black window over the game,
#  mouse buttons doing nothing, and the game appearing to hang.  One cause.
#
#  -WindowStyle Hidden has to be applied to the process that actually has the
#  console -- the elevated cmd -- and it does work there, because cmd is a
#  console application.  Verified: an elevated `cmd /c <script>` with
#  -WindowStyle Hidden runs invisible and still writes its redirect.
#
#  -Verb RunAs cannot be combined with -RedirectStandardOutput: the two live in
#  mutually exclusive parameter sets, since a shell-executed process has no
#  standard handles to give.  So the redirect is done by the elevated cmd, as
#  before, and only the window handling changes.
# ---------------------------------------------------------------------------
#
# Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File run_hidden.ps1 <exe> <outfile> [args...]

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

$argString = ''
if ($Rest) { $argString = ($Rest -join ' ') }

$inner = Join-Path $env:TEMP ("vex_run_{0}.cmd" -f (Get-Random))

# The redirect happens here, inside the elevated cmd, because that is the only
# place it can happen once -Verb RunAs is involved.
$exeDir = Split-Path -Parent $Exe
@(
  '@echo off',
  ('cd /d "{0}"' -f $exeDir),
  ('"{0}" {1} > "{2}" 2>&1' -f $Exe, $argString, $Out),
  'exit %ERRORLEVEL%'
) | Set-Content -Path $inner -Encoding ASCII

try {
    $p = Start-Process -FilePath $env:ComSpec `
                       -ArgumentList '/c', "`"$inner`"" `
                       -Verb RunAs `
                       -PassThru `
                       -Wait `
                       -WindowStyle Hidden
    exit $p.ExitCode
}
finally {
    Remove-Item $inner -ErrorAction SilentlyContinue
}
