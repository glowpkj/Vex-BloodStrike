// Relaxed sweep: are vftables present anywhere, in any section, in any encoding?
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
const t = secs.find(s => s.name === '.text');
const inText = (rva) => rva >= t.rva && rva < t.rva + t.vsize;

console.log('section            slots->.text   runs(>=3)   runs(>=3,prologue)');
for (const s of secs) {
  if (!s.rawsz) continue;
  let inTextHits = 0, run3 = 0, run3p = 0;
  let run = 0;
  const end = s.raw + s.rawsz;

  const PRO = [[0x48,0x89],[0x48,0x83,0xec],[0x40,0x55],[0x55,0x48,0x8b,0xec],
               [0x48,0x8b,0xec],[0x40,0x53],[0x56],[0x48,0x81,0xec],[0x53],
               [0x41,0x56],[0x48,0x89,0x5c],[0x40,0x53,0x48],[0xcc],[0x33,0xc0]];
  const fn = (rva) => {
    const off = s.raw + (rva - s.rva);
    if (off < 0 || off + 4 > buf.length) return false;
    return PRO.some(p => p.every((b, k) => buf[off + k] === b));
  };

  let runP = 0;
  for (let o = s.raw; o + 4 <= end; o += 8) {
    const r = rvaOf(o);
    const tgt = (r + buf.readInt32LE(o)) >>> 0;
    if (inText(tgt)) {
      inTextHits++;
      run++;
      runP = fn(tgt) ? runP + 1 : 0;
      if (run === 3) run3++;
      if (runP === 3) run3p++;
    } else { run = 0; runP = 0; }
  }
  console.log(s.name.padEnd(18) +
              String(inTextHits).padStart(12) +
              String(run3).padStart(12) +
              String(run3p).padStart(19));
}
console.log('');
console.log('.text spans ' + ((t.vsize / (0x94e9000)) * 100).toFixed(1) +
            '% of the image, so "lands in .text" alone is nearly meaningless.');
