#!/usr/bin/env node
// -----------------------------------------------------------------------------
//  pe_scan.js  --  static triage for an Unreal Engine shipping binary.
//
//  Usage:
//      node pe_scan.js <exe> [--strings] [--refs N] [--dump NAME]
//
//  Why this exists
//  ---------------
//  Blood Strike ships as a 141 MB native UE5 executable with no IL2CPP and no
//  public SDK dump.  Every offset the client needs (GWorld, FNamePool,
//  GUObjectArray, GEngine, the WorldContext chain) has to come out of the
//  binary itself.  Guessing them is how people waste an evening; measuring them
//  is how it gets done in ten minutes.
//
//  What it does
//  ------------
//    1. parses the PE properly (sections, image base, entry point, dirs)
//    2. identifies the engine generation (UE4 vs UE5) from build strings
//    3. builds a census of RIP-relative references from .text -- a global that
//       400 different functions touch is GWorld, not a debug string
//    4. matches known UE accessors and reports the .data address each one
//       dereferences
//    5. greps for engine/runtime strings that the runtime prober will need
//
//  Nothing here is Blood Strike specific: point it at any UE title.
// -----------------------------------------------------------------------------
'use strict';

const fs = require('fs');
const path = require('path');

// ------------------------------------------------------------------ helpers
function die(msg) {
  console.error('[pe_scan] ' + msg);
  process.exit(1);
}

const IMAGE_SCN_MEM_EXECUTE = 0x20000000;
const IMAGE_SCN_MEM_READ    = 0x40000000;
const IMAGE_SCN_MEM_WRITE   = 0x80000000;

function parsePE(buf) {
  if (buf.length < 0x40 || buf.readUInt16LE(0) !== 0x5a4d) die('not an MZ image');

  const lfanew = buf.readUInt32LE(0x3c);
  if (lfanew + 0x18 > buf.length) die('truncated PE header');
  if (buf.readUInt32LE(lfanew) !== 0x00004550) die('bad PE signature');

  const coff = lfanew + 4;
  const machine           = buf.readUInt16LE(coff + 0);
  const numberOfSections  = buf.readUInt16LE(coff + 2);
  const timeDateStamp     = buf.readUInt32LE(coff + 4);
  const sizeOfOptionalHdr = buf.readUInt16LE(coff + 16);
  const characteristics  = buf.readUInt16LE(coff + 18);

  const opt = coff + 20;
  const magic = buf.readUInt16LE(opt);
  if (magic !== 0x20b) die('expected PE32+ (x64), got 0x' + magic.toString(16));

  const imageBase      = buf.readBigUInt64LE(opt + 24);
  const sectionAlign   = buf.readUInt32LE(opt + 32);
  const fileAlign      = buf.readUInt32LE(opt + 36);
  const sizeOfImage    = buf.readUInt32LE(opt + 56);
  const sizeOfHeaders  = buf.readUInt32LE(opt + 60);
  const subsystem      = buf.readUInt16LE(opt + 68);
  const dllChars       = buf.readUInt16LE(opt + 70);
  const numRvaAndSizes = buf.readUInt32LE(opt + 108);

  const dirs = [];
  const dirBase = opt + 112;
  for (let i = 0; i < numRvaAndSizes && i < 16; i++) {
    dirs.push({
      rva:  buf.readUInt32LE(dirBase + i * 8),
      size: buf.readUInt32LE(dirBase + i * 8 + 4),
    });
  }

  // IMAGE_SECTION_HEADER is exactly 40 bytes:
  //   +0  Name[8]              +20 PointerToRelocations
  //   +8  VirtualSize          +24 PointerToLinenumbers
  //   +12 VirtualAddress       +28 NumberOfRelocations  (WORD)
  //   +16 SizeOfRawData        +30 NumberOfLinenumbers   (WORD)
  //   +20 PointerToRawData     +36 Characteristics
  const sections = [];
  const secBase = opt + sizeOfOptionalHdr;
  for (let i = 0; i < numberOfSections; i++) {
    const o = secBase + i * 40;
    const nameRaw = buf.subarray(o, o + 8);
    const name = nameRaw.toString('latin1').replace(/\0+$/, '');
    sections.push({
      index: i,
      name: name.length ? name : ('<unnamed' + i + '>'),
      virtualSize:      buf.readUInt32LE(o + 8),
      virtualAddress:   buf.readUInt32LE(o + 12),
      sizeOfRawData:    buf.readUInt32LE(o + 16),
      pointerToRawData: buf.readUInt32LE(o + 20),
      characteristics:  buf.readUInt32LE(o + 36),
    });
  }

  const byRva = (rva) => {
    for (const s of sections) {
      const span = Math.max(s.virtualSize, s.sizeOfRawData);
      if (rva >= s.virtualAddress && rva < s.virtualAddress + span) return s;
    }
    return null;
  };

  const rvaToOffset = (rva) => {
    if (rva < sizeOfHeaders) return rva;
    const s = byRva(rva);
    if (!s) return -1;
    return s.pointerToRawData + (rva - s.virtualAddress);
  };

  return {
    machine, numberOfSections, timeDateStamp, characteristics,
    magic, imageBase, sectionAlign, fileAlign, sizeOfImage, sizeOfHeaders,
    subsystem, dllChars, dirs, sections, byRva, rvaToOffset,
    entryPoint: buf.readUInt32LE(opt + 16),
  };
}

