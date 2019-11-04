/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#include "porthole/client.h"
#include "common/objectlist.h"
#include "common/debug.h"

#include "../phmsg.h"

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>

typedef struct
{
  uint32_t  id;
  int       fd;
  int       refcount;
  uint8_t * map;
  size_t    size;
}
SharedFD;

typedef struct
{
  SharedFD * sfd;
  uint64_t   addr;
  uint32_t   size;
}
Segment;

typedef struct
{
  uint32_t   id;
  ObjectList segments;
  size_t     size;
}
Mapping;

struct PortholeClient
{
  int                 socket;
  PortholeMapEvent    map_cb;
  PortholeUnmapEvent  unmap_cb;
  PortholeDisconEvent discon_cb;
  ObjectList          fds;
  ObjectList          intmaps;
  Mapping           * current;
  ObjectList          maps;
  bool                running;
  pthread_t           thread;
  bool                thread_valid;
};

// forwards
static void * porthole_socket_thread(void * opaque);
static void porthole_free_map(Mapping * map);
static void porthole_sharedfd_free_handler(void * opaque);
static void porthole_intmaps_free_handler(void * opaque);
static void porthole_segment_free_handler(void * opaque);
static Mapping * porthole_intmap_new();
static void porthole_sharedfd_new(PortholeClient handle, const uint32_t id, const int fd);
static void porthole_segment_new(ObjectList fds, Mapping *map, const uint32_t fd_id, const uint64_t addr, const uint32_t size);
static void porthole_do_map(PortholeClient handle, Mapping * map, const uint32_t type);


// implementation
bool porthole_client_open(
  PortholeClient    * handle,
  const char        * socket_path,
  PortholeMapEvent    map_cb,
  PortholeUnmapEvent  unmap_cb,
  PortholeDisconEvent discon_cb)
{
  assert(handle);

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1)
  {
    DEBUG_ERROR("Failed to create a unix socket");
    return false;
  }

  struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);

  if (connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) == -1)
  {
    DEBUG_ERROR("Failed to connect to the socket");
    close(fd);
    return false;
  }

  *handle = (PortholeClient)calloc(sizeof(struct PortholeClient), 1);

  (*handle)->socket    = fd;
  (*handle)->map_cb    = map_cb;
  (*handle)->unmap_cb  = unmap_cb;
  (*handle)->discon_cb = discon_cb;
  (*handle)->fds       = objectlist_new(porthole_sharedfd_free_handler);
  (*handle)->intmaps   = objectlist_new(porthole_intmaps_free_handler);
  (*handle)->maps      = objectlist_new(objectlist_free_item);
  (*handle)->running   = true;

  if (pthread_create(&(*handle)->thread, NULL, porthole_socket_thread, *handle) != 0)
  {
    DEBUG_ERROR("Failed to create porthole socket thread");
    porthole_client_close(handle);
    return false;
  }

  (*handle)->thread_valid = true;
  return true;
}

void porthole_client_close(PortholeClient * handle)
{
  assert(handle && *handle);

  if ((*handle)->thread_valid)
  {
    (*handle)->running = false;
    pthread_join((*handle)->thread, NULL);
  }

  close((*handle)->socket);

  if ((*handle)->current)
    porthole_free_map((*handle)->current);

  objectlist_free(&(*handle)->maps   );
  objectlist_free(&(*handle)->intmaps);
  objectlist_free(&(*handle)->fds    );

  free(*handle);
  *handle = NULL;
}

static void * porthole_socket_thread(void * opaque)
{
  PortholeClient handle = (PortholeClient)opaque;
  DEBUG_INFO("Porthole socket thread started");

  while(handle->running)
  {
    PHMsg msg;
    struct iovec io =
    {
      .iov_base = &msg,
      .iov_len  = sizeof(msg)
    };

    char buffer[256] = {0};
    struct msghdr msghdr =
    {
      .msg_iov       = &io,
      .msg_iovlen    = 1,
      .msg_control    = &buffer,
      .msg_controllen = sizeof(buffer)
    };

    if (recvmsg(handle->socket, &msghdr, 0) < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue;

      DEBUG_ERROR("Failed to recieve the message");
      if (handle->discon_cb)
        handle->discon_cb();
      break;
    }

    switch(msg.msg)
    {
      case PH_MSG_MAP:
        if (handle->current)
        {
          DEBUG_WARN("Started a new map before finishing the last one");
          porthole_free_map(handle->current);
        }

        handle->current = porthole_intmap_new();
        break;

      case PH_MSG_FD:
      {
        struct cmsghdr * cmsg = CMSG_FIRSTHDR(&msghdr);
        int            * fds  = (int *)CMSG_DATA(cmsg);
        porthole_sharedfd_new(handle, msg.u.fd.id, fds[0]);
        break;
      }

      case PH_MSG_SEGMENT:
      {
        if (!handle->current)
          DEBUG_FATAL("Segment sent before map, this is a bug in the guest porthole device or driver");

        porthole_segment_new(
          handle->fds,
          handle->current,
          msg.u.segment.fd_id,
          msg.u.segment.addr,
          msg.u.segment.size
        );
        break;
      }

      case PH_MSG_FINISH:
        if (!handle->current)
          DEBUG_FATAL("Finished map before starting one");

        handle->current->id = msg.u.finish.id;
        objectlist_push(handle->intmaps, handle->current);
        porthole_do_map(handle, handle->current, msg.u.finish.type);
        handle->current = NULL;
        break;

      case PH_MSG_UNMAP:
      {
        // notify the application of the unmap
        handle->unmap_cb(msg.u.unmap.id);

        // remove the PortholeMap object
        unsigned int count = objectlist_count(handle->maps);
        for(unsigned int i = 0; i < count; ++i)
        {
          PortholeMap *m = (PortholeMap *)objectlist_at(handle->maps, i);
          if (m->id == msg.u.unmap.id)
          {
            objectlist_remove(handle->maps, i);
            break;
          }
        }

        // remove the internal mapping object
        count = objectlist_count(handle->intmaps);
        for(unsigned int i = 0; i < count; ++i)
        {
          Mapping *m = (Mapping *)objectlist_at(handle->intmaps, i);
          if (m->id == msg.u.unmap.id)
          {
            objectlist_remove(handle->intmaps, i);
            break;
          }
        }

        // reply to the guest to allow it to continue
        uint32_t reply = PH_MSG_UNMAP;
        msghdr.msg_controllen = 0;
        io.iov_base = &reply;
        io.iov_len  = sizeof(reply);
        if (sendmsg(handle->socket, &msghdr, 0) < 0)
        {
          DEBUG_ERROR("Failed to respond to the guest");
          handle->running = false;
          if (handle->discon_cb)
            handle->discon_cb();
        }

        break;
      }
    }
  }

  handle->running = false;
  DEBUG_INFO("Porthole socket thread stopped");
  return NULL;
}

