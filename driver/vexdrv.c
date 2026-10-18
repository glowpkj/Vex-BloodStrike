/* =============================================================================
 *  vexdrv.c  --  the kernel component
 *
 *  Exposes a single MmCopyVirtualMemory primitive behind a device that is never
 *  registered with the PnP stack, has no service entry and no symbol.  Because
 *  the image is mapped by hand (see mapper/), there is no registry footprint,
 *  nothing for usermode driver enumeration to find, and -- critically -- no
 *  usermode handle to the game process ever exists.  Vanguard's handle
 *  scrubbing has nothing to intercept.
 *
 *  Source access uses KernelMode so pages belonging to a just-spawned pawn that
 *  have not been faulted in yet do not raise a fault in the caller.
 * ============================================================================= */
/* ntifs.h rather than ntddk.h: it pulls wdm.h in transitively and additionally
 * declares the object-manager surface this driver needs --
 * PsLookupProcessByProcessId, ObDereferenceObject, PsProcessType, PsThreadType
 * and the EPROCESS layout.  Including both headers is what causes the
 * conflicting typedefs; using only ntifs.h is the fix. */
#include <ntifs.h>
#include <ntstrsafe.h>

#include "vexdrv_ioctl.h"

#define VEX_POOL_TAG 'xveV'

/* Defined in protect.c -- see that file for what is stripped and why. */
NTSTATUS  VexProtectProcess(PEPROCESS proc);
BOOLEAN   VexProtectionRegistered(void);
void      VexUnprotect(void);

/* MmCopyVirtualMemory is not declared in the public WDK headers; declare it
 * explicitly so the call type-checks against ntoskrnl's export. */
NTSTATUS NTAPI MmCopyVirtualMemory(
    PEPROCESS  SourceProcess,
    PVOID      SourceAddress,
    PEPROCESS  TargetProcess,
    PVOID      TargetAddress,
    SIZE_T     BufferSize,
    KPROCESSOR_MODE PreviousMode,
    PSIZE_T    ReturnSize
);

typedef struct _VEX_DEVICE {
    UNICODE_STRING  SymLink;
    volatile LONG   Requests;
    volatile LONG64 Bytes;
    PDEVICE_OBJECT  DeviceObject;
} VEX_DEVICE, *PVEX_DEVICE;

static PVEX_DEVICE g_device = NULL;

/* ---------------------------------------------------------------------------
 *  Resolve a PID to an EPROCESS.
 *
 *  PsLookupProcessByProcessId is used rather than walking PsActiveProcessHead:
 *  the list walk needs the offset of ActiveProcessLinks inside EPROCESS, which
 *  is not a public contract and moves between kernel builds.  This API takes a
 *  PID directly.
 *
 *  IT TAKES A REFERENCE on the returned object.  Every success here MUST be
 *  balanced with ObDereferenceObject by the caller, or the process object can
 *  never be freed -- which means a target that exits stays half-alive for as
 *  long as the driver holds it.
 * ------------------------------------------------------------------------- */
static PEPROCESS vex_resolve(ULONG pid) {
    PEPROCESS proc = NULL;
    NTSTATUS  st;

    if (!pid) return NULL;

    /* The "process id" is passed as a shorthand handle value, not a real one. */
    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st) || !proc) return NULL;

    return proc;                       /* caller must ObDereferenceObject */
}

/* --------------------------------------------------------------------------- */
static NTSTATUS vex_transfer(PVEX_REQUEST req, BOOLEAN to_target) {
    PEPROCESS proc;
    NTSTATUS  st = STATUS_SUCCESS;
    SIZE_T    moved = 0;

    if (!req || !req->Size || req->Size > (64u << 20))
        return STATUS_INVALID_PARAMETER;

    /* METHOD_BUFFERED hands us a kernel copy of the struct, but the Buffer
     * field still points into the caller's address space.  ProbeForRead and
     * ProbeForWrite return void and raise on failure, so a separate
     * __try/__except is needed to turn a bad pointer into a status. */
    __try {
        ProbeForRead(req, sizeof(VEX_REQUEST), __alignof(VEX_REQUEST));
        if (to_target)
            ProbeForRead((PVOID)req->Buffer, req->Size, 1);
        else
            ProbeForWrite((PVOID)req->Buffer, req->Size, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_ACCESS_VIOLATION;
    }

    proc = vex_resolve(req->ProcessId);
    if (!proc) return STATUS_NOT_FOUND;

    /* KernelMode on both sides: the source is the target process, the
     * destination is the caller's usermode buffer which we just validated. */
    __try {
        NTSTATUS s = MmCopyVirtualMemory(
            proc,                                   /* source process      */
            (PVOID)(ULONG_PTR)req->Address,         /* source address      */
            PsGetCurrentProcess(),                  /* target process      */
            (PVOID)(ULONG_PTR)req->Buffer,          /* target address      */
            req->Size,
            KernelMode, KernelMode);
        if (!NT_SUCCESS(s)) st = s;
        else moved = req->Size;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = STATUS_ACCESS_VIOLATION;
    }

    /* Mandatory: vex_resolve took a reference.  Without this the process
     * object is never freed, so a game that exits stays in a half-terminated
     * state for as long as this driver is loaded. */
    ObDereferenceObject(proc);

    if (NT_SUCCESS(st) && g_device) {
        InterlockedIncrement(&g_device->Requests);
        InterlockedExchangeAdd64(&g_device->Bytes, (LONG64)moved);
    }
    return st;
}

/* --------------------------------------------------------------------------- */
static NTSTATUS vex_dispatch(PDEVICE_OBJECT DeviceObject, IRP *Irp) {
    PIO_STACK_LOCATION io  = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS           st  = STATUS_INVALID_DEVICE_REQUEST;
    ULONG              code= io->Parameters.DeviceIoControl.IoControlCode;

    UNREFERENCED_PARAMETER(DeviceObject);

    switch (code) {
    case IOCTL_VEX_PING: {
        PVEX_PING p = (PVEX_PING)Irp->AssociatedIrp.SystemBuffer;
        if (p) {
            RtlZeroMemory(p, sizeof(*p));
            ULONG tick = 0;
            KeQueryTickCount(&tick);
            p->Magic = VEX_PING_MAGIC;
            p->Tick  = tick;

            /* First process to reach us is the overlay.  Register protection
             * for it once, here, where the requesting EPROCESS is available. */
            if (!VexProtectionRegistered()) {
                PEPROCESS caller = IoGetRequestorProcess(Irp);
                if (caller) VexProtectProcess(caller);
            }
            st = STATUS_SUCCESS;
        }
        break;
    }
    case IOCTL_VEX_READ_VIRTUAL:
        st = vex_transfer((PVEX_REQUEST)Irp->AssociatedIrp.SystemBuffer, FALSE);
        break;

    case IOCTL_VEX_WRITE_VIRTUAL:
        st = vex_transfer((PVEX_REQUEST)Irp->AssociatedIrp.SystemBuffer, TRUE);
        break;

    default:
        break;
    }

    Irp->IoStatus.Status      = st;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

static VOID vex_unload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);

    // The teardown order matters:
    //   1. IoDeleteDevice first -- the device object still points its
    //      DeviceExtension at our VEX_DEVICE, and IoDeleteDevice touches that
    //      extension.  Freeing the pool before this leaves it dangling, a
    //      use-after-free that BSODs on every "sc stop".
    //   2. then the symbolic link
    //   3. then the pool
    if (DriverObject->DeviceObject) {
        IoDeleteDevice(DriverObject->DeviceObject);
        DriverObject->DeviceObject = NULL;
    }

    PVEX_DEVICE state = (PVEX_DEVICE)g_device;
    if (state) {
        if (state->SymLink.Buffer) {
            IoDeleteSymbolicLink(&state->SymLink);
            // Pool-allocated in DriverEntry; it used to point at a string
            // literal via RtlInitUnicodeString, which made ExFreePool corrupt
            // the kernel pool here.
            ExFreePool(state->SymLink.Buffer);
        }
        ExFreePool(state);
        g_device = NULL;
    }

    VexUnprotect();
    DbgPrint("[vex] unloaded\n");
}

