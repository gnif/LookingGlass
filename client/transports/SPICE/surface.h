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

#ifndef _H_LG_CLIENT_TRANSPORT_SPICE_SURFACE_
#define _H_LG_CLIENT_TRANSPORT_SPICE_SURFACE_

#include "interface/transport.h"

#include <purespice.h>

typedef struct SpiceSurface SpiceSurface;

bool spiceSurface_init(SpiceSurface ** surface);
void spiceSurface_free(SpiceSurface ** surface);

const LG_VideoOps * spiceSurface_getVideoOps(void);
void spiceSurface_sessionStopped(SpiceSurface * surface);

#ifdef ENABLE_TESTS
void spiceSurface_testLockActivation(SpiceSurface * surface);
void spiceSurface_testUnlockActivation(SpiceSurface * surface);
#endif

void spiceSurface_create(SpiceSurface * surface, unsigned int surfaceId,
    PSSurfaceFormat format, unsigned int width, unsigned int height);
void spiceSurface_destroy(SpiceSurface * surface, unsigned int surfaceId);
void spiceSurface_drawFill(SpiceSurface * surface, unsigned int surfaceId,
    int x, int y, int width, int height, uint32_t color);
void spiceSurface_drawBitmap(SpiceSurface * surface, unsigned int surfaceId,
    PSBitmapFormat format, bool topDown, int x, int y,
    int width, int height, int stride, void * data);

void spiceSurface_setRGBAImage(SpiceSurface * surface,
    int width, int height, int hx, int hy, const void * data);
void spiceSurface_setMonoImage(SpiceSurface * surface,
    int width, int height, int hx, int hy,
    const void * xorMask, const void * andMask);
void spiceSurface_setColorImage(SpiceSurface * surface,
    int width, int height, int hx, int hy,
    const void * data, const void * maskData);
void spiceSurface_setPointerState(SpiceSurface * surface,
    bool visible, int x, int y);

#endif
