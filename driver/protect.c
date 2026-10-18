/* =============================================================================
 *  protect.c  --  process protection callbacks, compiled into vexdrv.sys
 *
 *  Vanguard's scanner works by opening handles to every running process and
 *  inspecting what it finds.  ObRegisterCallbacks is the sanctioned place to
 *  answer that: a PRE_OPERATION callback runs before the grant, so rights can
 *  be stripped rather than merely logged afterwards.
 *
 *  Stripped from any handle opened to the overlay process:
 *      PROCESS_VM_READ / VM_WRITE / VM_OPERATION  -- memory inspection
 *      PROCESS_CREATE_THREAD                      -- injection
 *      PROCESS_DUP_HANDLE                         -- handle theft
 *      PROCESS_TERMINATE / SUSPEND_RESUME         -- kill
 *      PROCESS_SET_INFORMATION / SET_QUOTA        -- tamper
 *
 *  The target is self-configuring: the first process to open our device is the
 *  overlay, and we protect that EPROCESS from then on.  Nothing depends on a
 *  process name, which a scanner could read back anyway.
 * ============================================================================= */
#include <ntifs.h>

#include "vexdrv_ioctl.h"

#define VEX_POOL_TAG 'xveV'

/* Rights that let a caller learn what we are or touch us. */
#define VEX_DENIED_ACCESS                                                  \
    (PROCESS_VM_READ          | PROCESS_VM_WRITE   | PROCESS_VM_OPERATION  | \
     PROCESS_CREATE_THREAD    | PROCESS_DUP_HANDLE | PROCESS_TERMINATE     | \
     PROCESS_SUSPEND_RESUME  | PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA | \
     PROCESS_ALL_ACCESS)

/* PROCESS_QUERY_LIMITED_INFORMATION is a user-mode access right (0x1000);
 * it is not declared in the public WDK headers. */
#define PROCESS_QUERY_LIMITED_INFORMATION 0x00001000

/* Rights that stay, because stripping them makes the handle look anomalous and
 * a scanner will escalate to the kernel component when it sees that:
 *      SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ (no)
 * We keep only SYNCHRONIZE so the scanner sees a "process exists" answer. */
#define VEX_ALLOWED_ACCESS (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION)

static PEPROCESS g_protected = NULL;
static PVOID     g_cb_handle = NULL;

/* -------------------------------------------------------------------------- */
static OB_PREOP_CALLBACK_STATUS NTAPI vex_pre_op(PVOID RegistrationContext, POB_PRE_OPERATION_INFORMATION OperationInformation) {
    PEPROCESS target;
    ACCESS_MASK* granted;

    UNREFERENCED_PARAMETER(RegistrationContext);

    if (!g_protected)                                   return OB_PREOP_SUCCESS;
    target = (PEPROCESS)OperationInformation->Object;
    if (target != g_protected)                          return OB_PREOP_SUCCESS;

    granted = &OperationInformation->Parameters->CreateHandleInformation.DesiredAccess;
    *granted &= VEX_ALLOWED_ACCESS;

    /* SYNCHRONIZE must survive or the handle is invalid and the scanner knows
     * something happened.  Keep it even if the caller's mask was narrower. */
    if (!(*granted & SYNCHRONIZE)) *granted |= SYNCHRONIZE;

    return OB_PREOP_SUCCESS;
}

/* -------------------------------------------------------------------------- */
NTSTATUS VexProtectProcess(PEPROCESS proc) {
    OB_CALLBACK_REGISTRATION reg = { 0 };
    OB_OPERATION_REGISTRATION  ops[2] = { 0 };

    if (!proc) return STATUS_INVALID_PARAMETER;

    if (g_cb_handle) {                                  /* already registered */
        g_protected = proc;
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&reg, sizeof(reg));
    RtlZeroMemory(ops, sizeof(ops));

    /* PsProcessType fires for handle creation on processes ... */
    reg.Version               = OB_FLT_REGISTRATION_VERSION;
    reg.OperationRegistrationCount = 2;
    reg.Altitude.Buffer       = NULL;
    reg.Altitude.Length       = 0;
    reg.Altitude.MaximumLength = 0;
    reg.RegistrationContext   = NULL;
    reg.OperationRegistration = ops;

    /* ... and the mirror class catches thread opens, which is how an injector
     * usually gets started. */
    ops[0].ObjectType   = PsProcessType;
    ops[0].Operations   = OB_OPERATION_HANDLE_CREATE;
    ops[0].PreOperation = vex_pre_op;

    ops[1].ObjectType   = PsThreadType;
    ops[1].Operations   = OB_OPERATION_HANDLE_CREATE;
    ops[1].PreOperation = vex_pre_op;

    {
        NTSTATUS st = ObRegisterCallbacks(&reg, &g_cb_handle);
        if (!NT_SUCCESS(st)) return st;
    }

    g_protected = proc;
    DbgPrint("[vex] process protection registered\n");
    return STATUS_SUCCESS;
}

BOOLEAN VexProtectionRegistered(void) { return g_cb_handle != NULL; }

void VexUnprotect(void) {
    if (g_cb_handle) {
        ObUnRegisterCallbacks(g_cb_handle);
        g_cb_handle = NULL;
    }
    g_protected = NULL;
}
