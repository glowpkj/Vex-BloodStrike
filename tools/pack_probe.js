#!/usr/bin/env node
// -----------------------------------------------------------------------------
//  pack_probe.js  --  is this PE actually readable, or is it a protected image?
//
//  The PE header of BloodStrike.exe reports SizeOfRawData = 0 for every section
//  and carries PyRuntim / AsyncioD sections.  That combination has two very
//  different meanings, and guessing wrong costs a whole afternoon:
//
//      (a) the file is a normal image whose headers were doctored to defeat
//          static mappers  ->  the real bytes are still on disk, we can parse
//          them by scanning instead of trusting the header
//      (b) the file is a packed/encrypted loader  ->  the interesting code only
//          exists in memory after the loader unpacks, and static analysis of the
//          file is worthless
//
//  This distinguishes them with three cheap measurements:
//      1. byte histogram / entropy per 4 MB slice   (packed -> ~8.0 everywhere)
//      2. whether a plain .NET / embedded PE exists at some overlay offset
//      3. how many printable-ASCII runs survive in the whole file
//
//  Usage: node pack_probe.js <exe> [--detail]
// -----------------------------------------------------------------------------
'use strict';

const fs = require('fs');

function entropyOf(buf, start, end) {
  const hist = new Uint32Array(256);
  let n = 0;
  for (let i = start; i < end; i++) { hist[buf[i]]++; n++; }
  if (!n) return 0;
  let h = 0;
  for (let c = 0; c < 256; c++) {
    if (!hist[c]) continue;
    const p = hist[c] / n;
    h -= p * Math.log2(p);
  }
  return h;
}

function printableRuns(buf, minLen) {
  let count = 0, total = 0, longest = 0;
  let run = 0;
  for (let i = 0; i < buf.length; i++) {
    const b = buf[i];
    if (b >= 0x20 && b <= 0x7e) {
      run++;
    } else {
      if (run >= minLen) { count++; total += run; if (run > longest) longest = run; }
      run = 0;
    }
  }
  if (run >= minLen) { count++; total += run; if (run > longest) longest = run; }
  return { count, total, longest };
}

function findEmbeddedPE(buf) {
  const hits = [];
  const needle = Buffer.from([0x4d, 0x5a, 0x90, 0x00, 0x03, 0x00, 0x00, 0x00]);
  let off = 0;
  while ((off = buf.indexOf(needle, off)) !== -1 && hits.length < 12) {
    const lfanew = buf.readUInt32LE(off + 0x3c);
    let note = 'no PE header';
    if (off + lfanew + 4 < buf.length &&
        buf.readUInt32LE(off + lfanew) === 0x00004550) {
      const mach = buf.readUInt16LE(off + lfanew + 4);
      const chars = buf.readUInt16LE(off + lfanew + 4 + 18);
      note = 'valid PE, machine 0x' + mach.toString(16) +
             ', chars 0x' + chars.toString(16);
    }
    hits.push({ off, note });
    off += 2;
  }
  return hits;
}

// Common packer / protector markers, matched on raw bytes.
const PROTECTORS = [
  ['VMProtect',  Buffer.from('VMProtect', 'latin1')],
  ['Themida',    Buffer.from('Themida', 'latin1')],
  ['WinLicense', Buffer.from('WinLicense', 'latin1')],
  ['UPX',        Buffer.from('UPX!', 'latin1')],
  ['MPRESS',     Buffer.from('.MPRESS', 'latin1')],
  ['HyperShield',Buffer.from('HyperShield', 'latin1')],
  ['ACE',        Buffer.from('ACE-Base', 'latin1')],
  ['NEProtect',  Buffer.from('NEProtect', 'latin1')],
  ['NeacSafe',   Buffer.from('NeacSafe', 'latin1')],
  ['dobby',      Buffer.from('dobby', 'latin1')],
  ['Frida',      Buffer.from('frida', 'latin1')],
  ['Dbghelp',    Buffer.from('DbgHelp', 'latin1')],
  ['Python',     Buffer.from('Python', 'latin1')],
  ['python3',    Buffer.from('python3', 'latin1')],
  ['libpython',  Buffer.from('libpython', 'latin1')],
];

function main() {
  const argv = process.argv.slice(2);
  const file = argv[0];
  if (!file) { console.error('usage: node pack_probe.js <exe> [--detail]'); process.exit(1); }
  const detail = argv.includes('--detail');
  const buf = fs.readFileSync(file);

  console.log('file   : ' + file);
  console.log('size   : ' + buf.length.toLocaleString() + ' bytes (' +
              (buf.length / 1048576).toFixed(2) + ' MB)');
  console.log('');

  // ---- 1. entropy profile ------------------------------------------------
  const SLICE = 4 * 1024 * 1024;
  console.log('entropy per ' + (SLICE / 1048576) + ' MB slice');
  console.log('-'.repeat(72));
  let hi = 0, lo = 0;
  for (let off = 0; off < buf.length; off += SLICE) {
    const end = Math.min(off + SLICE, buf.length);
    const h = entropyOf(buf, off, end);
    if (h > 7.5) hi++; else if (h < 6) lo++;
    const bar = '#'.repeat(Math.round(h * 6));
    console.log('  0x' + off.toString(16).padStart(8, '0') + '  ' +
                h.toFixed(3) + '  ' + bar);
  }
  console.log('  -> high-entropy slices: ' + hi + '   low-entropy slices: ' + lo);
  console.log('');

  // ---- 2. printable strings ---------------------------------------------
  const pr = printableRuns(buf, 8);
  const pct = (pr.total / buf.length * 100);
  console.log('printable ascii runs >= 8 chars : ' + pr.count.toLocaleString() +
              '  (' + pct.toFixed(2) + '% of file, longest ' + pr.longest + ')');
  if (pct < 1.0) console.log('  -> LOW: consistent with an encrypted/packed payload');
  console.log('');

  // ---- 3. embedded PEs ---------------------------------------------------
  const pes = findEmbeddedPE(buf);
  console.log('embedded PE images (MZ..PE at +0x3c): ' + pes.length);
  for (const p of pes) {
    console.log('  0x' + p.off.toString(16).padStart(10, '0') + '  ' + p.note);
  }
  console.log('');

  // ---- 4. protector markers ---------------------------------------------
  console.log('marker scan');
  console.log('-'.repeat(72));
  for (const [name, needle] of PROTECTORS) {
    const idx = buf.indexOf(needle);
    console.log('  ' + name.padEnd(14) + (idx >= 0 ? 'hit @ 0x' + idx.toString(16) : '-'));
  }
  console.log('');

  if (detail) {
    // first 512 bytes, annotated -- shows what the header area really holds
    console.log('first 512 bytes');
    console.log('-'.repeat(72));
    for (let i = 0; i < 512; i += 16) {
      const hex = [...buf.subarray(i, i + 16)]
        .map(b => b.toString(16).padStart(2, '0')).join(' ');
      const asc = [...buf.subarray(i, i + 16)]
        .map(b => (b >= 0x20 && b <= 0x7e) ? String.fromCharCode(b) : '.').join('');
      console.log('  0x' + i.toString(16).padStart(6, '0') + '  ' + hex.padEnd(47) + '  ' + asc);
    }
  }
}

main();
