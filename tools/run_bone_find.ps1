# -----------------------------------------------------------------------------
#  run_bone_find.ps1 -- run bone_find elevated and hold the window open.
#
#  Why a script and not just "run it from a terminal": the tool needs
#  OpenProcess with PROCESS_VM_READ on a protected game process, which requires
#  elevation, and the result is a table the operator has to read.  A plain
#  elevated Start-Process closes the console the moment the program exits, so
#  the table would flash and vanish -- which is how the first measurement of
#  this kind was lost and had to be re-run.
#
#  The window stays until a key is pressed.  The output is also teed to a file
#  next to the tool, so the numbers survive even if the window is closed by
#  accident and can be read back without elevation.
# -----------------------------------------------------------------------------
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = Join-Path $root 'out\bone_find.exe'
$out = Join-Path $root 'out\bone_find.txt'

if (-not (Test-Path $exe)) {
    Write-Host "not found: $exe" -ForegroundColor Red
    Write-Host "build it first: cmake --build build --config Release --target bone_find"
    exit 1
}

Write-Host @"

  bone_find -- locate the bone array AND ITS STRIDE
  =================================================

  This needs you IN A MATCH, ALIVE, and MOVING.

  While the 20-second measurement runs, WALK IN A STRAIGHT LINE and
  do not stop.  Standing still scores near zero and the verdict
  will say so -- the first run moved in only 38 of 120 frames and
  could not be trusted.

  Accept the UAC prompt when it appears.

"@ -ForegroundColor Cyan

# The inner script runs the tool, captures everything, writes the file, and
# waits for a keypress before closing.  Written to a temp file rather than
# passed as -Command so that quoting cannot go wrong on the paths.
$inner = Join-Path $env:TEMP ("vex_bone_{0}.ps1" -f (Get-Random))

@"
`$ErrorActionPreference = 'Continue'
Set-Location '$root\out'
& '$exe' 2>&1 | Tee-Object -FilePath '$out'
Write-Host ''
Write-Host '---------------------------------------------------------------' -ForegroundColor DarkGray
Write-Host "result also written to $out" -ForegroundColor DarkGray
Write-Host 'press any key to close' -ForegroundColor DarkGray
`$null = `$Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown')
"@ | Set-Content -Path $inner -Encoding UTF8

try {
    $p = Start-Process -FilePath 'powershell' `
                       -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $inner `
                       -Verb RunAs `
                       -PassThru
    Write-Host ("elevated pid {0}; a console will open -- walk while it counts" -f $p.Id) -ForegroundColor Green
} finally {
    # The inner file is left on disk for the elevated process to read; it is
    # named with a random suffix and removed on the next run of this script.
    Get-ChildItem -Path $env:TEMP -Filter 'vex_bone_*.ps1' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -ne $inner } |
        Remove-Item -Force -ErrorAction SilentlyContinue
}
