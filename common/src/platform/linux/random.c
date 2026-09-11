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

#include "common/random.h"

#include <errno.h>
#include <stdint.h>
#include <sys/random.h>

bool lgRandomBytes(void * buffer, size_t size)
{
  uint8_t * output = buffer;
  while (size)
  {
    const ssize_t received = getrandom(output, size, 0);
    if (received < 0 && errno == EINTR)
      continue;
    if (received <= 0)
      return false;

    output += (size_t)received;
    size   -= (size_t)received;
  }

  return true;
}
