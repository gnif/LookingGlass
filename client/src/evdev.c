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

#include "app_internal.h"
#include "core.h"
#include "main.h"

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
  bool          exclusive;
  int           keys[KEY_MAX];

  void (*dsGrabKeyboard)(void);
  void (*dsUngrabKeyboard)(void);

  int           epoll;
  LGThread    * thread;
  bool          grabbed;

  enum
  {
    PENDING_NONE,
    PENDING_GRAB,
    PENDING_UNGRAB
  }
  pending;
};

static struct EvdevState state = {};

static struct Option options[] =
{
  {
    .module         = "input",
    .name           = "evdev",
    .description    = "csv list of evdev input devices to use "
      "for capture mode (ie: /dev/input/by-id/usb-some_device-event-kbd)",
    .type           = OPTION_TYPE_STRING,
    .value.x_string = NULL,
  },
  {
    .module       = "input",
    .name         = "evdevExclusive",
    .description  = "Only use evdev devices for input when in capture mode",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = true
  },
  {0}
};

void evdev_earlyInit(void)
{
  option_register(options);
}

static bool evdev_grabDevice(EvdevDevice * device)
{
  if (device->grabbed)
    return true;

  if (ioctl(device->fd, EVIOCGRAB, (void *)1) < 0)
  {
    DEBUG_ERROR("EVIOCGRAB=1 failed: %s", strerror(errno));
    return false;
  }

  DEBUG_INFO("Grabbed %s", device->path);
  device->grabbed = true;
  return true;
}

static bool evdev_openDevice(EvdevDevice * device, bool quiet)
{
  device->fd = open(device->path, O_RDWR);
  if (device->fd < 0)
  {
    if (errno != ENOENT || !quiet)
      DEBUG_ERROR("Unable to open %s (%s)", device->path, strerror(errno));
    goto err;
  }

  struct epoll_event event =
  {
    .events   = EPOLLIN,
    .data.ptr = device
  };

  if (epoll_ctl(state.epoll, EPOLL_CTL_ADD, device->fd, &event) != 0)
  {
    DEBUG_ERROR("Failed to add fd to epoll");
    goto err;
  }

  DEBUG_INFO("Opened: %s", device->path);

  if (state.grabbed)
    evdev_grabDevice(device);

  return true;

err:
  close(device->fd);
  device->fd = 0;
  return false;
}

