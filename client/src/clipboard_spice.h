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

#ifndef _H_LG_CLIENT_CLIPBOARD_SPICE_
#define _H_LG_CLIENT_CLIPBOARD_SPICE_

#include "interface/clipboard.h"

#include <purespice.h>

extern const LG_ClipboardOps LGC_Spice;

void lgcSpice_setAvailable(bool available);

void lgcSpice_notice(PSDataType type);
void lgcSpice_data(PSDataType type, uint8_t * buffer, uint32_t size);
void lgcSpice_release(void);
void lgcSpice_request(PSDataType type);

#endif
