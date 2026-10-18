// Find the engine chain by its shape instead of by a remembered address.
//
//  Every known source disagrees on where ClientEngine lives -- the dump says
//  0x7ECE2E8, the forum says 0x7FCE2E8 or 0x7FCD2E8, and all three read back as
//  ASCII strings in this build.  Addresses move between patches; structure does
//  not.  So search for the chain itself:
//
//      slot  ->  A       A+0x58 -> B      B+0x58 -> C
//
//  where A, B and C must each be pointers to live objects (first qword resolves
//  into .text), and B+0x58 must reach something that also answers at +0x238 and
//  +0x288.  A random qword has no chance of satisfying four linked constraints
//  at once, so this finds the real thing or nothing.
//
//  The candidate slots are only in the image's own writable sections: globals
//  live there, and searching the whole heap just turns up other subsystems.
$ErrorActionPreference = 'SilentlyContinue'

$code = @"
using System;
using System.Runtime.InteropServices;
public class M2 {
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint a, bool i, int p);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, int s, out IntPtr r);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  static IntPtr _h;
  public static bool Attach(int pid) { _h = OpenProcess(0x10|0x400|0x1000, false, pid); return _h != IntPtr.Zero; }
  public static bool Read(long va, byte[] b, int len) { IntPtr g; return ReadProcessMemory(_h,(IntPtr)va,b,len,out g) && g.ToInt64()==len; }
  public static ulong U64(long va) { var b=new byte[8]; return Read(va,b,8)?BitConverter.ToUInt64(b,0):0UL; }
  public static byte[] Chunk(long va, int len) { var b=new byte[len]; return Read(va,b,len)?b:null; }
}
"@
Add-Type -TypeDefinition $code -ErrorAction SilentlyContinue

$p = Get-Process BloodStrike
if (-not $p) { Write-Output "game not running"; exit 1 }
if (-not [M2]::Attach($p.Id)) { Write-Output "attach failed"; exit 1 }

$base  = $p.MainModule.BaseAddress.ToInt64()
$tlo   = $base + 0x1000
$thi   = $base + 0x1000 + 0x4E32C00
$dataLo = $base + 0x7636000
$dataHi = $base + 0x7636000 + 0x189EE24

$out = New-Object System.Collections.Generic.List[string]
$out.Add("pid $($p.Id)  base 0x$('{0:X}' -f $base)")
$out.Add("scanning .data 0x$('{0:X}' -f $dataLo)..0x$('{0:X}' -f $dataHi)")

function Is-Object([long]$va) {
  if ($va -lt 0x10000 -or $va -gt 0x00007FFFFFFFFFFFull) { return $false }
  $vt = [M2]::U64($va)
  return ($vt -ge $tlo -and $vt -lt $thi)
}

# Read .data in chunks; the structure is aligned, so step 8 bytes.
$CH = 8MB
$hits = New-Object System.Collections.Generic.List[object]
$scanned = 0L

for ($off = $dataLo; $off -lt $dataHi; $off += $CH) {
  $want = [int][Math]::Min($CH, $dataHi - $off)
  $buf = [M2]::Chunk($off, $want)
  if (-not $buf) { continue }
  $scanned += $want
  for ($i = 0; $i -le $want - 8; $i += 8) {
    $a = [BitConverter]::ToUInt64($buf, $i)
    if (-not (Is-Object ([long]$a))) { continue }
    $b = [M2]::U64([long]$a + 0x58)
    if (-not (Is-Object ([long]$b))) { continue }
    $c = [M2]::U64([long]$b + 0x58)
    if (-not (Is-Object ([long]$c))) { continue }
    # B+0x58 must also answer at the camera and actor hops
    $cam = [M2]::U64([long]$c + 0x238)
    $act = [M2]::U64([long]$c + 0x288)
    $camOk = (Is-Object ([long]$cam))
    $actOk = (Is-Object ([long]$act))
    if (-not ($camOk -or $actOk)) { continue }
    $hits.Add([pscustomobject]@{
      Slot = $off + $i
      A = [long]$a; B = [long]$b; C = [long]$c
      Cam = [long]$cam; Act = [long]$act
      CamOk = $camOk; ActOk = $actOk
    })
  }
}

$out.Add("bytes scanned : $($scanned / 1MB) MB")
$out.Add("chain hits    : $($hits.Count)")
$out.Add("")

foreach ($h in $hits | Select-Object -First 25) {
  $out.Add("slot 0x$('{0:X}' -f $h.Slot)   (rva 0x$('{0:X}' -f ($h.Slot - $base)))")
  $out.Add("    A            0x$('{0:X16}' -f $h.A)")
  $out.Add("    A+0x58   -> B 0x$('{0:X16}' -f $h.B)")
  $out.Add("    B+0x58   -> C 0x$('{0:X16}' -f $h.C)")
  $out.Add("    C+0x238  -> Camera 0x$('{0:X16}' -f $h.Cam)  object=$($h.CamOk)")
  $out.Add("    C+0x288  -> Actor  0x$('{0:X16}' -f $h.Act)  object=$($h.ActOk)")
  $out.Add("")
}

$out | Set-Content -LiteralPath (Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'out\chainhunt.txt') -Encoding utf8
$out | Write-Output
