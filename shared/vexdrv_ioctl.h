/* -----------------------------------------------------------------------------
 *  vexdrv_ioctl.h  --  contract shared by the kernel driver and the usermode client
 *
 *  The device name is deliberately not a constant: both sides read it from a
 *  build-time define so the name is unique per build and cannot be probed by a
 *  fixed string.
 * -------------------------------------------------------------------------- */
#pragma once

#ifndef VEX_DEVICE_NAME
#define VEX_DEVICE_NAME  L"\\Device\\vexdrv"
#endif

#define VEX_DOS_DEVICE_NAME  L"\\DosDevices\\vexdrv"

#define FILE_DEVICE_UNKNOWN  0x00000022
#define VEX_DEVICE_TYPE      (FILE_DEVICE_UNKNOWN)

#define IOCTL_VEX_READ_VIRTUAL   CTL_CODE(VEX_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VEX_WRITE_VIRTUAL  CTL_CODE(VEX_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VEX_PING           CTL_CODE(VEX_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* METHOD_BUFFERED: the kernel stages the payload, so the caller must be able to
 * pass a usermode pointer and a usermode destination in the same struct. */
typedef struct _VEX_REQUEST {
    ULONG  ProcessId;
    ULONG  _pad0;
    ULONG64 Address;        /* virtual address in the target process        */
    ULONG64 Buffer;         /* destination in *this* process (usermode VA)  */
    ULONG   Size;
    ULONG   _pad1;
} VEX_REQUEST, *PVEX_REQUEST;

typedef struct _VEX_PING {
    ULONG64 Magic;          /* echoed back; proves the driver answered      */
    ULONG64 Tick;
    ULONG64 Padding[6];
} VEX_PING, *PVEX_PING;

#define VEX_PING_MAGIC 0x56455850494E4731ull   /* "VEXPING1" */
