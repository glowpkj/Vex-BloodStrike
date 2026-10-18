// Validate a candidate view-projection by projecting points and checking the
// results land on screen.  A structural tests alone let the identity matrix
// through; this one asks the only question that matters: does this matrix put
// the world in front of a camera?
const fs = require('fs');
const m = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const vp = m.viewproj;

if (!vp || !vp.found) { console.log('no VP recorded'); process.exit(0); }
console.log('VP at', vp.va, 'fov', vp.fov);
const M = vp.m;
const show = (label, a) => console.log('  ' + label.padEnd(10), a.map(v => v.toFixed(4)).join('  '));
for (let r = 0; r < 4; r++) show('row' + r, M.slice(r * 4, r * 4 + 4));

// Project a world point: clip = M * (x,y,z,1); ndc = clip.xyz / clip.w
function proj(x, y, z) {
  const cx = M[0]*x + M[1]*y + M[2]*z + M[3];
  const cy = M[4]*x + M[5]*y + M[6]*z + M[7];
  const cw = M[12]*x + M[13]*y + M[14]*z + M[15];
  if (Math.abs(cw) < 1e-9) return null;
  return { x: cx/cw, y: cy/cw, w: cw };
}

console.log('\nrow norms (should be f/aspect, f, |A|):');
for (let r = 0; r < 4; r++) {
  const n = Math.hypot(M[r*4], M[r*4+1], M[r*4+2]);
  console.log('  row' + r, n.toFixed(4));
}
console.log('\nrow orthogonality (3D parts, should be ~0):');
for (let a = 0; a < 3; a++) for (let b = a+1; b < 3; b++) {
  const d = M[a*4]*M[b*4] + M[a*4+1]*M[b*4+1] + M[a*4+2]*M[b*4+2];
  console.log('  row'+a+'.row'+b, d.toExponential(2));
}

// A camera at the origin looking down -Z: points in front should give
// ndc in [-1,1] and w > 0.
console.log('\nprojecting points for a camera at origin looking down -Z:');
const tests = [[0,0,-1],[0,0,-5],[0,0,-50],[1,0,-10],[0,1,-10],[0,0,1],[0,0,-0.1]];
let inside = 0;
for (const [x,y,z] of tests) {
  const p = proj(x, y, z);
  if (!p) { console.log(`  (${x},${y},${z}) -> degenerate`); continue; }
  const ok = p.w > 0 && Math.abs(p.x) <= 1.5 && Math.abs(p.y) <= 1.5;
  if (ok) inside++;
  console.log(`  (${x},${y},${z}) -> ndc ${p.x.toFixed(4)}, ${p.y.toFixed(4)}  w=${p.w.toFixed(4)}  ${ok?'on screen':'BEHIND/outside'}`);
}
console.log(`\n  ${inside}/${tests.length} plausible`);

// The eye can be recovered: for VP = P*V the camera position is such that
// M * eye = (0,0,-something,0) for a standard projection (the w row is the
// third basis vector, so w=0 plane passes through the eye).
console.log('\nw-row as the third basis vector (unit length expected):');
const w = [M[12], M[13], M[14]];
console.log('  |w3d| =', Math.hypot(...w).toFixed(4));
console.log('  m[15] =', M[15].toFixed(4));
