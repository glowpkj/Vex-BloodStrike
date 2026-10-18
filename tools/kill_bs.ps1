# Kill bs_esp.exe from an elevated context.
#
# The overlay process runs elevated (it needs OpenProcess against the game), and
# a non-elevated shell cannot terminate it -- taskkill returns "Access denied",
# which is exactly what happened when the user could not click anything to close
# it: the overlay had hold of the mouse and the task manager could not be
# reached with it.
#
# This runs through the same elevated launcher the overlay itself uses, so the
# UAC prompt is the familiar one and the kill actually has the privilege.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root 'out\kill.txt'
"attempting" | Set-Content -Path $out
& taskkill /F /IM bs_esp.exe *>> $out
Start-Sleep -Milliseconds 800
if (Get-Process bs_esp -ErrorAction SilentlyContinue) {
    "still running" | Add-Content -Path $out
} else {
    "killed" | Add-Content -Path $out
}
