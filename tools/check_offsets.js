// Check whether a set of claimed RVAs actually land where their described
// purpose implies.  A "global D3D11 device pointer" or "global object list"
// must live in a writable data section.  Finding one inside .text is a strong
// signal the offset is stale -- i.e. from a different patch.
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
const toOff = (rva) => {
  for (const s of secs) {
    const span = Math.max(s.vsize, s.rawsz);
    if (rva >= s.rva && rva < s.rva + span) return s.raw + (rva - s.rva);
  }
  return -1;
};
const secOf = (rva) => {
  for (const s of secs) {
    const span = Math.max(s.vsize, s.rawsz);
    if (rva >= s.rva && rva < s.rva + span) return s;
  }
  return null;
};

const claimed = [
  ['addrD3D11Device',  0x39B8DA8, 'global ptr -> ID3D11Device'],
  ['addrObjects',      0x39C8B98, 'global ptr -> object manager'],
];

console.log('image size 0x' + buf.readUInt32LE(opt + 56).toString(16));
console.log('');
for (const [nm, rva, purpose] of claimed) {
  const s = secOf(rva);
  const off = toOff(rva);
  console.log(nm + ' = 0x' + rva.toString(16).toUpperCase());
  console.log('   purpose : ' + purpose);
  console.log('   in      : ' + (s ? s.name : 'OUTSIDE IMAGE') +
              (s ? '  (r-x=' + !!(s.chars & 0x20000000) +
                   ' rw-=' + !!(s.chars & 0x80000000) + ')' : ''));
  if (off >= 0 && off + 32 <= buf.length) {
    const hex = [...buf.subarray(off, off + 32)]
      .map(b => b.toString(16).padStart(2, '0')).join(' ');
    console.log('   bytes   : ' + hex);
    // crude code-vs-data read: x64 prologues vs a plausible pointer/init value
    const looksCode = [0x48, 0x40, 0x55, 0x56, 0x57, 0x41, 0xcc, 0x33, 0x8b, 0x89]
      .includes(buf[off]);
    const q = buf.readBigUInt64LE(off);
    console.log('   qword   : 0x' + q.toString(16) +
                (q >= 0x140000000n && q < 0x150000000n ? '   <- in-image pointer' : ''));
  }
  console.log('');
}
