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

#include "effect.h"

extern const D12Effect D12Effect_HDR16to10;
extern const D12Effect D12Effect_RGB24;
extern const D12Effect D12Effect_Downsample;

static const D12Effect * D12Effects[] =
{
  &D12Effect_Downsample,
  &D12Effect_HDR16to10,

  &D12Effect_RGB24, // this MUST be last
  NULL
};
