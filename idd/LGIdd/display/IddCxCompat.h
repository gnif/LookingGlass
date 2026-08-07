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

#pragma once

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>

// IddCx 1.10 HDR/WCG types are only visible when the WDK targets
// (NTDDI >= 0x0A000005) and the build flags select IddCx 1.10 or newer.
#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || \
    (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10)) && \
  NTDDI_VERSION >= 0x0A000005
  #define HAS_IDDCX_110
#endif
