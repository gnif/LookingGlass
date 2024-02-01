/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
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

#ifndef _H_D12_
#define _H_D12_

#include "com_ref.h"
#include "interface/capture.h"

extern ComScope * d12_comScope;
#define comRef_toGlobal(dst, src) \
  _comRef_toGlobal(d12_comScope, dst, src)

// APIs for the backends to call

void d12_updatePointer(
  CapturePointer * pointer, void * shape, size_t shapeSize);

#endif
