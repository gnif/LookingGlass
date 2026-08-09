/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "wayland.h"

#include <stdbool.h>
#include <string.h>

#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <wayland-client.h>

#include "common/debug.h"
#include "common/locking.h"

bool waylandPollInit(void)
{
  wlWm.epollFd = epoll_create1(EPOLL_CLOEXEC);
  if (wlWm.epollFd < 0)
  {
    DEBUG_ERROR("Failed to initialize epoll: %s", strerror(errno));
    return false;
  }

  wl_list_init(&wlWm.poll);
  wl_list_init(&wlWm.pollFree);
  LG_LOCK_INIT(wlWm.pollLock);
  LG_LOCK_INIT(wlWm.pollFreeLock);

  return wlWm.desktop->pollInit(wlWm.display);
}

static void waylandPollDestroyNode(struct WaylandPoll * node)
{
  if (node->cleanup)
    node->cleanup(node->opaque);
  free(node);
}

static void waylandPollDestroyList(struct wl_list * list)
{
  struct WaylandPoll * node;
  struct WaylandPoll * temp;
  wl_list_for_each_safe(node, temp, list, link)
  {
    wl_list_remove(&node->link);
    waylandPollDestroyNode(node);
  }
}

static void waylandPollTakeList(struct wl_list * source,
    struct wl_list * destination)
{
  struct WaylandPoll * node;
  struct WaylandPoll * temp;
  wl_list_for_each_safe(node, temp, source, link)
  {
    wl_list_remove(&node->link);
    wl_list_insert(destination, &node->link);
  }
}

void waylandWait(unsigned int time)
{
  INTERLOCKED_SECTION(wlWm.pollFreeLock,
  {
    ++wlWm.pollWaiters;
  });

  wlWm.desktop->pollWait(wlWm.display, wlWm.epollFd, time);

  struct wl_list retired;
  wl_list_init(&retired);
  INTERLOCKED_SECTION(wlWm.pollFreeLock,
  {
    if (--wlWm.pollWaiters == 0)
      waylandPollTakeList(&wlWm.pollFree, &retired);
  });
  waylandPollDestroyList(&retired);
}

void waylandPollFree(void)
{
  if (wlWm.epollFd >= 0)
  {
    close(wlWm.epollFd);
    wlWm.epollFd = -1;
  }

  struct wl_list active;
  struct wl_list retired;
  wl_list_init(&active);
  wl_list_init(&retired);
  INTERLOCKED_SECTION(wlWm.pollLock,
  {
    waylandPollTakeList(&wlWm.poll, &active);
  });

  INTERLOCKED_SECTION(wlWm.pollFreeLock,
  {
    waylandPollTakeList(&wlWm.pollFree, &retired);
  });
  waylandPollDestroyList(&active);
  waylandPollDestroyList(&retired);

  LG_LOCK_FREE(wlWm.pollFreeLock);
  LG_LOCK_FREE(wlWm.pollLock);
}

bool waylandPollRegisterWithCleanup(int fd, WaylandPollCallback callback,
    void * opaque, WaylandPollCleanup cleanup, uint32_t events)
{
  struct WaylandPoll * node = malloc(sizeof(*node));
  if (!node)
    return false;

  node->fd       = fd;
  node->callback = callback;
  node->cleanup  = cleanup;
  node->opaque   = opaque;
  atomic_init(&node->removed, false);

  LG_LOCK(wlWm.pollLock);
  if (epoll_ctl(wlWm.epollFd, EPOLL_CTL_ADD, fd, &(struct epoll_event) {
    .events = events,
    .data = (epoll_data_t) { .ptr = node },
  }) < 0)
  {
    LG_UNLOCK(wlWm.pollLock);
    free(node);
    return false;
  }
  wl_list_insert(&wlWm.poll, &node->link);
  LG_UNLOCK(wlWm.pollLock);

  return true;
}

bool waylandPollRegister(int fd, WaylandPollCallback callback,
    void * opaque, uint32_t events)
{
  return waylandPollRegisterWithCleanup(
      fd, callback, opaque, NULL, events);
}

bool waylandPollUnregister(int fd)
{
  struct WaylandPoll * node = NULL;
  LG_LOCK(wlWm.pollLock);
  struct WaylandPoll * current;
  wl_list_for_each(current, &wlWm.poll, link)
  {
    if (current->fd == fd)
    {
      node = current;
      atomic_store_explicit(
          &node->removed, true, memory_order_release);
      break;
    }
  }

  if (!node)
  {
    LG_UNLOCK(wlWm.pollLock);
    DEBUG_ERROR("Attempt to unregister a fd that was not registered: %d", fd);
    return false;
  }

  if (epoll_ctl(wlWm.epollFd, EPOLL_CTL_DEL, fd, NULL) < 0)
  {
    LG_UNLOCK(wlWm.pollLock);
    DEBUG_ERROR("Failed to unregister from epoll: %s", strerror(errno));
    return false;
  }
  wl_list_remove(&node->link);
  LG_UNLOCK(wlWm.pollLock);

  INTERLOCKED_SECTION(wlWm.pollFreeLock,
  {
    wl_list_insert(&wlWm.pollFree, &node->link);
  });
  return true;
}
