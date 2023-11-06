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

#include "downsample_parser.h"
#include "common/debug.h"

#include <string.h>

Vector downsampleRules = {0};

bool downsampleParser(struct Option * opt, const char * str)
{
  if (!str)
    return false;

  opt->value.x_string = strdup(str);

  if (downsampleRules.data)
    vector_destroy(&downsampleRules);

  if (!vector_create(&downsampleRules, sizeof(DownsampleRule), 10))
  {
    DEBUG_ERROR("Failed to allocate ram");
    return false;
  }

  char * tmp   = strdup(str);
  char * token = strtok(tmp, ",");
  int count = 0;
  while(token)
  {
    DownsampleRule rule = {0};
    if (token[0] == '>')
    {
      rule.greater = true;
      ++token;
    }

    if (sscanf(token, "%ux%u:%ux%u",
      &rule.x,
      &rule.y,
      &rule.targetX,
      &rule.targetY) != 4)
    {
      DEBUG_INFO("Unable to parse downsample rules");
      return false;
    }

    rule.id = count++;

    DEBUG_INFO(
      "Rule %u: %ux%u IF X %s %4u %s Y %s %4u",
      rule.id,
      rule.targetX,
      rule.targetY,
      rule.greater ? "> "  : "==",
      rule.x,
      rule.greater ? "OR " : "AND",
      rule.greater ? "> "  : "==",
      rule.y
    );
    vector_push(&downsampleRules, &rule);

    token = strtok(NULL, ",");
  }
  free(tmp);

  return true;
}
