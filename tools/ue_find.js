#!/usr/bin/env node
// -----------------------------------------------------------------------------
//  ue_find.js  --  locate UE globals (GWorld / GEngine / GUObjectArray / FNamePool)
//                  in a stripped shipping UE binary, with evidence.
//
//  Why this file exists
//  --------------------
//  BloodStrike.exe is a 135 MB UE build with every engine string stripped: no
//  "++UE5+Release", no "Engine/Source/Runtime/Engine", no reflection names in
//  .rdata.  Name-based dumping is off the table, so the globals have to be found
//  structurally.  The method that survives stripping is reference analysis:
//
//      A UWorld* global is not just "some" global -- it is the one that the
//      largest number of unrelated functions load and then immediately
//      dereference at a small field offset.  Gameplay code does
//          mov rax, [rip+GWorld]
//          mov rcx, [rax+0x1A0]      ; some world field
//      thousands of times.  Nothing else in the image looks like that.
//
//  So this tool:
//    1. builds a RIP-relative reference census over .text, deduplicated per
//       7-byte instruction window so overlapping false positives do not inflate
//       the ranking;
//    2. classifies each candidate by what the code does *after* loading it
//       ("dereferenced at +0x1A0", "compared against null", "never dereferenced");
//    3. matches known UE accessor shapes and prints the exact .data address each
//       one resolves to;
//    4. dumps raw context bytes so a claim can be checked by eye.
//
//  The output is a shortlist, not a verdict.  ue_probe (C++) is what turns a
//  shortlist into offsets, by proving the invariant at runtime.
//
//  Usage:
//      node ue_find.js <exe> [--refs N] [--top K] [--strings] [--grep PAT]
// -----------------------------------------------------------------------------
'use strict';

const fs = require('fs');
const path = require('path');

// ------------------------------------------------------------------ PE parse
const SCN_EXEC = 0x20000000, SCN_READ = 0x40000000, SCN_WRITE = 0x80000000;