/* =============================================================================
 *  Entry point.  Named so the manual mapper can resolve it from the export
 *  directory after relocating the image.
 * ============================================================================= */
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    PVEX_DEVICE     dev = NULL;
    PDEVICE_OBJECT  device_obj = NULL;
    UNICODE_STRING  us;
    UNICODE_STRING  dev_name;
    NTSTATUS        st;
    size_t          name_bytes;

    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = vex_dispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = vex_dispatch;
    DriverObject->MajorFunction[IRP_MJ_READ]           = vex_dispatch;
    DriverObject->MajorFunction[IRP_MJ_WRITE]          = vex_dispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = vex_dispatch;
    DriverObject->DriverUnload = vex_unload;

    dev = (PVEX_DEVICE)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                       sizeof(VEX_DEVICE), VEX_POOL_TAG);
    if (!dev) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(dev, sizeof(*dev));

    // The device MUST be created with a name.  IoCreateSymbolicLink can only
    // link to a device that exists under \Device; an unnamed device makes the
    // link fail, which is why \\.\vexdrv could not be opened.
    RtlInitUnicodeString(&dev_name, VEX_DEVICE_NAME);
    st = IoCreateDevice(DriverObject, sizeof(VEX_DEVICE), &dev_name,
                        VEX_DEVICE_TYPE, FILE_DEVICE_SECURE_OPEN, FALSE,
                        &device_obj);
    if (!NT_SUCCESS(st)) {
        DbgPrint("[vex] IoCreateDevice failed 0x%08X\n", st);
        ExFreePool(dev);
        return st;
    }

    // Keep the extension pointing at our state so vex_unload can find it.
    device_obj->DeviceExtension = dev;

    device_obj->Flags |= DO_DIRECT_IO;
    device_obj->Flags &= ~DO_DEVICE_INITIALIZING;

    // The symbolic-link UNICODE_STRING must own its buffer: RtlInitUnicodeString
    // only points at the literal, and the unload frees this buffer.  Copy it
    // into pool memory so ExFreePool later is legal.
    name_bytes = sizeof(VEX_DOS_DEVICE_NAME) + sizeof(WCHAR);
    dev->SymLink.Buffer = (PWCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                  name_bytes, VEX_POOL_TAG);
    if (!dev->SymLink.Buffer) {
        IoDeleteDevice(device_obj);
        ExFreePool(dev);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(dev->SymLink.Buffer, VEX_DOS_DEVICE_NAME,
                  sizeof(VEX_DOS_DEVICE_NAME) - sizeof(WCHAR));
    dev->SymLink.Length        = (USHORT)(sizeof(VEX_DOS_DEVICE_NAME) - sizeof(WCHAR));
    dev->SymLink.MaximumLength = (USHORT)name_bytes;

    // dev_name was already initialised above for IoCreateDevice; the symbolic
    // link must point at that exact \Device\vexdrv.
    st = IoCreateSymbolicLink(&dev->SymLink, &dev_name);
    if (!NT_SUCCESS(st)) {
        // Non-fatal: without the DOS link \\.\vexdrv will not open.
        DbgPrint("[vex] symlink failed 0x%08X\n", st);
    }

    dev->DeviceObject = device_obj;
    g_device = dev;
    DbgPrint("[vex] loaded, symlink=0x%08X\n", st);
    return STATUS_SUCCESS;
}
