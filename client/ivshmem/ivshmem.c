/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "ivshmem.h"
#include "debug.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <sys/mman.h>

#define MAX_IRQS 32

struct IVSHMEMServer
{
   int64_t version;
   int64_t clientID;
   int     sharedFD;

   int     irqs[MAX_IRQS];
   int     irqCount;
};

struct IVSHMEMClient
{
  uint16_t clientID;

  int      irqs[MAX_IRQS];
  int      irqCount;

  struct IVSHMEMClient * last;
  struct IVSHMEMClient * next;
};

struct IVSHMEM
{
  bool                   connected;
  bool                   shutdown;
  int                    socket;
  struct IVSHMEMServer   server;
  struct IVSHMEMClient * clients;

  off_t  mapSize;
  void * map;
};

struct IVSHMEM ivshmem =
{
  .connected = false,
  .shutdown  = false,
  .socket    = -1
};

// ============================================================================
// internal functions

void ivshmem_cleanup();
bool ivshmem_read(void * buffer, const ssize_t size);
bool ivshmem_read_msg(int64_t * index, int *fd);
struct IVSHMEMClient * ivshmem_get_client(uint16_t clientID);
void ivshmem_remove_client(struct IVSHMEMClient * client);

// ============================================================================

bool ivshmem_connect(const char * unix_socket)
{
  ivshmem.shutdown = false;
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

  if (!ivshmem_read(&ivshmem.server.version, sizeof(ivshmem.server.version)))
  {
    DEBUG_ERROR("read protocol version failed");
    ivshmem_cleanup();
    return false;
  }

  if (ivshmem.server.version != 0)
  {
    DEBUG_ERROR("unsupported protocol version %ld", ivshmem.server.version);
    ivshmem_cleanup();
    return false;
  }

  if (!ivshmem_read(&ivshmem.server.clientID, sizeof(ivshmem.server.clientID)))
  {
    DEBUG_ERROR("read client id failed");
    ivshmem_cleanup();
    return false;
  }

  DEBUG_PROTO("Protocol : %ld", ivshmem.server.version );
  DEBUG_PROTO("Client ID: %ld", ivshmem.server.clientID);

  if (!ivshmem_read_msg(NULL, &ivshmem.server.sharedFD))
  {
    DEBUG_ERROR("failed to read shared memory file descriptor");
    ivshmem_cleanup();
    return false;
  }

  struct stat stat;
  if (fstat(ivshmem.server.sharedFD, &stat) != 0)
  {
    DEBUG_ERROR("failed to stat shared FD");
    ivshmem_cleanup();
    return false;
  }

  ivshmem.mapSize = stat.st_size;

  DEBUG_INFO("RAM Size : %ld", ivshmem.mapSize);
  ivshmem.map = mmap(
      NULL,
      stat.st_size,
      PROT_READ | PROT_WRITE,
      MAP_SHARED,
      ivshmem.server.sharedFD,
      0);

  if (!ivshmem.map)
  {
    DEBUG_ERROR("failed to map memory");
    ivshmem_cleanup();
    return false;
  }

  return true;
}

// ============================================================================

void ivshmem_cleanup()
{
  struct IVSHMEMClient * client, * next;
  client = ivshmem.clients;
  while(client)
  {
    for(int i = 0; i < client->irqCount; ++i)
      close(client->irqs[i]);

    next = client->next;
    free(client);
    client = next;
  }
  ivshmem.clients = NULL;

  for(int i = 0; i < ivshmem.server.irqCount; ++i)
    close(ivshmem.server.irqs[i]);
  ivshmem.server.irqCount = 0;

  if (ivshmem.map)
    munmap(ivshmem.map, ivshmem.mapSize);
  ivshmem.map     = NULL;
  ivshmem.mapSize = 0;

  if (ivshmem.socket >= 0)
  {
    ivshmem.shutdown = true;
    shutdown(ivshmem.socket, SHUT_RDWR);
    close(ivshmem.socket);
    ivshmem.socket = -1;
  }

  ivshmem.connected = false;
}

// ============================================================================

void ivshmem_disconnect()
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

// ============================================================================

