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

  Vector * downsampleRules = (Vector *)opt->opaque;
  if (downsampleRules->data)
    vector_destroy(downsampleRules);

  if (!vector_create(downsampleRules, sizeof(DownsampleRule), 10))
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

    rule.module = opt->module;
    if (sscanf(token, "%ux%u:%ux%u",
      &rule.x,
      &rule.y,
      &rule.targetX,
      &rule.targetY) != 4)
    {
      DEBUG_INFO("Unable to parse downsample rules");
      free(tmp);
      return false;
    }

    rule.id = count++;

    DEBUG_INFO(
      "%s:%s rule %u: %ux%u IF X %s %4u %s Y %s %4u",
      opt->module,
      opt->name,
      rule.id,
      rule.targetX,
      rule.targetY,
      rule.greater ? "> "  : "==",
      rule.x,
      rule.greater ? "OR " : "AND",
      rule.greater ? "> "  : "==",
      rule.y
    );
    vector_push(downsampleRules, &rule);

    token = strtok(NULL, ",");
  }
  free(tmp);

  return true;
}

void downsampleCleanup(struct Option * opt)
{
  Vector * downsampleRules = (Vector *)opt->opaque;
  if (downsampleRules->data)
    vector_destroy(downsampleRules);
}

DownsampleRule * downsampleRule_match(Vector * rules, int x, int y)
{
  DownsampleRule * rule, * match = NULL;
  vector_forEachRef(rule, rules)
  {
    if (( rule->greater && (x  > rule->x || y  > rule->y)) ||
        (!rule->greater && (x == rule->x && y == rule->y)))
    {
      match = rule;
    }
  }

  if (match)
    DEBUG_INFO("Matched downsample rule %d", rule->id);

  return match;
}
