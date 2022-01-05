/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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
#include <wayland-client.h>

#include "common/debug.h"
#include "common/locking.h"

#ifdef ENABLE_LIBDECOR
#include <libdecor.h>
#endif

#define EPOLL_EVENTS 10 // Maximum number of fds we can process at once in waylandWait

#ifndef ENABLE_LIBDECOR
static void waylandDisplayCallback(uint32_t events, void * opaque)
{
  if (events & EPOLLERR)
    wl_display_cancel_read(wlWm.display);
  else
    wl_display_read_events(wlWm.display);
  wl_display_dispatch_pending(wlWm.display);
}
#endif

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

#ifndef ENABLE_LIBDECOR
  wlWm.displayFd = wl_display_get_fd(wlWm.display);
  if (!waylandPollRegister(wlWm.displayFd, waylandDisplayCallback, NULL, EPOLLIN))
  {
    DEBUG_ERROR("Failed register display to epoll: %s", strerror(errno));
    return false;
  }
#endif

  return true;
}

void waylandWait(unsigned int time)
{
#ifdef ENABLE_LIBDECOR
  libdecor_dispatch(wlWm.libdecor, 0);
#else
  while (wl_display_prepare_read(wlWm.display))
    wl_display_dispatch_pending(wlWm.display);
  wl_display_flush(wlWm.display);
#endif

  struct epoll_event events[EPOLL_EVENTS];
  int count;
  if ((count = epoll_wait(wlWm.epollFd, events, EPOLL_EVENTS, time)) < 0)
  {
    if (errno != EINTR)
      DEBUG_INFO("epoll failed: %s", strerror(errno));
#ifndef ENABLE_LIBDECOR
    wl_display_cancel_read(wlWm.display);
#endif
    return;
  }

#ifndef ENABLE_LIBDECOR
  bool sawDisplay = false;
#endif
  for (int i = 0; i < count; ++i) {
    struct WaylandPoll * poll = events[i].data.ptr;
    if (!poll->removed)
      poll->callback(events[i].events, poll->opaque);
#ifndef ENABLE_LIBDECOR
    if (poll->fd == wlWm.displayFd)
      sawDisplay = true;
#endif
  }

#ifndef ENABLE_LIBDECOR
  if (!sawDisplay)
    wl_display_cancel_read(wlWm.display);
#endif

  INTERLOCKED_SECTION(wlWm.pollFreeLock,
  {
    struct WaylandPoll * node;
    struct WaylandPoll * temp;
    wl_list_for_each_safe(node, temp, &wlWm.pollFree, link)
    {
      wl_list_remove(&node->link);
      free(node);
    }
  });
}

static void waylandPollRemoveNode(struct WaylandPoll * node)
{
  INTERLOCKED_SECTION(wlWm.pollLock,
  {
    wl_list_remove(&node->link);
  });
}

bool waylandPollRegister(int fd, WaylandPollCallback callback, void * opaque, uint32_t events)
{
  struct WaylandPoll * node = malloc(sizeof(*node));
  if (!node)
    return false;

  node->fd       = fd;
  node->removed  = false;
  node->callback = callback;
  node->opaque   = opaque;

  INTERLOCKED_SECTION(wlWm.pollLock,
  {
    wl_list_insert(&wlWm.poll, &node->link);
  });

  if (epoll_ctl(wlWm.epollFd, EPOLL_CTL_ADD, fd, &(struct epoll_event) {
    .events = events,
    .data = (epoll_data_t) { .ptr = node },
  }) < 0)
  {
    waylandPollRemoveNode(node);
    free(node);
    return false;
  }

  return true;
}

bool waylandPollUnregister(int fd)
{
  struct WaylandPoll * node = NULL;
  INTERLOCKED_SECTION(wlWm.pollLock,
  {
    wl_list_for_each(node, &wlWm.poll, link)
    {
      if (node->fd == fd)
        break;
    }
  });

  if (!node)
  {
    DEBUG_ERROR("Attempt to unregister a fd that was not registered: %d", fd);
    return false;
  }

  node->removed = true;
  if (epoll_ctl(wlWm.epollFd, EPOLL_CTL_DEL, fd, NULL) < 0)
  {
    DEBUG_ERROR("Failed to unregistered from epoll: %s", strerror(errno));
    return false;
  }

  waylandPollRemoveNode(node);

  INTERLOCKED_SECTION(wlWm.pollFreeLock,
  {
    wl_list_insert(&wlWm.pollFree, &node->link);
  });
  return true;
}
