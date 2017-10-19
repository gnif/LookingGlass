#include <stdint.h>
#include <stdbool.h>

struct IVSHMEMInit
{
   int64_t version;
   int64_t clientID;
   int64_t unused;
   int64_t sharedFD;
};

bool ivshmem_connect(const char * unix_socket);
void ivshmem_close();