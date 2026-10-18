// Test the offset sets found online against the live process, with the one
// invariant that actually settles it: a pointer into an object must have a
// vtable in .text, and the chain must terminate in a plausible object.
//
// Also decodes the two ASCII values the earlier run produced, because
// 0x7ECE2E8 and 0x7FCE2E8 differ by a single nibble and the dump may simply
// have a typo.
$ErrorActionPreference = 'SilentlyContinue'

$code = @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Mem {
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint a, bool i, int p);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, int s, out IntPtr r);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);

  static IntPtr _h;
  public static bool Attach(int pid) {
    _h = OpenProcess(0x10 | 0x400 | 0x1000, false, pid);
    return _h != IntPtr.Zero;
  }
  public static bool Read(long va, byte[] buf, int len) {
    IntPtr got;
    return ReadProcessMemory(_h, (IntPtr)va, buf, len, out got) && got.ToInt64() == len;
  }
  public static ulong U64(long va) { var b = new byte[8]; return Read(va, b, 8) ? BitConverter.ToUInt64(b, 0) : 0; }
  public static float F32(long va) { var b = new byte[4]; return Read(va, b, 4) ? BitConverter.ToSingle(b, 0) : 0; }
  public static int I32(long va) { var b = new byte[4]; return Read(va, b, 4) ? BitConverter.ToInt32(b, 0) : 0; }
  public static byte[] Bytes(long va, int len) { var b = new byte[len]; return Read(va, b, len) ? b : null; }
}
"@
Add-Type -TypeDefinition $code -ErrorAction SilentlyContinue

$p = Get-Process BloodStrike
if (-not $p) { Write-Output "game not running"; exit 1 }
if (-not [Mem]::Attach($p.Id)) { Write-Output "attach failed (need admin)"; exit 1 }

# Everything below goes to the transcript as well as the console, so the
# result survives however the script was launched.
$script:Rep = @()

$base = $p.MainModule.BaseAddress.ToInt64()
$textLo = $base + 0x1000
$textHi = $base + 0x1000 + 0x4E32C00
Write-Output "pid $($p.Id)  base 0x$('{0:X}' -f $base)  .text 0x$('{0:X}' -f $textLo)..0x$('{0:X}' -f $textHi)"
Write-Output ""
# Also write a copy next to the script so Start-Process redirection issues never
# hide the result.
$script:Report = New-Object System.Collections.Generic.List[string]

function Test-Object([long]$va) {
  if ($va -lt 0x10000 -or $va -gt 0x7FFFFFFFFFFF) { return "out of range" }
  $vt = [Mem]::U64($va)
  if ($vt -lt 0x10000) { return "unreadable (vt=0)" }
  if ($vt -ge $textLo -and $vt -lt $textHi) { return "OBJECT (vtable in .text)" }
  return "not an object (vt=0x$('{0:X}' -f $vt))"
}

function Show-Ascii([long]$va, [int]$n) {
  $b = [Mem]::Bytes($va, $n)
  if (-not $b) { return "(unreadable)" }
  ($b | ForEach-Object { if ($_ -ge 32 -and $_ -le 126) { [char]$_ } else { '.' } }) -join ''
}

# --- the two candidates that differ by one nibble -------------------------
Write-Output "=== ClientEngine candidates ==="
foreach ($cand in @(
  @{ Name = 'teu dump    0x7ECE2E8'; Rva = 0x7ECE2E8 },
  @{ Name = 'forum GL    0x7FCE2E8'; Rva = 0x7FCE2E8 },
  @{ Name = 'forum GL2   0x7FCD2E8'; Rva = 0x7FCD2E8 }
)) {
  $va = $base + $cand.Rva
  $val = [Mem]::U64($va)
  $script:Rep += ("  {0}  value=0x{1:X16}  {2}" -f $cand.Name, $val, (Show-Ascii $va 8))
  $script:Rep += ("      as object: {0}" -f (Test-Object $va))
}
$script:Rep | Set-Content -LiteralPath (Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'out\chain.txt') -Encoding utf8

Write-Output ""
Write-Output "=== walk the chain from each candidate ==="
foreach ($cand in @(
  @{ Name = 'teu dump  0x7ECE2E8'; Rva = 0x7ECE2E8 },
  @{ Name = 'forum GL  0x7FCE2E8'; Rva = 0x7FCE2E8 }
)) {
  $script:Rep += ("  --- {0} ---" -f $cand.Name)
  $eng = [Mem]::U64($base + $cand.Rva)
  $script:Rep += ("      ClientEngine   0x{0:X16}  {1}" -f $eng, (Test-Object $eng))
  if (-not (Test-Object $eng).StartsWith('OBJECT')) { continue }
  $gp = [Mem]::U64($eng + 0x58)
  $script:Rep += ("      +0x58 Gameplay  0x{0:X16}  {1}" -f $gp, (Test-Object $gp))
  if (-not (Test-Object $gp).StartsWith('OBJECT')) { continue }
  $pl = [Mem]::U64($gp + 0x58)
  $script:Rep += ("      +0x58 ClientPlr 0x{0:X16}  {1}" -f $pl, (Test-Object $pl))
  if (-not (Test-Object $pl).StartsWith('OBJECT')) { continue }
  $pl60 = [Mem]::U64($gp + 0x60)
  $script:Rep += ("      +0x60 Scenario 0x{0:X16}  {1}" -f $pl60, (Test-Object $pl60))
  $cam = [Mem]::U64($pl + 0x238)
  $script:Rep += ("      +0x238 Camera   0x{0:X16}  {1}" -f $cam, (Test-Object $cam))
  $act = [Mem]::U64($pl + 0x288)
  $script:Rep += ("      +0x288 Actor    0x{0:X16}  {1}" -f $act, (Test-Object $act))
}

Write-Output ""
Write-Output "=== entity list head (circular linked list) ==="
foreach ($h in @(0x6E4D0D8, 0x8A6FA58, 0x8A6EA58)) {
  $va = $base + $h
  $v = [Mem]::U64($va)
  $h8 = [Mem]::U64($va + 8)
  $script:Rep += ("  0x{0:X}  [0]=0x{1:X16}  [+8]=0x{2:X16}" -f $h, $v, $h8)
}
$script:Rep | Add-Content -LiteralPath (Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'out\chain.txt') -Encoding utf8
