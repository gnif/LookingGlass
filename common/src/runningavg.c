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

#include "common/runningavg.h"
#include "common/debug.h"

#include <stdlib.h>

struct RunningAvg
{
  int       length, samples;
  int       pos;
  int64_t   value;
  int64_t   values[0];
};

RunningAvg runningavg_new(int length)
{
  struct RunningAvg * ra = calloc(1, sizeof(*ra) + sizeof(*ra->values) * length);
  if (!ra)
  {
    DEBUG_ERROR("out of memory");
    return NULL;
  }

  ra->length = length;
  return ra;
}

void runningavg_free(RunningAvg * ra)
{
  free(*ra);
  *ra = NULL;
}

void runningavg_push(RunningAvg ra, int64_t value)
{
  if (ra->samples == ra->length)
    ra->value -= ra->values[ra->pos];
  else
    ++ra->samples;

  ra->value += value;
  ra->values[ra->pos++] = value;

  if (ra->pos == ra->length)
    ra->pos = 0;
}

void runningavg_reset(RunningAvg ra)
{
  ra->samples = 0;
  ra->pos     = 0;
  ra->value   = 0;
}

double runningavg_calc(RunningAvg ra)
{
  return (double)ra->value / ra->samples;
}
