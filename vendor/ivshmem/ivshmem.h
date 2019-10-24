#include <initguid.h>

DEFINE_GUID (GUID_DEVINTERFACE_IVSHMEM,
    0xdf576976,0x569d,0x4672,0x95,0xa0,0xf5,0x7e,0x4e,0xa0,0xb2,0x10);
// {df576976-569d-4672-95a0-f57e4ea0b210}

typedef UINT16 IVSHMEM_PEERID;
typedef UINT64 IVSHMEM_SIZE;

#define IVSHMEM_CACHE_NONCACHED     0
#define IVSHMEM_CACHE_CACHED        1
#define IVSHMEM_CACHE_WRITECOMBINED 2

/*
    This structure is for use with the IOCTL_IVSHMEM_REQUEST_MMAP IOCTL
*/
typedef struct IVSHMEM_MMAP_CONFIG
{
    UINT8 cacheMode; // the caching mode of the mapping, see IVSHMEM_CACHE_* for options
}
IVSHMEM_MMAP_CONFIG, *PIVSHMEM_MMAP_CONFIG;

/*
    This structure is for use with the IOCTL_IVSHMEM_REQUEST_MMAP IOCTL
*/
typedef struct IVSHMEM_MMAP
{
    IVSHMEM_PEERID peerID;  // our peer id
    IVSHMEM_SIZE   size;    // the size of the memory region
    PVOID          ptr;     // pointer to the memory region
    UINT16         vectors; // the number of vectors available
}
IVSHMEM_MMAP, *PIVSHMEM_MMAP;

/*
    This structure is for use with the IOCTL_IVSHMEM_RING_DOORBELL IOCTL
*/
typedef struct IVSHMEM_RING
{
    IVSHMEM_PEERID peerID;  // the id of the peer to ring
    UINT16         vector;  // the doorbell to ring
}
IVSHMEM_RING, *PIVSHMEM_RING;

/*
   This structure is for use with the IOCTL_IVSHMEM_REGISTER_EVENT IOCTL

   Please Note:
     - The IVSHMEM driver has a hard limit of 32 events.
     - Events that are singleShot are released after they have been set.
     - At this time repeating events are only released when the driver device
       handle is closed, closing the event handle doesn't release it from the
       drivers list. While this won't cause a problem in the driver, it will
       cause you to run out of event slots.
 */
typedef struct IVSHMEM_EVENT
{
    UINT16  vector;     // the vector that triggers the event
    HANDLE  event;      // the event to trigger
    BOOLEAN singleShot; // set to TRUE if you want the driver to only trigger this event once
}
IVSHMEM_EVENT, *PIVSHMEM_EVENT;

#define IOCTL_IVSHMEM_REQUEST_PEERID CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_REQUEST_SIZE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_REQUEST_MMAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_RELEASE_MMAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_RING_DOORBELL  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_REGISTER_EVENT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
