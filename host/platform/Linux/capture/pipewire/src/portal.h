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

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum PipewireCaptureType
{
  PIPEWIRE_CAPTURE_DESKTOP = 1,
  PIPEWIRE_CAPTURE_WINDOW  = 2,
};

struct Portal;

struct Portal * portal_create(void);
void portal_free(struct Portal * portal);
bool portal_createScreenCastSession(struct Portal * portal, char ** handle);
void portal_destroySession(struct Portal * portal, char ** sessionHandle);
bool portal_selectSource(struct Portal * portal, const char * sessionHandle);
uint32_t portal_getPipewireNode(struct Portal * portal, const char * sessionHandle);
int portal_openPipewireRemote(struct Portal * portal, const char * sessionHandle);