static void porthole_sharedfd_new(PortholeClient handle, const uint32_t id, const int fd)
{
  struct stat st;
  fstat(fd, &st);

  SharedFD * sfd = (SharedFD *)calloc(sizeof(SharedFD), 1);
  sfd->id   = id;
  sfd->fd   = fd;
  sfd->size = st.st_size;

  DEBUG_INFO("Guest FD ID %u (FD:%d, Size:%lu)", sfd->id, sfd->fd, sfd->size);
  objectlist_push(handle->fds, sfd);
}

static void porthole_sharedfd_inc_ref(SharedFD * sfd)
{
  if (sfd->refcount == 0)
  {
    sfd->map = mmap(NULL, sfd->size, PROT_READ | PROT_WRITE, MAP_SHARED, sfd->fd, 0);
    if(!sfd->map)
      DEBUG_FATAL("Failed to map shared memory");
  }
  ++sfd->refcount;
}

static void porthole_sharedfd_dec_ref(SharedFD * sfd)
{
  if (sfd->refcount == 0)
    return;

  munmap(sfd->map, sfd->size);
  sfd->map = NULL;

  --sfd->refcount;
}

static void porthole_sharedfd_free_handler(void * opaque)
{
  SharedFD * sfd = (SharedFD *)opaque;

  if (sfd->map)
  {
    munmap(sfd->map, sfd->size);
    sfd->map = NULL;
  }

  close(sfd->fd);
  free(sfd);
}

static Mapping * porthole_intmap_new()
{
  Mapping * map = (Mapping *)calloc(sizeof(Mapping), 1);
  map->segments = objectlist_new(porthole_segment_free_handler);
  return map;
}

static void porthole_free_map(Mapping * map)
{
  objectlist_free(&map->segments);
  free(map);
}

static void porthole_intmaps_free_handler(void * opaque)
{
  porthole_free_map((Mapping *)opaque);
}

static void porthole_segment_new(ObjectList fds, Mapping *map, const uint32_t fd_id, const uint64_t addr, const uint32_t size)
{
  Segment * seg = calloc(sizeof(Segment), 1);
  seg->addr = addr;
  seg->size = size;

  const unsigned int count = objectlist_count(fds);
  for(unsigned int i = 0; i < count; ++i)
  {
    SharedFD *sfd = (SharedFD*)objectlist_at(fds, i);
    if (sfd->id == fd_id)
    {
      seg->sfd = sfd;
      break;
    }
  }

  if (!seg->sfd)
    DEBUG_FATAL("Unable to find the FD for the segment, this is a bug in the porthole device!");

  map->size += size;
  porthole_sharedfd_inc_ref(seg->sfd);
  objectlist_push(map->segments, seg);
}

static void porthole_segment_free_handler(void * opaque)
{
  Segment * seg = (Segment *)opaque;
  porthole_sharedfd_dec_ref(seg->sfd);
  free(seg);
}

static void porthole_do_map(PortholeClient handle, Mapping * map, const uint32_t type)
{
  const unsigned int count = objectlist_count(map->segments);

  PortholeMap *m = calloc(sizeof(PortholeMap) + sizeof(PortholeSegment) * count, 1);

  m->id           = map->id;
  m->size         = map->size;
  m->num_segments = count;

  for(unsigned int i = 0; i < count; ++i)
  {
    Segment * seg = (Segment *)objectlist_at(map->segments, i);
    m->segments[i].size = seg->size;
    m->segments[i].data = seg->sfd->map + seg->addr;
  }

  objectlist_push(handle->maps, m);
  handle->map_cb(type, m);
}