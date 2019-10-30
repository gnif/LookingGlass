#include <windows.h>
#include <initguid.h>

DEFINE_GUID (GUID_DEVINTERFACE_PORTHOLE,
    0x10ccc0ac,0xf4b0,0x4d78,0xba,0x41,0x1e,0xbb,0x38,0x5a,0x52,0x85);
// {10ccc0ac-f4b0-4d78-ba41-1ebb385a5285}

typedef struct _PortholeMsg
{
	UINT32 type;
	PVOID  addr;
	UINT32 size;
}
PortholeMsg, *PPortholeMsg;

typedef struct _PortholeLockMsg
{
	PVOID  addr;
	UINT32 size;
}
PortholeLockMsg, *PPortholeLockMsg;

#define IOCTL_PORTHOLE_SEND_MSG      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PORTHOLE_UNLOCK_BUFFER CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)