// ------------------------------------------------------- string extraction
function findAllStrings(buf, minLen, limit) {
  const out = [];
  const ascii = /[\x20-\x7e]{%d,}/g;
  const utf16 = /([\x20-\x7e]\x00){%d,}/g;
  const re1 = new RegExp(ascii.source.replace('%d', minLen), 'g');
  const re2 = new RegExp(utf16.source.replace('%d', minLen), 'g');

  let m;
  while ((m = re1.exec(buf)) !== null) {
    if (out.length >= limit) break;
    out.push({ off: m.index, str: m[0] });
  }
  while ((m = re2.exec(buf)) !== null && out.length < limit) {
    out.push({ off: m.index, str: m[0].replace(/\0/g, '') });
  }
  return out;
}

// --------------------------------------------- RIP-relative reference census
//
//  Decoding every x86 instruction properly is out of scope here.  Instead we
//  match the shapes that actually carry a RIP-relative memory operand:
//      [REX] [prefix] opcode ModRM(00,101) disp32
//  A handful of false positives survive, which is harmless: the census only
//  has to rank candidates, and the runtime prober does the invariant checking.
const MEM_OPCODES = new Set([
  0x00, 0x01, 0x02, 0x03, 0x08, 0x09, 0x0a, 0x0b,
  0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1a, 0x1b,
  0x20, 0x21, 0x22, 0x23, 0x28, 0x29, 0x2a, 0x2b,
  0x30, 0x31, 0x32, 0x33, 0x38, 0x39, 0x3a, 0x3b,
  0x62, 0x63,                     // bound / movsxd
  0x80, 0x81, 0x83,               // group1 rm
  0x84, 0x85,                     // test
  0x86, 0x87,
  0x88, 0x89, 0x8a, 0x8b,
  0x8d,                           // lea
  0xc6, 0xc7,                     // mov rm,imm
  0xd1, 0xd3,
  0xf6, 0xf7,                     // group3
  0xfe, 0xff,                     // group4/5
]);

