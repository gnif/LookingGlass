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

#ifndef _H_X11DS_WM_DEFAULT_
#define _H_X11DS_WM_DEFAULT_

#include "wm.h"
#include "x11.h"
#include "atoms.h"
#include "common/debug.h"
#include "common/option.h"
#include "common/util.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static struct Option options[] =
{
  // app options
  {
    .module         = "i3",
    .name           = "globalFullScreen",
    .description    = "Use i3's global full screen feature (spans all monitors)",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {0}
};

struct i3state
{
  int sock;
  bool globalFullScreen;
};

static struct i3state i3;

static void wm_i3_setup(void)
{
  option_register(options);
}

static bool wm_i3_init(void)
{
  memset(&i3, 0, sizeof(i3));

  i3.globalFullScreen = option_get_bool("i3", "globalFullScreen");

  FILE * fd = popen("i3 --get-socketpath", "r");
  if (!fd)
    return false;

  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  char * path = (char *)&addr.sun_path;
  int pathLen;
  if ((pathLen = fread(path, 1, sizeof(addr.sun_path), fd)) <= 0)
  {
    pclose(fd);
    return false;
  }
  pclose(fd);

  if(path[pathLen-1] == '\n')
    --pathLen;
  path[pathLen] = '\0';

  i3.sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (i3.sock < 0)
  {
    DEBUG_ERROR("Failed to create socket for i3 IPC");
    return false;
  }

  if (connect(i3.sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    DEBUG_ERROR("Failed to connect to the i3 IPC socket");
    perror("connect");
    goto err_socket;
  }

  DEBUG_INFO("i3 IPC Connected");
  return true;

err_socket:
  close(i3.sock);
  return false;
}

static void wm_i3_deinit(void)
{
  close(i3.sock);
}

static void wm_i3_setFullscreen(bool enable)
{
  if (!i3.globalFullScreen)
  {
    X11WM_Default.setFullscreen(enable);
    return;
  }

  struct i3Msg
  {
    char     magic[6];
    uint32_t length;
    uint32_t type;
    char     payload[0];
  }
  __attribute__((packed));

  #define I3_IPC_TYPE_RUN_COMMAND 0

  char cmd[128];
  int cmdLen = snprintf(cmd, sizeof(cmd),
      "[id=%lu] fullscreen toggle global",
      x11.window);

  struct i3Msg *msg = alloca(sizeof(struct i3Msg) + cmdLen);
  memcpy(msg->magic, "i3-ipc", 6);
  msg->length = cmdLen;
  msg->type   = I3_IPC_TYPE_RUN_COMMAND;
  memcpy(msg->payload, cmd, cmdLen);

  int msgSize = sizeof(*msg) + msg->length;
  char * buf = (char *)msg;
  while(msgSize)
  {
    int wrote = write(i3.sock, buf, msgSize);
    if (wrote <= 0)
    {
      DEBUG_WARN("i3 IPC communication failure");
      return;
    }

    buf     += wrote;
    msgSize -= wrote;
  }

  if ((msgSize = read(i3.sock, msg, sizeof(*msg))) < 0)
  {
    DEBUG_WARN("i3 IPC read failure");
    return;
  }

  if (memcmp(msg->magic, "i3-ipc", 6) != 0 ||
      msg->type != I3_IPC_TYPE_RUN_COMMAND)
  {
    DEBUG_WARN("i3 IPC unexpected reply");
    return;
  }

  // read and discard the payload
  while(msg->length)
  {
    int len = read(i3.sock, cmd, min(msg->length, sizeof(cmd)));
    if (len <= 0)
    {
      DEBUG_WARN("i3 IPC failed to read payload");
      return;
    }
    msg->length -= len;
  }
};

X11WM X11WM_i3 =
{
  .setup         = wm_i3_setup,
  .init          = wm_i3_init,
  .deinit        = wm_i3_deinit,
  .setFullscreen = wm_i3_setFullscreen
};

#endif
