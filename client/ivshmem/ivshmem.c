#include "ivshmem.h"

#define DEBUG
#define DEBUG_IVSHMEM
#include "debug.h"

#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>

#include <sys/socket.h>
#include <sys/un.h>

struct IVSHMEM
{
  bool connected;
  int  socket;
};

struct IVSHMEM ivshmem =
{
  .connected = false,
  .socket    = -1
};

// ============================================================================
// internal functions

void ivshmem_cleanup();
bool ivshmem_read(void * buffer, const ssize_t size);

// ============================================================================

bool ivshmem_connect(const char * unix_socket)
{
  ivshmem.socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ivshmem.socket < 0)
  {
    DEBUG_ERROR("socket creation failed");
    return false;
  }

  struct sockaddr_un addr;
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, unix_socket, sizeof(addr.sun_path));

  if (connect(ivshmem.socket, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) < 0)
  {
    DEBUG_ERROR("socket connect failed");
    ivshmem_cleanup();
    return false;
  }

  ivshmem.connected = true;

  struct IVSHMEMInit init;
  if (!ivshmem_read(&init.version, sizeof(init.version)))
  {
    DEBUG_ERROR("read protocol version failed");
    ivshmem_cleanup();
    return false;
  }

  if (init.version != 0)
  {
    DEBUG_ERROR("unsupported protocol version %ld", init.version);
    ivshmem_cleanup();
    return false;
  }

  if (!ivshmem_read(&init.clientID, sizeof(init.clientID)))
  {
    DEBUG_ERROR("read client id failed");
    ivshmem_cleanup();
    return false;
  }

  if (!ivshmem_read(&init.unused, sizeof(init.unused)))
  {
    DEBUG_ERROR("read unused failed");
    ivshmem_cleanup();
    return false;
  }

  if (!ivshmem_read(&init.sharedFD, sizeof(init.sharedFD)))
  {
    DEBUG_ERROR("read shared memory file descriptor failed");
    ivshmem_cleanup();
    return false;
  }

  DEBUG_PROTO("Protocol : %ld", init.version );
  DEBUG_PROTO("Client ID: %ld", init.clientID);
  DEBUG_PROTO("Unused   : %ld", init.unused  );
  DEBUG_PROTO("Shared FD: %ld", init.sharedFD);

  return true;
}

// ============================================================================

void ivshmem_cleanup()
{
  if (ivshmem.socket >= 0)
  {
    close(ivshmem.socket);
    ivshmem.socket = -1;
  }

  ivshmem.connected = false;
}

// ============================================================================

void ivshmem_close()
{
  if (!ivshmem.connected)
  {
    DEBUG_WARN("socket not connected");
    return;
  }

  ivshmem_cleanup();
}

// ============================================================================

bool ivshmem_read(void * buffer, const ssize_t size)
{
  if (!ivshmem.connected)
  {
    DEBUG_ERROR("not connected");
    return false;
  }

  ssize_t len = read(ivshmem.socket, buffer, size);
  if (len != size)
  {
    DEBUG_ERROR("incomplete read");
    return false;
  }

  return true;
}