bool ivshmem_read_msg(int64_t * index, int * fd)
{
  if (!ivshmem.connected)
  {
    DEBUG_ERROR("not connected");
    return false;
  }

  struct msghdr msg;
  struct iovec  iov[1];
  union {
    struct cmsghdr cmsg;
    char control[CMSG_SPACE(sizeof(int))];
  } msg_control;

  int64_t tmp;
  if (!index)
    index = &tmp;

  iov[0].iov_base = index;
  iov[0].iov_len  = sizeof(*index);

  memset(&msg, 0, sizeof(msg));
  msg.msg_iov        = iov;
  msg.msg_iovlen     = 1;
  msg.msg_control    = &msg_control;
  msg.msg_controllen = sizeof(msg_control);

  int ret = recvmsg(ivshmem.socket, &msg, 0);
  if (ret < sizeof(*index))
  {
    if (!ivshmem.shutdown)
      DEBUG_ERROR("failed to read message\n");
    return false;
  }

  if (ret == 0)
  {
    if (!ivshmem.shutdown)
      DEBUG_ERROR("lost connetion to server\n");
    return false;
  }

  if (!fd)
    return true;

  *fd = -1;
  struct cmsghdr *cmsg;
  for(cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
  {
    if (cmsg->cmsg_len   != CMSG_LEN(sizeof(int)) ||
        cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type  != SCM_RIGHTS)
    {
      continue;
    }

    memcpy(fd, CMSG_DATA(cmsg), sizeof(int));
  }

  return true;
}

// ============================================================================

uint16_t ivshmem_get_id()
{
  if (!ivshmem.connected)
  {
    DEBUG_ERROR("not connected");
    return -1;
  }

  return ivshmem.server.clientID;
}

// ============================================================================

void * ivshmem_get_map()
{
  if (!ivshmem.connected)
  {
    DEBUG_ERROR("not connected");
    return NULL;
  }

  if (!ivshmem.map)
  {
    DEBUG_ERROR("not mapped");
    return NULL;
  }

  return ivshmem.map;
}

// ============================================================================

size_t ivshmem_get_map_size()
{
  if (!ivshmem.connected)
  {
    DEBUG_ERROR("not connected");
    return 0;
  }

  if (!ivshmem.map)
  {
    DEBUG_ERROR("not mapped");
    return 0;
  }

  return ivshmem.mapSize;
}

// ============================================================================

struct IVSHMEMClient * ivshmem_get_client(uint16_t clientID)
{
  struct IVSHMEMClient * client = NULL;

  if (ivshmem.clients == NULL)
  {
    client = (struct IVSHMEMClient *)malloc(sizeof(struct IVSHMEMClient));
    client->clientID = clientID;
    client->last     = NULL;
    client->next     = NULL;
    client->irqCount = 0;
    ivshmem.clients  = client;
    return client;
  }

  client = ivshmem.clients;
  while(client)
  {
    if (client->clientID == clientID)
      return client;
    client = client->next;
  }

  client = (struct IVSHMEMClient *)malloc(sizeof(struct IVSHMEMClient));
  client->clientID   = clientID;
  client->last       = NULL;
  client->next       = ivshmem.clients;
  client->irqCount   = 0;
  client->next->last = client;
  ivshmem.clients    = client;

  return client;
}

// ============================================================================

void ivshmem_remove_client(struct IVSHMEMClient * client)
{
  if (client->last)
    client->last->next = client->next;

  if (client->next)
    client->next->last = client->last;

  if (ivshmem.clients == client)
    ivshmem.clients = client->next;

  free(client);
}

// ============================================================================

bool ivshmem_process()
{
  int64_t index;
  int     fd;

  if (!ivshmem_read_msg(&index, &fd))
  {
    if (!ivshmem.shutdown)
      DEBUG_ERROR("failed to read message");
    return false;
  }

  if (index == -1)
  {
    DEBUG_ERROR("invalid index -1");
    return false;
  }

  if (index > 0xFFFF)
  {
    DEBUG_ERROR("invalid index > 0xFFFF");
    return false;
  }

  if (index == ivshmem.server.clientID)
  {
    if (fd == -1)
    {
      DEBUG_ERROR("server sent disconnect");
      return false;
    }

    if (ivshmem.server.irqCount == MAX_IRQS)
    {
      DEBUG_WARN("maximum IRQs reached, closing extra");
      close(fd);
      return true;
    }

    ivshmem.server.irqs[ivshmem.server.irqCount++] = fd;
    return true;
  }

  struct IVSHMEMClient * client = ivshmem_get_client(index);
  if (!client)
  {
    DEBUG_ERROR("failed to get/create client record");
    return false;
  }

  if (fd == -1)
  {
    DEBUG_PROTO("remove client %u", client->clientID);
    ivshmem_remove_client(client);
    return true;
  }

  if (client->irqCount == MAX_IRQS)
  {
    DEBUG_WARN("maximum client IRQs reached, closing extra");
    close(fd);
    return true;
  }

  client->irqs[client->irqCount++] = fd;
  return true;
}

// ============================================================================

enum IVSHMEMWaitResult ivshmem_wait_irq(uint16_t vector, unsigned int timeout)
{
  if (vector > ivshmem.server.irqCount - 1)
    return false;

  int fd = ivshmem.server.irqs[vector];
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);

  struct timeval tv;
  tv.tv_sec  = timeout / 1000000L;
  tv.tv_usec = timeout % 1000000L;

  while(true)
  {
    int ret = select(fd+1, &fds, NULL, NULL, &tv);
    if (ret < 0)
    {
      if (errno == EINTR)
        continue;

      DEBUG_ERROR("select error");
      break;
    }

    if (ret == 0)
      return IVSHMEM_WAIT_RESULT_TIMEOUT;

    if (FD_ISSET(fd, &fds))
    {
      uint64_t kick;
      int unused = read(fd, &kick, sizeof(kick));
      (void)unused;
      return IVSHMEM_WAIT_RESULT_OK;
    }
  }

  return IVSHMEM_WAIT_RESULT_ERROR;
}

// ============================================================================

bool ivshmem_kick_irq(uint16_t clientID, uint16_t vector)
{
  struct IVSHMEMClient * client = ivshmem_get_client(clientID);
  if (!client)
  {
    DEBUG_ERROR("invalid client");
    return false;
  }

  if (vector > client->irqCount - 1)
  {
    DEBUG_ERROR("invalid vector for client");
    return false;
  }

  int fd = client->irqs[vector];
  uint64_t kick = ivshmem.server.clientID;
  if (write(fd, &kick, sizeof(kick)) == sizeof(kick))
    return true;

  DEBUG_ERROR("failed to send kick");
  return false;
}