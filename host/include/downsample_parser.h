/**
 * looking glass
 * copyright © 2017-2023 the looking glass authors
 * https://looking-glass.io
 *
 * this program is free software; you can redistribute it and/or modify it
 * under the terms of the gnu general public license as published by the free
 * software foundation; either version 2 of the license, or (at your option)
 * any later version.
 *
 * this program is distributed in the hope that it will be useful, but without
 * any warranty; without even the implied warranty of merchantability or
 * fitness for a particular purpose. see the gnu general public license for
 * more details.
 *
 * you should have received a copy of the gnu general public license along
 * with this program; if not, write to the free software foundation, inc., 59
 * temple place, suite 330, boston, ma 02111-1307 usa
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