function ripCensus(pe, buf) {
  const census = new Map();          // rva -> count
  const sites = new Map();           // rva -> first few code RVAs

  for (const sec of pe.sections) {
    if (!(sec.characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    const start = sec.pointerToRawData;
    const end   = start + sec.sizeOfRawData;
    if (end > buf.length) continue;

    for (let i = start; i + 8 <= end; i++) {
      let p = i;
      const b0 = buf[p];

      // REX
      if (b0 >= 0x40 && b0 <= 0x4f) p++;
      // legacy prefixes we care about
      for (;;) {
        const b = buf[p];
        if (b === 0x66 || b === 0x67 || b === 0x64 || b === 0x65 ||
            b === 0xf2 || b === 0xf3) { p++; continue; }
        break;
      }
      const op = buf[p];
      if (!MEM_OPCODES.has(op)) continue;
      const modrm = buf[p + 1];
      if (modrm === undefined) continue;

      const mod = modrm >> 6;
      const rm  = modrm & 7;
      if (mod !== 0 || rm !== 5) continue;        // need [rip+disp32]

      const disp = buf.readInt32LE(p + 2);
      const instrLen = (p + 6) - i;
      const nextRva = sec.virtualAddress + (p + 6 - start);
      const targetRva = (nextRva + disp) >>> 0;

      census.set(targetRva, (census.get(targetRva) || 0) + 1);
      if (!sites.has(targetRva)) sites.set(targetRva, []);
      const s = sites.get(targetRva);
      if (s.length < 6) s.push(sec.virtualAddress + (i - start));
    }
  }
  return { census, sites };
}

// --------------------------------------------------- targeted UE signatures
//
//  Each signature returns the RVA of the .data/.rdata global it dereferences.
const SIGS = [
  {
    name: 'GWorld  (mov rax,[rip+G]; mov rcx,[rax+off])',
    bytes: [0x48, 0x8b, 0x05, '????', '????', '????', 0x48, 0x8b, 0x88, '????', '????', '????'],
    kind: 'global',
  },
  {
    name: 'GWorld  (mov rax,[rip+G]; mov rdx,[rax+off])',
    bytes: [0x48, 0x8b, 0x05, '????', '????', '????', 0x48, 0x8b, 0x90, '????', '????', '????'],
    kind: 'global',
  },
  {
    name: 'GEngine (mov rax,[rip+G]; mov rcx,[rax+off])',
    bytes: [0x48, 0x8b, 0x05, '????', '????', '????', 0x48, 0x8b, 0x88, '????', '????', '????'],
    kind: 'global',
  },
  {
    name: 'GUObjectArray (mov rax,[rip+G]; mov rcx,[rax+rax])',
    bytes: [0x48, 0x8b, 0x05, '????', '????', '????', 0x48, 0x8b, 0x0c, 0x24],
    kind: 'global',
  },
  {
    name: 'store to global (mov [rip+G], rax/rcx)',
    bytes: [0x48, 0x89, 0x05, '????', '????', '????'],
    kind: 'global',
    store: true,
  },
];

function scanSignatures(pe, buf, limitPer) {
  const results = [];
  for (const sig of SIGS) {
    const hits = [];
    const text = pe.sections.find(s => s.characteristics & IMAGE_SCN_MEM_EXECUTE);
    if (!text) continue;

    const start = text.pointerToRawData;
    const end   = start + text.sizeOfRawData;
    const first = sig.bytes[0];

    for (let i = start; i + sig.bytes.length <= end; i++) {
      if (buf[i] !== first) continue;
      let ok = true;
      let dispAt = -1;
      for (let k = 0; k < sig.bytes.length; k++) {
        const want = sig.bytes[k];
        const have = buf[i + k];
        if (want === '????') {
          if (k < sig.bytes.length && sig.bytes[k + 1] !== undefined &&
              want === '????' && k === 3 && first !== 0x48) { /* noop */ }
          continue;
        }
        if (have !== want) { ok = false; break; }
        if (sig.kind === 'global' && !sig.store && k === 3) dispAt = i + k;
      }
      if (!ok || dispAt < 0) continue;

      const disp = buf.readInt32LE(dispAt);
      const instrEndRva = text.virtualAddress + (dispAt + 4 - start);
      const targetRva = (instrEndRva + disp) >>> 0;

      const sec = pe.byRva(targetRva);
      hits.push({
        codeRva: text.virtualAddress + (i - start),
        targetRva,
        targetSection: sec ? sec.name : '<none>',
      });
      if (hits.length >= limitPer) break;
    }
    if (hits.length) results.push({ sig: sig.name, hits });
  }
  return results;
}

// --------------------------------------------------------------------- main
function main() {
  const argv = process.argv.slice(2);
  const file = argv.find(a => !a.startsWith('--'));
  if (!file) die('usage: node pe_scan.js <exe> [--refs N] [--strings]');

  const wantRefs    = argv.includes('--refs') || true;
  const refLimit    = parseInt((argv.indexOf('--refs') >= 0) ? argv[argv.indexOf('--refs') + 1] : '40', 10);
  const wantStrings = argv.includes('--strings');
  const strFilter   = argv.includes('--grep') ? argv[argv.indexOf('--grep') + 1] : null;

  if (!fs.existsSync(file)) die('no such file: ' + file);
  const buf = fs.readFileSync(file);
  const pe  = parsePE(buf);

  console.log('='.repeat(78));
  console.log('  ' + path.basename(file));
  console.log('='.repeat(78));
  console.log('  size            : ' + (buf.length / 1048576).toFixed(2) + ' MB');
  console.log('  machine         : 0x' + pe.machine.toString(16) + (pe.machine === 0x8664 ? ' (x64)' : ''));
  console.log('  build           : ' + new Date(pe.timeDateStamp * 1000).toISOString());
  console.log('  image base      : 0x' + pe.imageBase.toString(16));
  console.log('  entry point RVA : 0x' + pe.entryPoint.toString(16));
  console.log('  size of image   : 0x' + pe.sizeOfImage.toString(16));
  console.log('  subsystem       : ' + pe.subsystem + (pe.subsystem === 2 ? ' (GUI)' : ''));
  console.log('  characteristics : 0x' + pe.characteristics.toString(16) +
              (pe.characteristics & 0x20 ? ' [RELOCS_STRIPPED]' : ' [RELOCS]'));
  console.log('  dll chars       : 0x' + pe.dllChars.toString(16) +
              (pe.dllChars & 0x0040 ? ' [DYNAMIC_BASE]' : '') +
              (pe.dllChars & 0x0100 ? ' [NX]' : '') +
              (pe.dllChars & 0x0400 ? ' [NO_SEH]' : '') +
              (pe.dllChars & 0x4000 ? ' [GUARD_CF]' : ''));
  console.log('');

  console.log('  sections');
  console.log('  ' + '-'.repeat(74));
  console.log('  ' + 'name'.padEnd(14) + 'rva'.padStart(12) + 'vsize'.padStart(12) +
              'raw'.padStart(12) + 'flags');
  for (const s of pe.sections) {
    const flags = (s.characteristics & IMAGE_SCN_MEM_READ ? 'r' : '-') +
                  (s.characteristics & IMAGE_SCN_MEM_WRITE ? 'w' : '-') +
                  (s.characteristics & IMAGE_SCN_MEM_EXECUTE ? 'x' : '-');
    console.log('  ' + s.name.padEnd(14) +
                ('0x' + s.virtualAddress.toString(16)).padStart(12) +
                ('0x' + s.virtualSize.toString(16)).padStart(12) +
                ('0x' + s.sizeOfRawData.toString(16)).padStart(12) + '   ' + flags);
  }
  console.log('');

  // ---- engine generation ------------------------------------------------
  const markers = [
    ['++UE5', 0], ['UE5+Release', 0], ['UE4+Release', 0],
    ['Engine/Binaries/ThirdParty', 0], ['Engine/Source/Runtime/Engine', 0],
    ['/Script/Engine', 0], ['__Default__', 0],
  ];
  const probe = Buffer.from(markers.map(m => m[0]).join('\x00'), 'latin1');
  console.log('  engine markers');
  console.log('  ' + '-'.repeat(74));
  for (const m of markers) {
    const needle = Buffer.from(m[0], 'latin1');
    const idx = buf.indexOf(needle);
    console.log('  ' + m[0].padEnd(32) + (idx >= 0 ? 'found @ 0x' + idx.toString(16) : '-'));
  }
  // UE version literal: "++UE5+Release-5.x-CL-xxxx" or similar
  {
    const re = /\+\+UE[45]\+Release-[0-9]+[^\x00]{0,80}/g;
    const seen = new Set();
    let mm;
    while ((mm = re.exec(buf.toString('latin1'))) !== null && seen.size < 6) {
      seen.add(mm[0].replace(/[^\x20-\x7e]/g, ''));
    }
    if (seen.size) { console.log('  version strings:'); for (const s of seen) console.log('    ' + s); }
  }
  console.log('');

  // ---- reference census -------------------------------------------------
  const t0 = Date.now();
  const { census, sites } = ripCensus(pe, buf);
  const ranked = [...census.entries()]
    .filter(([rva]) => {
      const s = pe.byRva(rva);
      return s && (s.characteristics & IMAGE_SCN_MEM_WRITE) !== 0;
    })
    .sort((a, b) => b[1] - a[1])
    .slice(0, refLimit);

  if (wantRefs) {
    console.log('  most-referenced mutable globals  (RIP-relative census, ' +
                ((Date.now() - t0) / 1000).toFixed(1) + 's)');
    console.log('  ' + '-'.repeat(74));
    console.log('  ' + 'rva'.padStart(12) + 'refs'.padStart(7) + '  section   first referencing sites');
    for (const [rva, n] of ranked) {
      const s = pe.byRva(rva);
      const first = (sites.get(rva) || []).slice(0, 3)
        .map(r => '0x' + r.toString(16)).join(' ');
      console.log('  ' + ('0x' + rva.toString(16)).padStart(12) +
                  String(n).padStart(7) + '  ' + s.name.padEnd(9) + first);
    }
    console.log('');
  }

  // ---- targeted signatures --------------------------------------------
  const sigs = scanSignatures(pe, buf, 12);
  console.log('  UE accessor signatures');
  console.log('  ' + '-'.repeat(74));
  if (!sigs.length) console.log('  (none matched)');
  for (const s of sigs) {
    console.log('  ' + s.sig + '   [' + s.hits.length + ' hits shown]');
    for (const h of s.hits) {
      console.log('      code 0x' + h.codeRva.toString(16).padStart(8) +
                  '  ->  global 0x' + h.targetRva.toString(16) +
                  '  (' + h.targetSection + ')');
    }
  }
  console.log('');

  // ---- strings ----------------------------------------------------------
  if (wantStrings) {
    const pat = strFilter
      ? new RegExp(strFilter, 'i')
      : /(Health|Team|PlayerState|CameraCache|AntiCheat|Cheat|Shield|Actor|Pawn|Controller|Weapon|Recoil|Spread|Bone|NetEase|Hyper)/;
    const strings = findAllStrings(buf, 6, 4_000_000);
    console.log('  matching strings  (pattern: ' + pat.source + ')');
    console.log('  ' + '-'.repeat(74));
    let shown = 0;
    for (const s of strings) {
      if (!pat.test(s.str)) continue;
      console.log('    0x' + s.off.toString(16).padStart(8) + '  ' + s.str.slice(0, 110));
      if (++shown >= 220) { console.log('    ... (truncated)'); break; }
    }
    console.log('');
  }
}

main();
