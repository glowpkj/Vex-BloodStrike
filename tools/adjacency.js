// Test whether vftables sit adjacent to their COL in .rdata.
//
// The protector stripped the vtable[-1] -> COL back-pointer, which kills every
// RTTI dumper.  But the *linker* still emits a class's COL and its vftable
// together (both are COMDATs of the same class), so if they remain adjacent we
// can recover the pairing positionally without the back-pointer at all.
//
// Method: take COLs we have already proven valid (signature 1, pSelf == own RVA,
// pTypeDescriptor resolving to a known descriptor) and dump the bytes around
// them, marking any 8-byte slot that behaves like a vtable entry.
//
// An entry is judged real only if, read as a slot-relative int32, it lands in
// .text AND the target begins with a plausible function prologue.  .text spans
// 26% of the address space, so "lands in .text" alone is far too weak.
const fs = require('fs');
const buf = fs.readFileSync(process.argv[2]);

const lf = buf.readUInt32LE(0x3c);
const nsec = buf.readUInt16LE(lf + 4 + 2);
const so = buf.readUInt16LE(lf + 4 + 16);
const opt = lf + 4 + 20;
const sb = opt + so;
const secs = [];
for (let i = 0; i < nsec; i++) {
  const o = sb + i * 40;
  secs.push({
    name: buf.subarray(o, o + 8).toString('latin1').replace(/\0+$/, ''),
    vsize: buf.readUInt32LE(o + 8), rva: buf.readUInt32LE(o + 12),
    rawsz: buf.readUInt32LE(o + 16), raw: buf.readUInt32LE(o + 20),
    chars: buf.readUInt32LE(o + 36),
  });
}
const rvaOf = (off) => {
  for (const s of secs) if (off >= s.raw && off < s.raw + s.rawsz) return s.rva + (off - s.raw);
  return -1;
};
const toOff = (rva) => {
  for (const s of secs) {
    const span = Math.max(s.vsize, s.rawsz);
    if (rva >= s.rva && rva < s.rva + span) return s.raw + (rva - s.rva);
  }
  return -1;
};
const inText = (rva) => {
  const t = secs.find(s => s.name === '.text');
  return rva >= t.rva && rva < t.rva + t.vsize;
};

const PROLOGUES = [
  [0x48, 0x89, 0x5c, 0x24], [0x48, 0x89, 0x4c, 0x24], [0x48, 0x89, 0x54, 0x24],
  [0x48, 0x83, 0xec], [0x48, 0x81, 0xec], [0x40, 0x55], [0x55, 0x48, 0x8b, 0xec],
  [0x48, 0x8b, 0xec], [0x40, 0x53], [0x56, 0x48, 0x8b, 0xec], [0x48, 0x89, 0x4c],
  [0xcc], [0x33, 0xc0], [0x48, 0x8d, 0x05], [0x90], [0x41, 0x57],
];
function looksLikeFunction(rva) {
  const off = toOff(rva);
  if (off < 0 || off + 6 > buf.length) return false;
  return PROLOGUES.some(p => p.every((b, i) => buf[off + i] === b));
}

/// Find every run of >=3 consecutive vtable entries anywhere in .rdata.
function scanVftables(limit) {
  const runs = [];
  const rdata = secs.find(s => s.name === '.rdata');
  const start = rdata.raw, end = rdata.raw + rdata.rawsz;
  let i = start;
  while (i + 24 <= end) {
    const slotRva = rvaOf(i);
    const t0 = (slotRva + buf.readInt32LE(i)) >>> 0;
    if (!inText(t0) || !looksLikeFunction(t0)) { i += 8; continue; }
    let n = 0;
    while (n < 256) {
      const off = i + n * 8;
      if (off + 4 > end) break;
      const r = rvaOf(off);
      const t = (r + buf.readInt32LE(off)) >>> 0;
      if (!inText(t) || !looksLikeFunction(t)) break;
      n++;
    }
    if (n >= 3) runs.push({ rva: slotRva, count: n });
    i += n ? n * 8 : 8;
    if (runs.length >= limit) break;
  }
  return runs;
}

const runs = scanVftables(400);
console.log('vftable-shaped runs found in .rdata: ' + runs.length +
            ' (showing first 12)');
for (const r of runs.slice(0, 12)) {
  console.log('  rva 0x' + r.rva.toString(16) + '  entries=' + r.count);
}
console.log('');

// Now: are COLs adjacent to any of these runs?
const colRvas = new Set();
for (const s of secs) {
  if (!s.rawsz) continue;
  for (let o = s.raw; o + 24 <= s.raw + s.rawsz; o += 4) {
    if (buf.readUInt32LE(o) !== 1) continue;
    const r = rvaOf(o);
    if (buf.readInt32LE(o + 20) >>> 0 !== r) continue;
    colRvas.add(r);
  }
}
console.log('valid COLs (sig=1, pSelf==self): ' + colRvas.size);

let near = 0, exact = 0;
const gaps = [];
for (const r of runs) {
  let best = Infinity;
  for (const c of colRvas) {
    const d = r.rva - c;
    if (d > -0x100 && d < 0x1000) {                 // COL just before the vtable
      if (Math.abs(d) < best) best = d;
    }
  }
  if (best !== Infinity) { near++; gaps.push(best); }
  if (best === 24 || best === 8) exact++;
}
gaps.sort((a, b) => a - b);
console.log('runs with a COL within -0x100..0x1000 before them: ' + near +
            '   of which at the classic +24/+8 offsets: ' + exact);
if (gaps.length) {
  const q = (p) => gaps[Math.min(gaps.length - 1, Math.floor(gaps.length * p))];
  console.log('gap distribution (vtable_rva - col_rva): min=' + gaps[0] +
              ' p25=' + q(0.25) + ' p50=' + q(0.5) + ' p75=' + q(0.75) +
              ' max=' + gaps[gaps.length - 1]);
}
