#!/usr/bin/env node
// -----------------------------------------------------------------------------
//  rtti_dump.js  --  extract the MSVC RTTI class hierarchy from a shipping exe.
//
//  Why
//  ---
//  BloodStrike.exe turned out to be NetEase's *Messiah* engine (a cocos2d-x
//  fork), not Unreal and not Unity/IL2CPP.  There is no UWorld, no GNames, no
//  GUObjectArray to lean on: nothing resembles the reflection tables the
//  ecosystem tools rely on.  What the binary *does* keep is MSVC RTTI, because
//  NetEase builds with the CRT's type-descriptor machinery enabled.  Those
//  records are a complete, typed inventory of the game's own C++ classes:
//
//      .?AV<Class>@<Namespace>@@        class
//      .?AU<Struct>@<Namespace>@@       struct
//      .?AV<lambda_N>@...@@             lambda (noise, but useful for xref)
//
//  From a type-descriptor we can walk to its CompleteObjectLocator, then to the
//  vftable in .rdata, then to the vtable's functions in .text.  That is enough
//  to identify the player/entity/camera classes without any debug symbols.
//
//  This script does the inventory half: it finds every type descriptor, decodes
//  its MSVC decoration into a readable name, groups by namespace, and writes the
//  whole thing to JSON for the C++ side to consume.
//
//  Usage:
//      node rtti_dump.js <exe> [--grep PAT] [--json out.json] [--min 4]
// -----------------------------------------------------------------------------
'use strict';

const fs = require('fs');
const path = require('path');

