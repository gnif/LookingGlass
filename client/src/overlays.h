/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#ifndef _H_OVERLAYS_H
#define _H_OVERLAYS_H

#include "interface/overlay.h"

extern struct LG_OverlayOps LGOverlayAlert;
extern struct LG_OverlayOps LGOverlayFPS;
extern struct LG_OverlayOps LGOverlayGraphs;
extern struct LG_OverlayOps LGOverlayHelp;
extern struct LG_OverlayOps LGOverlayConfig;

GraphHandle overlayGraph_register(const char * name, RingBuffer buffer,
    float min, float max);
void overlayGraph_unregister();
void overlayGraph_iterate(void (*callback)(GraphHandle handle, const char * name,
    bool * enabled, void * udata), void * udata);

void overlayConfig_register(const char * title,
    void (*callback)(void * udata, int * id), void * udata);

void overlayConfig_registerTab(const char * title,
    void (*callback)(void * udata, int * id), void * udata);

#endif
