/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "common/option.h"
#include "common/debug.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

struct OptionGroup
{
  const char    *  module;
  struct Option ** options;
  int              count;
  int              pad;
};

struct State
{
  struct Option      * options;
  int                  oCount;
  struct OptionGroup * groups;
  int                  gCount;
};

struct State state =
{
  .options = NULL,
  .oCount  = 0,
  .groups  = NULL,
  .gCount  = 0
};

bool option_register(struct Option options[])
{
  int new = 0;
  for(int i = 0; options[i].value.type != OPTION_TYPE_NONE; ++i)
    ++new;

  state.options = realloc(
    state.options,
    sizeof(struct Option) * (state.oCount + new)
  );

  for(int i = 0; options[i].value.type != OPTION_TYPE_NONE; ++i)
  {
    struct Option * o = &state.options[state.oCount + i];
    memcpy(o, &options[i], sizeof(struct Option));

    // ensure the string is locally allocated
    if (o->value.type == OPTION_TYPE_STRING)
      o->value.v.x_string = strdup(o->value.v.x_string);

    // add the option to the correct group for help printout
    bool found = false;
    for(int g = 0; g < state.gCount; ++g)
    {
      struct OptionGroup * group = &state.groups[g];
      if (strcmp(group->module, o->module) != 0)
        continue;

      found = true;
      group->options = realloc(
        group->options,
        sizeof(struct Option *) * (group->count + 1)
      );
      group->options[group->count] = o;

      int len = strlen(o->name);
      if (len > group->pad)
        group->pad = len;

      ++group->count;
    }

    if (!found)
    {
      state.groups = realloc(
        state.groups,
        sizeof(struct OptionGroup) * (state.gCount + 1)
      );

      struct OptionGroup * group = &state.groups[state.gCount];
      ++state.gCount;

      group->module     = o->module;
      group->options    = malloc(sizeof(struct Option *));
      group->options[0] = o;
      group->count      = 1;
      group->pad        = strlen(o->name);
    }
  }

  state.oCount += new;
  return true;
};

void option_free()
{
  for(int i = 0; i < state.oCount; ++i)
  {
    struct Option * o = &state.options[i];
    if (o->value.type == OPTION_TYPE_STRING)
      free(o->value.v.x_string);
  }
  free(state.options);
  state.options = NULL;
  state.oCount  = 0;

  free(state.groups);
  state.groups  = NULL;
  state.gCount  = 0;
}

bool option_parse(int argc, char * argv[])
{
  for(int a = 1; a < argc; ++a)
  {
    if (strcmp(argv[a], "-h") == 0 || strcmp(argv[a], "--help") == 0)
    {
      option_print();
      return false;
    }

    char * arg    = strdup(argv[a]);
    char * module = strtok(arg , ":");
    char * name   = strtok(NULL, "=");
    char * value  = strtok(NULL, "" );

    if (!module || !name || !value)
    {
      DEBUG_WARN("Ignored invalid argument: %s", argv[a]);
      free(arg);
      continue;
    }

    bool found = false;
    struct Option * o;
    for(int i = 0; i < state.oCount; ++i)
    {
      o = &state.options[i];
      if ((strcmp(o->module, module) != 0) || (strcmp(o->name, name) != 0))
        continue;

      found = true;
      break;
    }

    if (!found)
    {
      DEBUG_WARN("Ignored unknown argument: %s", argv[a]);
      free(arg);
      continue;
    }

    switch(o->value.type)
    {
      case OPTION_TYPE_INT:
        o->value.v.x_int = atol(value);
        break;

      case OPTION_TYPE_STRING:
        free(o->value.v.x_string);
        o->value.v.x_string = strdup(value);
        break;

      case OPTION_TYPE_BOOL:
        o->value.v.x_bool =
          strcmp(value, "1"   ) == 0 ||
          strcmp(value, "yes" ) == 0 ||
          strcmp(value, "true") == 0 ||
          strcmp(value, "on"  ) == 0;
        break;

      default:
        DEBUG_ERROR("BUG: Invalid option type, this should never happen");
        assert(false);
        break;
    }

    free(arg);
  }

  return true;
}

bool option_validate()
{
  // validate the option values
  bool ok = true;
  for(int i = 0; i < state.oCount; ++i)
  {
    struct Option * o = &state.options[i];
    const char * error = NULL;
    if (o->validator)
      if (!o->validator(&o->value, &error))
      {
        printf("\nInvalid value provided to the option: %s:%s\n", o->module, o->name);

        if (error)
          printf("\n Error: %s\n", error);

        if (o->printHelp)
        {
          printf("\n");
          o->printHelp();
        }

        ok = false;
      }
  }

  if (!ok)
    printf("\n");

  return ok;
}

void option_print()
{
  printf(
    "The following is a complete list of options accepted by this application\n\n"
  );

  for(int g = 0; g < state.gCount; ++g)
  {
    for(int i = 0; i < state.groups[g].count; ++i)
    {
      struct Option * o = state.groups[g].options[i];
      printf("  %s:%-*s - %s\n", o->module, state.groups[g].pad, o->name, o->description);
    }
    printf("\n");
  }
}

struct OptionValue * option_get(const char * module, const char * name)
{
  for(int i = 0; i < state.oCount; ++i)
  {
    struct Option * o = &state.options[i];
    if ((strcmp(o->module, module) == 0) && (strcmp(o->name, name) == 0))
      return &o->value;
  }
  return NULL;
}

int option_get_int(const char * module, const char * name)
{
  struct OptionValue * o = option_get(module, name);
  if (!o)
    return -1;
  assert(o->type == OPTION_TYPE_INT);
  return o->v.x_int;
}

const char * option_get_string(const char * module, const char * name)
{
  struct OptionValue * o = option_get(module, name);
  if (!o)
    return NULL;
  assert(o->type == OPTION_TYPE_STRING);
  return o->v.x_string;
}

bool option_get_bool(const char * module, const char * name)
{
  struct OptionValue * o = option_get(module, name);
  if (!o)
    return false;
  assert(o->type == OPTION_TYPE_BOOL);
  return o->v.x_bool;
}