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

#include <stdbool.h>
#include "common/option.h"
#include "common/vector.h"

typedef struct
{
  const char * module;
  unsigned int id;
  bool         greater;
  unsigned int x;
  unsigned int y;
  unsigned int targetX;
  unsigned int targetY;
}
DownsampleRule;

bool downsampleParser(struct Option * opt, const char * str);
void downsampleCleanup(struct Option * opt);

DownsampleRule * downsampleRule_match(Vector * rules, int x, int y);

#define DOWNSAMPLE_PARSER(moduleName, vector) \
{ \
  .module         = moduleName, \
  .name           = "downsample", \
  .description    = "Downsample rules, format: [>](width)x(height):(toWidth)x(toHeight)", \
  .type           = OPTION_TYPE_STRING, \
  .value.x_string = NULL, \
  .parser         = downsampleParser, \
  .cleanup        = downsampleCleanup, \
  .opaque         = (void*)(vector) \
}
