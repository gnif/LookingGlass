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

#include "interface/transport.h"
#include "dynamic/transports.h"

#include "common/debug.h"

#include <string.h>

void lgTransport_setup(void)
{
  for (unsigned i = 0; i < LG_TRANSPORT_COUNT; ++i)
    if (LG_Transports[i]->setup)
      LG_Transports[i]->setup();
}

bool lgTransport_isValid(const char * name)
{
  if (!name)
    return false;
  for (unsigned i = 0; i < LG_TRANSPORT_COUNT; ++i)
    if (strcmp(LG_Transports[i]->name, name) == 0)
      return true;
  return false;
}

bool lgTransport_create(const char * name, LG_TransportInstance * instance)
{
  if (!instance)
    return false;

  *instance = (LG_TransportInstance) { 0 };
  for (unsigned i = 0; i < LG_TRANSPORT_COUNT; ++i)
  {
    if (strcmp(LG_Transports[i]->name, name) != 0)
      continue;

    const LG_TransportOps * ops = LG_Transports[i];
    if (!ops->create || !ops->destroy || !ops->connectCancellable ||
        !ops->disconnect || !ops->sessionValid)
    {
      DEBUG_ERROR("Transport %s has an incomplete session interface", name);
      return false;
    }

    if (!ops->create(&instance->handle))
      return false;

    const LG_VideoOps * video = ops->getVideoOps ?
      ops->getVideoOps(instance->handle) : NULL;
    const bool badFrame = video && video->type == LG_VIDEO_TYPE_FRAME &&
      (!video->frame || !video->frame->nextFrame ||
       !video->frame->releaseFrame || !video->frame->cancelFrameWait ||
       ((video->frame->nextPointer != NULL) !=
        (video->frame->releasePointer != NULL)) ||
       (video->frame->nextPointer && !video->frame->cancelPointerWait));
    const bool badSurface = video && video->type == LG_VIDEO_TYPE_SW_SURFACE &&
      (!video->swSurface || !video->swSurface->attach ||
       !video->swSurface->detach || !video->swSurface->setActive ||
       !video->swSurface->cancelPending);
    if (video && (badFrame || badSurface ||
          (video->type != LG_VIDEO_TYPE_FRAME &&
           video->type != LG_VIDEO_TYPE_SW_SURFACE)))
    {
      DEBUG_ERROR("Transport %s has an incomplete video interface", name);
      ops->destroy(&instance->handle);
      return false;
    }

    instance->ops = ops;
    return true;
  }

  return false;
}

void lgTransport_destroy(LG_TransportInstance * instance)
{
  if (!instance)
    return;

  if (instance->ops)
    instance->ops->destroy(&instance->handle);
  *instance = (LG_TransportInstance) { 0 };
}