static int evdev_thread(void * opaque)
{
  struct epoll_event * events = alloca(sizeof(*events) * state.deviceCount);
  struct input_event msgs[256];

  DEBUG_INFO("evdev_thread Started");
  while(app_isRunning())
  {
    int openDevices = 0;
    for(int i = 0; i < state.deviceCount; ++i)
    {
      EvdevDevice * dev = &state.devices[i];
      if (dev->fd <= 0)
      {
        if (evdev_openDevice(dev, true))
          ++openDevices;
      }
      else
        ++openDevices;
    }

    if (openDevices == 0)
    {
      usleep(1000);
      continue;
    }

    int waiting = epoll_wait(state.epoll, events, state.deviceCount, 100);
    for(int i = 0; i < waiting; ++i)
    {
      EvdevDevice * device = (EvdevDevice *)events[i].data.ptr;
      int n = read(device->fd, msgs, sizeof(msgs));
      if (n < 0)
      {
        if (errno == ENODEV)
        {
          DEBUG_WARN("Device was removed: %s", device->path);
          epoll_ctl(state.epoll, EPOLL_CTL_DEL, device->fd, NULL);
          close(device->fd);
          device->fd = 0;
          device->grabbed = false;
          continue;
        }

        DEBUG_WARN("Failed to read evdev event: %s (%s)",
            device->path, strerror(errno));

        continue;
      }

      if (n % sizeof(*msgs) != 0)
        DEBUG_WARN("Incomplete evdev read: %s", device->path);

      bool grabbed = state.grabbed;

      int    count = n / sizeof(*msgs);
      struct input_event *ev = msgs;
      int mouseX = 0, mouseY = 0;

      for(int i = 0; i < count; ++i, ++ev)
      {
        switch(ev->type)
        {
          case EV_KEY:
          {
            bool isMouseBtn = ev->code >= BTN_MOUSE && ev->code <= BTN_BACK;
            static const int mouseBtnMap[] = {1, 3, 2, 6, 7, 0, 0};

            if (ev->value == 1)
            {
              ++state.keys[ev->code];

              if (grabbed && state.keys[ev->code] == 1)
              {
                if (isMouseBtn)
                  app_handleButtonPress(mouseBtnMap[ev->code - BTN_MOUSE]);
                else
                  app_handleKeyPressInternal(ev->code);
              }
            }
            else if (ev->value == 0 && --state.keys[ev->code] <= 0)
            {
              state.keys[ev->code] = 0;

              if (state.pending == PENDING_GRAB)
              {
                state.pending = PENDING_NONE;
                evdev_grabKeyboard();
              }
              else if (state.pending == PENDING_UNGRAB)
              {
                state.pending = PENDING_NONE;
                evdev_ungrabKeyboard();
              }

              if (grabbed)
              {
                if (isMouseBtn)
                  app_handleButtonRelease(mouseBtnMap[ev->code - BTN_MOUSE]);
                else
                  app_handleKeyReleaseInternal(ev->code);
              }
            }
            break;
          }

           case EV_REL:
            if (!grabbed)
              continue;

            switch(ev->code)
            {
              case REL_X:
                mouseX += ev->value;
                break;

              case REL_Y:
                mouseY += ev->value;
                break;

              case REL_WHEEL:
              {
                int btn;
                if (ev->value > 0)
                  btn = 4;
                else
                  btn = 5;

                app_handleButtonPress  (btn);
                app_handleButtonRelease(btn);
                break;
              }
            }
            break;
        }
      }

      if (mouseX != 0 || mouseY != 0)
        core_handleMouseGrabbed(mouseX, mouseY);
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

  state.exclusive = option_get("input", "evdevExclusive");

  state.epoll = epoll_create1(0);
  if (state.epoll < 0)
  {
    DEBUG_ERROR("Failed to create epoll (%s)", strerror(errno));
    return false;
  }

  for(int i = 0; i < state.deviceCount; ++i)
  {
    EvdevDevice * device = &state.devices[i];
    if (!evdev_openDevice(device, false))
      return false;
  }

  if (!lgCreateThread("Evdev", evdev_thread, NULL, &state.thread))
  {
    DEBUG_ERROR("Failed to create the evdev thread");
    return false;
  }

   //hook the display server's grab methods
   state.dsGrabKeyboard       = g_state.ds->grabKeyboard;
   state.dsUngrabKeyboard     = g_state.ds->ungrabKeyboard;
   g_state.ds->grabKeyboard   = &evdev_grabKeyboard;
   g_state.ds->ungrabKeyboard = &evdev_ungrabKeyboard;

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
  if (state.grabbed)
    return;

  // we must be in a neutral state
  for(int i = 0; i < KEY_MAX; ++i)
    if (state.keys[i] > 0)
    {
      state.pending = PENDING_GRAB;
      return;
    }

//  state.dsGrabKeyboard();

  for(EvdevDevice * device = state.devices; device->path; ++device)
  {
    if (device->fd > 0)
      evdev_grabDevice(device);
  }

  state.grabbed = true;
}

void evdev_ungrabKeyboard(void)
{
  if (!state.grabbed)
    return;

  // we must be in a neutral state
  for(int i = 0; i < KEY_MAX; ++i)
    if (state.keys[i] > 0)
    {
      state.pending = PENDING_UNGRAB;
      return;
    }

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

//  state.dsUngrabKeyboard();

  state.grabbed = false;
}

bool evdev_isExclusive(void)
{
  return state.exclusive && state.grabbed && !app_isOverlayMode();
}