function parsePE(buf) {
  const lfanew = buf.readUInt32LE(0x3c);
  const nsec    = buf.readUInt16LE(lfanew + 4 + 2);
  const sizeOpt = buf.readUInt16LE(lfanew + 4 + 16);
  const opt     = lfanew + 4 + 20;
  const imageBase = buf.readBigUInt64LE(opt + 24);
  const sizeOfHeaders = buf.readUInt32LE(opt + 60);
  const sections = [];
  const secBase = opt + sizeOpt;
  for (let i = 0; i < nsec; i++) {
    const o = secBase + i * 40;
    sections.push({
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
  const toOff = (rva) => (rva < sizeOfHeaders ? rva
    : (() => { const s = byRva(rva); return s ? s.pointerToRawData + (rva - s.virtualAddress) : -1; })());
  return { imageBase, sizeOfHeaders, sections, byRva, toOff };
}

// ------------------------------------------------------------------ demangle
//
//  Scope is deliberately narrow: only the type-descriptor grammar matters here
//      .?AVName@Ns1@Ns2@@      -> Name::Ns2::Ns1
//  plus the qualifiers that show up in practice (templates, lambdas, pointers).
//  Anything we cannot fully fold is returned with the templates intact, which is
//  still readable and still greppable.

function demangleTypeDescriptor(s) {
  // s is '.?AVFoo::Bar@Ns@@' / '.?AUStruct@Ns@@' -- the 'A' is a fixed tag,
  // the next char carries the kind, and the name follows.
  let body = s.slice(2, -2);          // strip leading '.?' and trailing '@@'

  let kind = '';
  if (body[0] === 'A' && (body[1] === 'V' || body[1] === 'U' || body[1] === 'W')) {
    kind = body[1] === 'V' ? 'class' : (body[1] === 'U' ? 'struct' : 'union');
    body = body.slice(2);
  } else if (body.startsWith('$A') || body.startsWith('$_Func_impl')) {
    kind = 'lambda';
  }

  // Split on '@' but keep balanced template arguments together.
  const parts = [];
  let cur = '', depth = 0;
  for (let i = 0; i < body.length; i++) {
    const c = body[i];
    if (c === '<' || c === '(') depth++;
    if (c === '>' || c === ')') depth--;
    if (c === '@' && depth === 0) { parts.push(cur); cur = ''; continue; }
    cur += c;
  }
  if (cur.length) parts.push(cur);

  if (kind === 'lambda' || kind === 'functor') {
    return { name: parts.join('::'), namespace: '', kind, mangled: s };
  }

  const name = parts[0] || '';
  const ns   = parts.slice(1).filter(Boolean).reverse();
  return { name, namespace: ns.join('::'), kind: kind || 'type', mangled: s };
}

// --------------------------------------------------------------- COL parsing
//
//  struct TypeDescriptor { void* pVFTable; void* spare; char name[]; }
//  struct RTTICompleteObjectLocator { DWORD sig, off, cd, pTypeDescriptor,
//                                      int pClassDescriptor, int pSelf; }
//  struct RTTIClassHierarchyDescriptor { DWORD sig, attributes, numBase,
//                                        int pBaseArray[]; }
//  Signature 1 on x64 (IMAGE_BUILD_NUMBER matching), 0 in older builds.

function collectTypeDescriptors(buf, pe) {
  const out = [];

  // NetEase places the RTTI descriptors in a *writable* section here, not in
  // .rdata -- the decorated names for this build sit at ~0x7C9F000, which is
  // inside .data.  So do not filter on the read-only flag; scan every section
  // with raw bytes on disk.  Byte-walk rather than regex: a lazy quantifier
  // over a 135 MB buffer backtracks pathologically and exhausts the heap before
  // it matches anything.
  const readable = pe.sections.filter(s => s.sizeOfRawData > 0);

  for (const sec of readable) {
    const start = sec.pointerToRawData;
    const end   = Math.min(start + sec.sizeOfRawData, buf.length);
    const secRva = sec.virtualAddress;

    for (let i = start; i + 4 < end; i++) {
      // type descriptors always begin with ".?AV" / ".?AU" / ".?AW"; the
      // lambda/functor forms start with ".?A$_Func" or ".?A$" which we skip --
      // they are thousands of entries of pure noise.
      if (buf[i] !== 0x2e || buf[i + 1] !== 0x3f || buf[i + 2] !== 0x41) continue;  // ".?A"
      const k = buf[i + 3];
      if (k !== 0x56 && k !== 0x55 && k !== 0x57) continue;                          // 'V' 'U' 'W'
      if (buf[i + 4] === 0x24) continue;                                              // "$" -> lambda

      // walk the decorated name to its terminating "@@"
      let e = i + 4;
      while (e + 1 < end && !(buf[e] === 0x40 && buf[e + 1] === 0x40)) {
        const c = buf[e];
        if (c === 0x00) break;
        if (c < 0x21 || c > 0x7e) break;
        e++;
      }
      if (!(buf[e] === 0x40 && buf[e + 1] === 0x40)) continue;
      if (e - (i + 4) < 2 || e - (i + 4) > 300) continue;

      const strOff = i;
      const nameBytes = buf.toString('latin1', i, e + 2);
      const tdOff = strOff - 16;
      if (tdOff < start) continue;         // descriptor must sit in the same blob

      const d = demangleTypeDescriptor(nameBytes);
      out.push({
        name: d.name,
        namespace: d.namespace,
        kind: d.kind,
        mangled: nameBytes,
        tdFileOffset: tdOff,
        tdRva: secRva + (tdOff - start),
        strFileOffset: strOff,
        strRva: secRva + (strOff - start),
        size: (e + 2) - strOff + 16,
      });
    }
  }
  return out;
}

/// Resolve every type descriptor to its Complete Object Locator, and from
/// there to its vftable.
///
/// Two facts drive the whole thing:
///
///   struct RTTICompleteObjectLocator {   // x64, self-relative int32 fields
///       DWORD signature;        // +0   (1 on modern toolchains)
///       DWORD offset;           // +4   vftable slot inside the object
///       DWORD cdOffset;         // +8
///       int   pTypeDescriptor;  // +12
///       int   pClassDescriptor; // +16
///       int   pSelf;            // +20
///   };
///
///   1. the COL never stores the vftable -- the vftable stores the COL, one
///      slot in front of the function pointers.  So discovering vftables means
///      searching for pointers *to* the COLs, not reading a field.
///   2. because this image is RELOCS_STRIPPED with a fixed 0x140000000 base,
///      that pointer is stored as a plain absolute VA, so one linear pass over
///      the data sections with a hash lookup finds every vftable at once.
///
/// The naive version -- scan the 0x400 bytes before each descriptor -- is
/// quadratic and exhausts the heap on a 135 MB image.  This is two linear
/// passes and a map.
function resolveColours(buf, pe) {
  const tds = collectTypeDescriptors(buf, pe);
  const byTd = new Map();
  for (const td of tds) byTd.set(td.tdRva, td);

  // ---- pass 1: locate the COLs -------------------------------------------
  const cols = new Map();          // colRva -> { td, cdOffset, objOffset }
  for (const sec of pe.sections) {
    if (!sec.sizeOfRawData) continue;
    const startOff = sec.pointerToRawData;
    const endOff = Math.min(startOff + sec.sizeOfRawData, buf.length);

    for (let o = startOff; o + 24 <= endOff; o += 4) {
      const colRva = sec.virtualAddress + (o - startOff);

      // pSelf is stored as an absolute RVA in this build (verified: the COL for
      // cocos2d::Node sits at RVA 0x70AE848 and its pSelf field reads exactly
      // that).  Requiring it first kills the coincidence matches -- and the
      // order matters, because testing the self-relative reading first used to
      // let a bogus zero-offset record claim the descriptor and block the real
      // COL behind it.
      const pSelf = buf.readInt32LE(o + 20);
      if ((pSelf >>> 0) !== colRva) continue;

      const sig = buf.readUInt32LE(o);
      if (sig !== 1) continue;

      const pTD = buf.readInt32LE(o + 12);
      const td = byTd.get(pTD >>> 0) || byTd.get((colRva + pTD) >>> 0);
      if (!td) continue;
      if (td.colRva) continue;                    // already claimed

      const pCD = buf.readInt32LE(o + 16);
      const cdRva = (pCD === 0) ? 0 : (pe.toOff(pCD >>> 0) >= 0 ? (pCD >>> 0)
                                                                  : ((colRva + pCD) >>> 0));

      cols.set(colRva, { td, objOffset: buf.readUInt32LE(o + 4), cdRva, pSelf });
      td.colRva = colRva;
    }
  }

  // ---- pass 2: find the vftables that point at those COLs ------------------
  // COLs live in read-only data; the pointers to them sit there too.  The slot
  // in front of a vftable can be encoded three ways depending on toolchain, so
  // accept all of them rather than betting on one:
  //     [a] full 8-byte absolute VA
  //     [b] 4-byte absolute RVA, in either half of the 8-byte slot
  //     [c] 4-byte offset relative to the slot itself
  const byVa = new Map();          // absolute VA (BigInt) -> colRva
  const byRva = new Map();         // colRva -> colRva
  for (const colRva of cols.keys()) {
    byVa.set(pe.imageBase + BigInt(colRva), colRva);
    byRva.set(colRva, colRva);
  }
  console.error('[resolve] type descriptors : ' + tds.length);
  console.error('[resolve] COLs located      : ' + cols.size);

  let vftFound = 0, vftTried = 0;
  for (const sec of pe.sections) {
    if (!sec.sizeOfRawData) continue;
    const startOff = sec.pointerToRawData;
    const endOff = Math.min(startOff + sec.sizeOfRawData, buf.length);

    // 4-byte alignment, not 8: this toolchain writes the COL reference as a
    // 4-byte RVA, so slots at addresses divisible by 4 but not 8 are exactly
    // the ones an 8-step misses.  Those are the common case here.
    for (let o = startOff; o + 16 <= endOff; o += 4) {
      const slotRva = sec.virtualAddress + (o - startOff);

      let hit = byRva.get(buf.readUInt32LE(o));
      if (hit === undefined) hit = byRva.get(buf.readUInt32LE(o + 4));
      if (hit === undefined) hit = byVa.get(buf.readBigUInt64LE(o));
      if (hit === undefined) {
        const rel = (slotRva + buf.readInt32LE(o)) >>> 0;
        hit = byRva.get(rel);
        if (hit === undefined) continue;
      }
      vftTried++;

      const entry = cols.get(hit);
      if (!entry || entry.td.vftableRva) continue;

      // The slot is 4 bytes, or 4 bytes plus padding.  Try both readings and
      // keep whichever yields an actual run of code pointers.
      for (const vftRva of [slotRva + 4, slotRva + 8]) {
        const vftOff = pe.toOff(vftRva);
        if (vftOff < 0 || vftOff + 8 > buf.length) continue;

        let n = 0;
        const vm = [];
        for (let k = 0; k < 1024; k++) {
          const e = pe.toOff((vftRva + k * 8) >>> 0);
          if (e < 0 || e + 4 > buf.length) { n = k; break; }
          const target = (vftRva + k * 8 + buf.readInt32LE(e)) >>> 0;
          const ts = pe.byRva(target);
          if (!ts || !(ts.characteristics & 0x20000000)) { n = k; break; }
          vm.push(target);
          n = k + 1;
        }
        if (n < 2) continue;      // a one-entry run is noise, not a vtable

        entry.td.vftableRva  = vftRva;
        entry.td.vftableSize = n;
        entry.td.vmethods    = vm;
        vftFound++;

        // class hierarchy: how many bases this class has
        const cdOff = pe.toOff(entry.cdRva);
        if (cdOff >= 0 && cdOff + 12 <= buf.length) {
          const numBases = buf.readUInt32LE(cdOff + 8);
          if (numBases > 0 && numBases < 256) {
            entry.td.numBases = numBases;
            entry.td.baseClasses = [];
            for (let b = 0; b < numBases; b++) {
              const bcOff = cdOff + 12 + b * 4;
              if (bcOff + 4 > buf.length) break;
              entry.td.baseClasses.push(buf.readUInt32LE(bcOff));
            }
          }
        }
        break;
      }
    }
  }
  console.error('[resolve] vftable slots hit : ' + vftTried +
                '   resolved: ' + vftFound);

  for (const td of tds) {
    td.colRva      = td.colRva      || 0;
    td.vftableRva  = td.vftableRva  || 0;
    td.vftableSize = td.vftableSize || 0;
    td.vmethods    = td.vmethods    || [];
    td.numBases    = td.numBases    ?? null;
    td.baseClasses = td.baseClasses || [];
  }
  return tds;
}

// -------------------------------------------------------------------- main
function main() {
  const argv = process.argv.slice(2);
  const file = argv.find(a => !a.startsWith('--'));
  if (!file) { console.error('usage: node rtti_dump.js <exe> [--grep PAT] [--json out.json]'); process.exit(1); }
  const argAfter = (f, d) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : d; };
  const grep = argAfter('--grep', null);
  const jsonOut = argAfter('--json', null);
  const minLen = parseInt(argAfter('--min', '4'), 10);

  const buf = fs.readFileSync(file);
  const pe = parsePE(buf);

  console.log('='.repeat(100));
  console.log('  rtti_dump  --  ' + path.basename(file));
  console.log('='.repeat(100));

  let tds = collectTypeDescriptors(buf, pe);
  console.log(`  type descriptors found: ${tds.length}`);
  tds = resolveColours(buf, pe);

  const named = tds.filter(t => t.name.length >= minLen && !t.name.startsWith('$'));
  const withVft = tds.filter(t => t.vftableRva !== 0);
  console.log(`  named types           : ${named.length}`);
  console.log(`  resolved to a vftable : ${withVft.length}`);
  console.log('');

  // namespace histogram
  const byNs = new Map();
  for (const t of named) {
    const k = t.namespace || '<global>';
    byNs.set(k, (byNs.get(k) || 0) + 1);
  }
  console.log('  namespaces by class count');
  console.log('  ' + '-'.repeat(98));
  for (const [k, v] of [...byNs.entries()].sort((a, b) => b[1] - a[1]).slice(0, 40)) {
    console.log('  ' + String(v).padStart(5) + '  ' + k);
  }
  console.log('');

  let show = named;
  if (grep) {
    const re = new RegExp(grep, 'i');
    show = show.filter(t => re.test(t.namespace + '::' + t.name));
  }
  console.log(`  types${grep ? ' matching /' + grep + '/' : ''}   (showing ${Math.min(show.length, 400)})`);
  console.log('  ' + '-'.repeat(98));
  console.log('  ' + 'tdRva'.padStart(10) + 'vftRva'.padStart(11) + 'nvm'.padStart(5) +
              '  kind     full name');
  for (const t of show.slice(0, 400)) {
    console.log('  ' + ('0x' + t.tdRva.toString(16)).padStart(10) +
                ('0x' + t.vftableRva.toString(16)).padStart(11) +
                String(t.vmethods.length).padStart(5) + '  ' +
                (t.kind || '').padEnd(8) + (t.namespace ? t.namespace + '::' : '') + t.name);
  }
  console.log('');

  if (jsonOut) {
    const payload = {
      image_base: '0x' + pe.imageBase.toString(16),
      source: path.basename(file),
      types: tds.map(t => ({
        name: t.name, namespace: t.namespace, kind: t.kind, mangled: t.mangled,
        td_rva: '0x' + t.tdRva.toString(16),
        col_rva: t.colRva ? '0x' + t.colRva.toString(16) : null,
        vftable_rva: t.vftableRva ? '0x' + t.vftableRva.toString(16) : null,
        vmethod_count: t.vmethods.length,
        num_bases: t.numBases ?? null,
        base_classes: t.baseClasses ?? null,
        vmethods: t.vmethods.map(v => '0x' + v.toString(16)),
      })),
    };
    fs.writeFileSync(jsonOut, JSON.stringify(payload, null, 2));
    console.log(`  wrote ${jsonOut}  (${tds.length} types)`);
    console.log('');
  }
}

main();
