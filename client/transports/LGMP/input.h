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

#ifndef _H_LG_CLIENT_TRANSPORT_LGMP_INPUT_
#define _H_LG_CLIENT_TRANSPORT_LGMP_INPUT_

#include "interface/input.h"

#include <lgmp/client.h>

#include <stdbool.h>

typedef struct LGMPInput LGMPInput;

bool lgmpInput_create(PLGMPClient client, LGMPInput ** result);
void lgmpInput_destroy(LGMPInput ** input);

bool lgmpInput_connect(LGMPInput * input);
void lgmpInput_disconnect(LGMPInput * input);

const LG_InputOps * lgmpInput_getOps(void);

#endif
