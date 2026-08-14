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

#ifndef _H_LG_COMMON_LGMP_CONFIG_
#define _H_LG_COMMON_LGMP_CONFIG_

#define LGMP_Q_POINTER     1
#define LGMP_Q_FRAME       2
// Base ID for LGMP_Q_FRAME_LEN independent owner-delivery queues.
#define LGMP_Q_FRAME_OWNER 3
#define LGMP_Q_INPUT       5
#define LGMP_Q_CLIPBOARD   6

// Two delivery lanes plus a spare buffer let the timing owner continue to
// alternate buffers while a secondary client holds the shared delivery.
#define LGMP_Q_FRAME_LEN        2
#define LGMP_Q_FRAME_BUFFER_LEN 3
#define LGMP_Q_POINTER_LEN      32
#define LGMP_Q_INPUT_LEN        4
#define LGMP_Q_CLIPBOARD_LEN    32

#endif