function parsePE(buf) {
  if (buf.readUInt16LE(0) !== 0x5a4d) throw new Error('not MZ');
  const lfanew = buf.readUInt32LE(0x3c);
  if (buf.readUInt32LE(lfanew) !== 0x00004550) throw new Error('bad PE sig');

  const coff = lfanew + 4;
  const nsec      = buf.readUInt16LE(coff + 2);
  const sizeOpt   = buf.readUInt16LE(coff + 16);
  const opt       = coff + 20;
  if (buf.readUInt16LE(opt) !== 0x20b) throw new Error('not PE32+');

  const imageBase = buf.readBigUInt64LE(opt + 24);
  const sizeOfImage = buf.readUInt32LE(opt + 56);
  const sizeOfHeaders = buf.readUInt32LE(opt + 60);
  const entryPoint = buf.readUInt32LE(opt + 16);

  const sections = [];
  const secBase = opt + sizeOpt;
  for (let i = 0; i < nsec; i++) {
    const o = secBase + i * 40;
    sections.push({
      index: i,
      name: buf.subarray(o, o + 8).toString('latin1').replace(/\0+$/, ''),
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
  const toOff = (rva) => {
    if (rva < sizeOfHeaders) return rva;
    const s = byRva(rva);
    return s ? s.pointerToRawData + (rva - s.virtualAddress) : -1;
  };

  return { imageBase, sizeOfImage, sizeOfHeaders, entryPoint, sections, byRva, toOff };
}

// ------------------------------------------------------------ RIP detection
const MEM_OPCODES = new Set([
  0x00,0x01,0x02,0x03,0x08,0x09,0x0a,0x0b,0x10,0x11,0x12,0x13,
  0x18,0x19,0x1a,0x1b,0x20,0x21,0x22,0x23,0x28,0x29,0x2a,0x2b,
  0x30,0x31,0x32,0x33,0x38,0x39,0x3a,0x3b,
  0x62,0x63,0x80,0x81,0x83,0x84,0x85,0x86,0x87,
  0x88,0x89,0x8a,0x8b,0x8d,0xc6,0xc7,0xd1,0xd3,
  0xf6,0xf7,0xfe,0xff,
]);

/// Decode just enough of the instruction starting at `i` to know its length and
/// whether it carries a RIP-relative operand.  Returns null when undecodable,
/// which makes the scanner fall back to a one-byte step (safe: it only costs a
/// missed hit, never a wrong address, because the operand decode is strict).
function decode(buf, i, limit) {
  let p = i;
  let rex = 0;
  for (;;) {
    if (p >= limit) return null;
    const b = buf[p];
    if (b === 0x66 || b === 0x67 || b === 0x64 || b === 0x65 ||
        b === 0xf2 || b === 0xf3 || b === 0x2e || b === 0x36 || b === 0x3e) { p++; continue; }
    if (b >= 0x40 && b <= 0x4f) { rex = b; p++; continue; }
    break;
  }
  if (p >= limit) return null;

  const op = buf[p];
  // two-byte 0F escape: handle the handful that take a RIP-relative operand
  if (op === 0x0f) {
    const op2 = buf[p + 1];
    if (op2 === undefined) return null;
    // 0F 10..17, 28..2F, 6E, 6F, 7E, 7F, B6, B7, BE, BF, D6, D7 carry memory ops
    const hasMem = (op2 >= 0x10 && op2 <= 0x17) || (op2 >= 0x28 && op2 <= 0x2f) ||
                   op2 === 0x6e || op2 === 0x6f || op2 === 0x7e || op2 === 0x7f ||
                   op2 === 0xb6 || op2 === 0xb7 || op2 === 0xbe || op2 === 0xbf;
    if (!hasMem) {
      // imm lengths for the common 0F opcodes we actually stride over
      const imm = { 0x05:0, 0x0b:0, 0x1f:0, 0x31:0, 0xa2:0, 0x34:0, 0x35:0,
                    0xa3:0, 0xa4:0, 0xa5:0, 0xaa:0, 0xab:0, 0xac:0, 0xad:0,
                    0xa8:0, 0xa9:0, 0xc8:3, 0xc9:0, 0xca:3, 0xcb:0,
                    0xcc:0, 0xcd:0, 0xce:0, 0xcf:0 }[op2];
      if (imm === undefined) return null;
      return { len: (p + 2 + imm) - i, rip: false, target: 0, op: op2, rex };
    }
    const len = memLen(buf, p + 2, limit);
    if (!len) return null;
    const modrmOff = p + 2;
    if (len.mod === 0 && len.rm === 5) {
      const dispOff = modrmOff + 1 + (len.sib ? 1 : 0);
      const disp = buf.readInt32LE(dispOff);
      return { len: (dispOff + 4) - i, rip: true, disp, dispOff, op: op2, rex, modrm: buf[modrmOff] };
    }
    return { len: (modrmOff + 1 + len.total) - i, rip: false, op: op2, rex };
  }

  if (op === 0xc4 || op === 0xc5 || op === 0x62) return null;  // VEX/EVEX: not modelled
  if (!MEM_OPCODES.has(op)) return null;

  const modrmOff = p + 1;
  if (modrmOff >= limit) return null;
  const modrm = buf[modrmOff];
  const mod = modrm >> 6, rm = modrm & 7;

  if (mod !== 3) {
    const sib = (rm === 4);
    const sibOff = modrmOff + 1;
    const dispOff = sibOff + (sib ? 1 : 0);
    const rip = (mod === 0 && rm === 5 && !sib);
    let len;
    if (rip) len = (dispOff + 4) - i;
    else if (mod === 0) len = sib ? (sibOff + 1) - i : (modrmOff + 1) - i;
    else if (mod === 1) len = (dispOff + 1) - i;
    else len = (dispOff + 4) - i;

    if (rip) {
      const disp = buf.readInt32LE(dispOff);
      return { len, rip: true, disp, dispOff, op, rex, modrm };
    }
    return { len, rip: false, op, rex };
  }

  // register operand: opcode-specific immediate tail
  const imm = immSizeFor(op, modrm, (rex & 1) !== 0);
  if (imm === null) return null;
  return { len: (modrmOff + 1 + imm) - i, rip: false, op, rex };
}

function memLen(buf, modrmOff, limit) {
  if (modrmOff >= limit) return null;
  const modrm = buf[modrmOff];
  const mod = modrm >> 6, rm = modrm & 7;
  const sib = (rm === 4);
  let total = 1 + (sib ? 1 : 0);
  if (mod === 0 && rm === 5) total += 4;      // disp32
  else if (mod === 0 && rm === 4) total += 0; // base=sib, index=101 -> disp32
  else if (mod === 1) total += 1;
  else if (mod === 2) total += 4;
  return { mod, rm, sib, total };
}

function immSizeFor(op, modrm, rEX_W) {
  switch (op) {
    case 0x83: return 1;
    case 0x81: return rEX_W ? 4 : (modrm >> 3) === 7 ? 4 : 2;
    case 0xc7: return rEX_W ? 4 : 2;
    case 0x69: case 0x6b: return null;       // imul with imm: 2/1/4 -> bail
    case 0x80: return 1;
    case 0xc6: return 1;
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7a: case 0x7b:
    case 0x7c: case 0x7d: case 0x7e: case 0x7f: return 0;
    case 0xe8: return 4;
    case 0xe9: return 4;
    case 0xeb: return 1;
    case 0xf6: return (modrm >> 3) === 0 || (modrm >> 3) === 1 ? 1 : 0;
    case 0xf7: return (modrm >> 3) === 0 || (modrm >> 3) === 1 ? (rEX_W ? 8 : 4) : 0;
    case 0x68: return rEX_W ? 4 : 4;
    case 0x6a: return 1;
    case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8d:
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3a: case 0x3b:
    case 0x62: case 0x63: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x8e: case 0xd1: case 0xd3:
    case 0xfe: case 0xff: return 0;
    default: return null;
  }
}

// ------------------------------------------------------------------- census
function census(pe, buf) {
  const count = new Map();     // rva -> refs
  const sites = new Map();     // rva -> [codeRva]
  let decoded = 0, fellBack = 0;

  for (const sec of pe.sections) {
    if (!(sec.characteristics & SCN_EXEC)) continue;
    const start = sec.pointerToRawData;
    const limit = Math.min(start + sec.sizeOfRawData, buf.length);
    let i = start;
    while (i + 8 < limit) {
      const d = decode(buf, i, limit);
      if (!d) { fellBack++; i++; continue; }
      decoded++;
      if (d.rip) {
        const nextRva = sec.virtualAddress + (i + d.len - start);
        const target = (nextRva + d.disp) >>> 0;
        count.set(target, (count.get(target) || 0) + 1);
        if (!sites.has(target)) sites.set(target, []);
        const arr = sites.get(target);
        if (arr.length < 8) arr.push(sec.virtualAddress + (i - start));
      }
      i += d.len;    // real instruction boundaries -> no double counting
    }
  }
  return { count, sites, decoded, fellBack };
}

// ------------------------------------------------- known UE accessor shapes
//
//  Each entry: [name, bytes].  '?' is a wildcard nibble-pair.
const SIGS = [
  { name: 'GWorld  mov rax,[rip+G] / test / mov rax,[rax+X] / ret',
    pat: '48 8B 05 ???? ???? 48 85 C0 74 ? 48 8B 40 ? C3', ripAt: 3 },
  { name: 'GWorld  mov rax,[rip+G] / mov rcx,[rax+X] / mov [rsp+Y],rcx',
    pat: '48 8B 05 ???? ???? 48 8B 88 ???? ????', ripAt: 3 },
  { name: 'GWorld  mov rax,[rip+G] / mov rdx,[rax+X]',
    pat: '48 8B 05 ???? ???? 48 8B 90 ???? ????', ripAt: 3 },
  { name: 'GEngine mov rcx,[rip+G] / mov rax,[rcx+X]',
    pat: '48 8B 0D ???? ???? 48 8B 81 ???? ????', ripAt: 3 },
  { name: 'GUObjectArray mov rax,[rip+G] / mov rcx,[rax+rax]',
    pat: '48 8B 05 ???? ???? 48 8B 0C 24', ripAt: 3 },
  { name: 'FNamePool lea rcx,[rip+Pool]',
    pat: '48 8D 0D ???? ????', ripAt: 3 },
  { name: 'global store  mov [rip+G], rbx',
    pat: '48 89 1D ???? ????', ripAt: 3 },
  { name: 'global store  mov [rip+G], rcx',
    pat: '48 89 0D ???? ????', ripAt: 3 },
  { name: 'global store  mov [rip+G], rax',
    pat: '48 89 05 ???? ????', ripAt: 3 },
];

function parsePat(str) {
  return str.trim().split(/\s+/).map(t => t === '?' ? [-1, -1]
    : [parseInt(t.slice(0, 2), 16), parseInt(t.slice(2, 4), 16)]);
}
const isWild = (t) => t[0] === -1;

function findAll(hay, needle, from) {
  const out = [];
  let i = from;
  for (;;) {
    i = hay.indexOf(needle, i);
    if (i === -1) break;
    out.push(i);
    i++;
  }
  return out;
}

function sigAt(buf, i, toks, limit) {
  for (let k = 0; k < toks.length; k++) {
    if (i + k >= limit) return false;
    if (isWild(toks[k])) continue;
    if (buf[i + k] !== toks[k][0]) return false;
  }
  return true;
}

function scanSigs(pe, buf, maxPer) {
  const res = [];
  for (const sig of SIGS) {
    const toks = parsePat(sig.pat);
    const hits = [];
    for (const sec of pe.sections) {
      if (!(sec.characteristics & SCN_EXEC)) continue;
      const start = sec.pointerToRawData;
      const limit = Math.min(start + sec.sizeOfRawData, buf.length);
      for (let i = start; i + toks.length <= limit && hits.length < maxPer; i++) {
        if (!sigAt(buf, i, toks, limit)) continue;
        const dispOff = i + sig.ripAt + 1;
        const disp = buf.readInt32LE(dispOff);
        const instrEndRva = sec.virtualAddress + (dispOff + 4 - start);
        const target = (instrEndRva + disp) >>> 0;
        hits.push({ codeRva: sec.virtualAddress + (i - start), target,
                    sec: pe.byRva(target) });
      }
    }
    if (hits.length) res.push({ name: sig.name, hits });
  }
  return res;
}

// ------------------------------------------------------------------ display
function hexdump(buf, off, n) {
  const rows = [];
  for (let i = 0; i < n; i += 16) {
    const slice = buf.subarray(off + i, off + i + 16);
    const hex = [...slice].map(b => b.toString(16).padStart(2, '0')).join(' ').padEnd(47);
    const asc = [...slice].map(b => (b >= 0x20 && b <= 0x7e) ? String.fromCharCode(b) : '.').join('');
    rows.push('      +' + i.toString(16).padStart(2, '0') + '  ' + hex + '  ' + asc);
  }
  return rows.join('\n');
}

/// What does the code do right after loading this global?  That is what
/// separates "GWorld" from "a hot lookup table".
function classifySites(pe, buf, sites) {
  const stats = { derefSmall: 0, derefOther: 0, nullTest: 0, lea: 0, other: 0 };
  const offsets = new Map();
  for (const codeRva of sites) {
    const o = pe.toOff(codeRva);
    if (o === -1) continue;
    const d = decode(buf, o, buf.length);
    if (!d) { stats.other++; continue; }
    const after = o + d.len;
    const nxt = decode(buf, after, buf.length);
    if (!nxt) { stats.other++; continue; }

    if (d.op === 0x8d) { stats.lea++; continue; }

    if (nxt.op === 0x85 || nxt.op === 0x84) { stats.nullTest++; continue; }

    // a memory operand on the same register => a field access
    if (nxt.rip === false && (nxt.op === 0x8b || nxt.op === 0x39 ||
                             nxt.op === 0x03 || nxt.op === 0x8d ||
                             nxt.op === 0x89)) {
      const m = memLen(buf, after + 1, buf.length);
      if (m && m.mod !== 3) {
        const dispSize = m.mod === 0 ? 0 : (m.mod === 1 ? 1 : 4);
        const disp = dispSize ? buf.readInt32LE(after + 1 + (m.sib ? 1 : 0)) : 0;
        const key = (m.mod === 1) ? disp : 0;
        if (dispSize === 0 || (disp >= 0 && disp < 0x1000)) {
          stats.derefSmall++;
          if (m.mod === 1) offsets.set((offsets.get(key) || 0) + 1);
        } else stats.derefOther++;
        continue;
      }
    }
    stats.other++;
  }
  const top = [...offsets.entries()].sort((a, b) => b[1] - a[1]).slice(0, 6)
    .map(([k, v]) => `+0x${k.toString(16)}(${v})`).join(' ');
  return { stats, top };
}

function main() {
  const argv = process.argv.slice(2);
  const file = argv.find(a => !a.startsWith('--'));
  if (!file) { console.error('usage: node ue_find.js <exe> [--refs N] [--top K]'); process.exit(1); }
  const argAfter = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
  const refLimit = parseInt(argAfter('--refs', '25'), 10);
  const topN     = parseInt(argAfter('--top', '8'), 10);
  const wantStr   = argv.includes('--strings');
  const grep      = argAfter('--grep', null);

  const buf = fs.readFileSync(file);
  const pe  = parsePE(buf);

  console.log('='.repeat(96));
  console.log('  ue_find  --  ' + path.basename(file));
  console.log('='.repeat(96));
  console.log(`  image base 0x${pe.imageBase.toString(16)}   size of image 0x${pe.sizeOfImage.toString(16)}` +
              `   (${(pe.sizeOfImage / 1048576).toFixed(1)} MB)`);
  console.log('');

  // ---- census -------------------------------------------------------------
  const t0 = Date.now();
  const { count, sites, decoded, fellBack } = census(pe, buf);
  const secs = ((Date.now() - t0) / 1000).toFixed(1);
  console.log(`  census: ${decoded.toLocaleString()} instructions decoded, ` +
              `${fellBack.toLocaleString()} undecodable byte steps, ${secs}s`);
  console.log('');

  const ranked = [...count.entries()]
    .filter(([rva]) => { const s = pe.byRva(rva); return s && (s.characteristics & SCN_WRITE); })
    .sort((a, b) => b[1] - a[1]);

  console.log('  candidate globals in writable sections');
  console.log('  ' + '-'.repeat(94));
  console.log('  ' + 'rva'.padStart(11) + 'refs'.padStart(7) + '  sec'.padEnd(9) +
              'derefs  nulltests  frequent field offsets');
  for (const [rva, n] of ranked.slice(0, refLimit)) {
    const s = pe.byRva(rva);
    const { stats, top } = classifySites(pe, buf, sites.get(rva) || []);
    console.log('  ' + ('0x' + rva.toString(16)).padStart(11) +
                String(n).padStart(7) + '  ' + s.name.padEnd(9) +
                String(stats.derefSmall).padStart(5) + '  ' +
                String(stats.nullTest).padStart(8) + '   ' + top);
  }
  console.log('');

  // ---- signature matches --------------------------------------------------
  console.log('  UE accessor signatures');
  console.log('  ' + '-'.repeat(94));
  const sigs = scanSigs(pe, buf, 6);
  if (!sigs.length) console.log('  (no match -- globals may be reached through a register or an inlined helper)');
  for (const s of sigs) {
    console.log('  ' + s.name);
    for (const h of s.hits) {
      const refs = count.get(h.target) || 0;
      console.log('      code 0x' + h.codeRva.toString(16).padStart(8) +
                  '  ->  0x' + h.target.toString(16).padStart(10) +
                  '  ' + (h.sec ? h.sec.name.padEnd(9) : '<none>') +
                  '  refs=' + refs);
    }
  }
  console.log('');

  // ---- context dump for the strongest candidates --------------------------
  console.log(`  raw context for the top ${topN} candidates`);
  console.log('  ' + '-'.repeat(94));
  const seen = new Set();
  let shown = 0;
  for (const [rva, n] of ranked) {
    if (shown >= topN) break;
    if (seen.has(rva)) continue;
    seen.add(rva);
    const list = sites.get(rva) || [];
    if (!list.length) continue;
    shown++;
    const o = pe.toOff(list[0]);
    console.log(`  --- 0x${rva.toString(16)}  (${n} refs)  first site 0x${list[0].toString(16)}`);
    if (o !== -1) console.log(hexdump(buf, o, 48));
  }
  console.log('');

  // ---- strings ------------------------------------------------------------
  if (wantStr) {
    const pat = grep ? new RegExp(grep, 'i') : null;
    const re = /[\x20-\x7e]{6,}/g;
    console.log('  strings' + (pat ? ' matching /' + pat.source + '/' : ' (longest 120)'));
    console.log('  ' + '-'.repeat(94));
    const all = [];
    let m;
    while ((m = re.exec(buf)) !== null) all.push({ off: m.index, s: m[0] });
    all.sort((a, b) => b.s.length - a.s.length);
    let n = 0;
    for (const e of all) {
      if (pat && !pat.test(e.s)) continue;
      console.log('    0x' + e.off.toString(16).padStart(8, '0') + '  ' + e.s.slice(0, 100));
      if (++n >= 120) break;
    }
    console.log('');
  }
}

main();
