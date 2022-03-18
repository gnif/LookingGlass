/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#ifndef _H_OVERLAY_MSG_H
#define _H_OVERLAY_MSG_H

#include <stdbool.h>
#include <stdarg.h>

#include "app.h"

bool overlayMsg_modal(void);

MsgBoxHandle overlayMsg_show(
    const char * caption, MsgBoxConfirmCallback confirm, void * opaque,
    const char * fmt, va_list args);

void overlayMsg_close(MsgBoxHandle handle);

#endif
