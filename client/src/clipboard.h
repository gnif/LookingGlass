/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include "spice/spice.h"
#include "interface/displayserver.h"

LG_ClipboardData cb_spiceTypeToLGType(const SpiceDataType type);
SpiceDataType cb_lgTypeToSpiceType(const LG_ClipboardData type);

void cb_spiceNotice(const SpiceDataType type);
void cb_spiceData(const SpiceDataType type, uint8_t * buffer, uint32_t size);
void cb_spiceRelease(void);
void cb_spiceRequest(const SpiceDataType type);
