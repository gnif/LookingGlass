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

#include "message.h"
#include "core.h"
#include "common/debug.h"
#include "common/ll.h"
#include "common/time.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct MsgEvent
{
  uint64_t timestamp;
  LGMsg    msg;
}
MsgEvent;

struct MsgState
{
  struct ll * list;
  struct
  {
    unsigned width;
    unsigned height;
  }
  lastWindowSize;
};

static struct MsgState this = {0};

bool lgMessage_init(void)
{
  this.list = ll_new();
  if (!this.list)
  {
    DEBUG_ERROR("Failed to create the message list");
    return false;
  }

  return true;
}

void lgMessage_deinit(void)
{
  if (this.list)
  {
    void * tmp;
    while(ll_shift(this.list, &tmp))
      free(tmp);
    ll_free(this.list);
    this.list = NULL;
  }
}

void lgMessage_post(const LGMsg * msg)
{
  MsgEvent * event = (MsgEvent *)malloc(sizeof(*event));
  if (!event)
  {
    DEBUG_ERROR("Out of memory");
    return;
  }

  event->timestamp = microtime();
  memcpy(&event->msg, msg, sizeof(event->msg));
  if (!ll_push(this.list, event))
  {
    DEBUG_ERROR("Failed to post a message to the list");
    free(event);
  }
}

void lgMessage_process(void)
{
  MsgEvent * event;
  MsgEvent * windowSize = NULL;

  while(ll_shift(this.list, (void **)&event))
  {
    switch(event->msg.type)
    {
      case LG_MSG_WINDOWSIZE:
      {
        // retain the last/latest windowsize event
        if (windowSize)
          free(windowSize);
        windowSize = event;
        continue;
      }

      default:
        DEBUG_ERROR("Unhandled %d", event->msg.type);
        break;
    }

    free(event);
  }

  // if there was a windowSize event, then process it
  if (windowSize)
  {
    const uint64_t time = microtime();
    if (time - windowSize->timestamp < 500000)
    {
      // requeue the event for later
      if (!ll_push(this.list, event))
      {
        DEBUG_ERROR("Failed to re-queue the windowSize event");
        free(windowSize);
      }
    }
    else
    {
      if (event->msg.windowSize.width  != this.lastWindowSize.width ||
          event->msg.windowSize.height != this.lastWindowSize.height)
      {
        this.lastWindowSize.width  = event->msg.windowSize.width;
        this.lastWindowSize.height = event->msg.windowSize.height;
        core_onWindowSizeChanged(
            event->msg.windowSize.width,
            event->msg.windowSize.height);
      }
      free(windowSize);
    }
  }
}
