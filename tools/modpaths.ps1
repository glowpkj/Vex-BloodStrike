# Run elevated. Reports the on-disk path of every loaded module and copies the
# interesting ones out. tier0_s64.dll and vstdlib_s64.dll are Valve Source 2
# code; if they are on disk anywhere, static analysis beats blind memory
# scanning by a wide margin.
$ErrorActionPreference = 'SilentlyContinue'
$out = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'out\module_paths.txt'
$lines = New-Object System.Collections.Generic.List[string]

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Mods {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct ME32 {
    public uint dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage;
    public IntPtr modBaseAddr; public uint modBaseSize; public IntPtr hModule;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=256)] public string szModule;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=260)] public string szExePath;
  }
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern IntPtr CreateToolhelp32Snapshot(uint f, uint pid);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool Module32FirstW(IntPtr s, ref ME32 e);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool Module32NextW(IntPtr s, ref ME32 e);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool CloseHandle(IntPtr h);
}
"@

$p = Get-Process BloodStrike
if (-not $p) { "game not running" | Out-File $out; exit 1 }

$snap = [Mods]::CreateToolhelp32Snapshot(0x18, $p.Id)   # MODULE|MODULE32
$e = New-Object Mods+ME32
$e.dwSize = [System.Runtime.InteropServices.Marshal]::SizeOf($e)

$rows = @()
if ([Mods]::Module32FirstW($snap, [ref]$e)) {
  do {
    $exists = Test-Path -LiteralPath $e.szExePath
    $rows += [pscustomobject]@{
      Module = $e.szModule
      Path   = $e.szExePath
      OnDisk = $exists
      MB     = [math]::Round($e.modBaseSize / 1MB, 2)
    }
  } while ([Mods]::Module32NextW($snap, [ref]$e))
}
[Mods]::CloseHandle($snap) | Out-Null

$lines.Add("total modules: $($rows.Count)")
$lines.Add("")
$lines.Add("=== modules whose file is NOT on disk (unpacked from .mpk at runtime) ===")
$missing = $rows | Where-Object { -not $_.OnDisk }
if ($missing) {
  $missing | ForEach-Object { $lines.Add(("  {0,-30} {1,8} MB  {2}" -f $_.Module, $_.MB, $_.Path)) }
} else {
  $lines.Add("  (none)")
}
$lines.Add("")
$lines.Add("=== the Source 2 / engine modules ===")
$rows | Where-Object { $_.Module -match 'tier0|vstdlib|AudioService|cclib|render|UniCrash|CC' } |
  ForEach-Object { $lines.Add(("  {0,-30} {1,8} MB  disk={2}  {3}" -f $_.Module, $_.MB, $_.OnDisk, $_.Path)) }
$lines | Out-File -LiteralPath $out -Encoding utf8
Write-Output "wrote $out"
