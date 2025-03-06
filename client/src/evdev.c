/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#include "evdev.h"
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <linux/input.h>
#include <string.h>
#include <sys/epoll.h>

#include "app.h"

#include "common/debug.h"
#include "common/option.h"
#include "common/stringlist.h"
#include "common/thread.h"

typedef struct
{
  char * path;
  int    fd;
  bool   grabbed;
}
EvdevDevice;

struct EvdevState
{
  char        * deviceList;
  EvdevDevice * devices;
  int           deviceCount;
  int           epoll;
  LGThread    * thread;
  bool          grabbed;
};

static struct EvdevState state = {};

static struct Option options[] =
{
  {
    .module         = "input",
    .name           = "evdev",
    .description    = "csv list of evdev keyboard devices to use "
      "for capture mode (ie: /dev/input/by-id/usb-some_device-event-kbd)",
    .type           = OPTION_TYPE_STRING,
    .value.x_string = NULL,
  },
  {0}
};

void evdev_earlyInit(void)
{
  option_register(options);
}

static int evdev_thread(void * opaque)
{
  struct epoll_event * events = alloca(sizeof(*events) * state.deviceCount);
  DEBUG_INFO("evdev_thread Started");
  while(app_isRunning())
  {
    int waiting = epoll_wait(state.epoll, events, state.deviceCount, 100);
    for(int i = 0; i < waiting; ++i)
    {
      struct input_event ev;
      size_t n = read(events[i].data.fd, &ev, sizeof(ev));
      if (n != sizeof(ev))
      {
        DEBUG_WARN("Failed to read evdev event");
        continue;
      }

      if (!state.grabbed || ev.type != EV_KEY)
        continue;

      if (ev.value == 1)
        app_handleKeyPress(ev.code);
      else if (ev.value == 0)
        app_handleKeyRelease(ev.code);
    }
  }
  DEBUG_INFO("evdev_thread Stopped");
  return 0;
}

bool evdev_start(void)
{
  const char * deviceList = option_get_string("input", "evdev");
  if (!deviceList)
    return false;

  state.deviceList = strdup(deviceList);
  StringList sl = stringlist_new(false);

  char * token = strtok(state.deviceList, ",");
  while(token != NULL)
  {
    stringlist_push(sl, token);
    token = strtok(NULL, ",");
  }

  state.deviceCount = stringlist_count(sl);
  state.devices     = calloc(state.deviceCount, sizeof(*state.devices));
  for(int i = 0; i < state.deviceCount; ++i)
    state.devices[i].path = stringlist_at(sl, i);
  stringlist_free(&sl);

  // nothing to do if there are no configured devices
  if (state.deviceCount == 0)
    return false;

  state.epoll = epoll_create1(0);
  if (state.epoll < 0)
  {
    DEBUG_ERROR("Failed to create epoll (%s)", strerror(errno));
    return false;
  }

  for(int i = 0; i < state.deviceCount; ++i)
  {
    EvdevDevice * device = &state.devices[i];
    device->fd = open(device->path, O_RDWR);
    if (device->fd < 0)
    {
      DEBUG_ERROR("Unable to open %s (%s)", device->path, strerror(errno));
      return false;
    }

    struct epoll_event event =
    {
      .events  = EPOLLIN,
      .data.fd = device->fd
    };

    if (epoll_ctl(state.epoll, EPOLL_CTL_ADD, device->fd, &event) != 0)
    {
      DEBUG_ERROR("Failed to add fd to epoll");
      return false;
    }

    DEBUG_INFO("Opened: %s", device->path);
  }

  if (!lgCreateThread("Evdev", evdev_thread, NULL, &state.thread))
  {
    DEBUG_ERROR("Failed to create the evdev thread");
    return false;
  }

  return true;
}

void evdev_stop(void)
{
  if (state.deviceList)
  {
    free(state.deviceList);
    state.deviceList = NULL;
  }

  if (state.thread)
  {
    lgJoinThread(state.thread, NULL);
    state.thread = NULL;
  }

  if (state.epoll >= 0)
  {
    close(state.epoll);
    state.epoll = 0;
  }

  for(EvdevDevice * device = state.devices; device->path; ++device)
  {
    if (device->fd <= 0)
      continue;

    close(device->fd);
    device->fd = 0;
  }
}

void evdev_grabKeyboard(void)
{
  for(EvdevDevice * device = state.devices; device->path; ++device)
  {
    if (device->fd <= 0 || device->grabbed)
      continue;

    if (ioctl(device->fd, EVIOCGRAB, (void *)1) < 0)
    {
      DEBUG_ERROR("EVIOCGRAB=1 failed: %s", strerror(errno));
      continue;
    }

    DEBUG_INFO("Grabbed %s", device->path);
    device->grabbed = true;
  }
  state.grabbed = true;
}

void evdev_ungrabKeyboard(void)
{
  for(EvdevDevice * device = state.devices; device->path; ++device)
  {
    if (device->fd <= 0 || !device->grabbed)
      continue;

    if (ioctl(device->fd, EVIOCGRAB, (void *)0) < 0)
    {
      DEBUG_ERROR("EVIOCGRAB=0 failed: %s", strerror(errno));
      continue;
    }

    DEBUG_INFO("Ungrabbed %s", device->path);
    device->grabbed = false;
  }
  state.grabbed = false;
}
