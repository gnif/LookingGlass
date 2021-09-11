/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include "common/option.h"
#include "common/debug.h"
#include "common/stringutils.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct OptionGroup
{
  const char    *  module;
  struct Option ** options;
  int              count;
  int              pad;
};

struct State
{
  enum doHelpMode      doHelp;
  struct Option     ** options;
  int                  oCount;
  struct OptionGroup * groups;
  int                  gCount;
};

static struct State state =
{
  .doHelp  = DOHELP_MODE_NO,
  .options = NULL,
  .oCount  = 0,
  .groups  = NULL,
  .gCount  = 0
};

static bool int_parser(struct Option * opt, const char * str)
{
  opt->value.x_int = atol(str);
  return true;
}

static bool bool_parser(struct Option * opt, const char * str)
{
  opt->value.x_bool =
    strcasecmp(str, "1"   ) == 0 ||
    strcasecmp(str, "on"  ) == 0 ||
    strcasecmp(str, "yes" ) == 0 ||
    strcasecmp(str, "true") == 0;
  return true;
}

static bool float_parser(struct Option * opt, const char * str)
{
  opt->value.x_float = atof(str);
  return true;
}

static bool string_parser(struct Option * opt, const char * str)
{
  free(opt->value.x_string);
  opt->value.x_string = strdup(str);
  return true;
}

static char * int_toString(struct Option * opt)
{
  int len = snprintf(NULL, 0, "%d", opt->value.x_int);
  char * ret = malloc(len + 1);
  sprintf(ret, "%d", opt->value.x_int);
  return ret;
}

static char * bool_toString(struct Option * opt)
{
  return strdup(opt->value.x_bool ? "yes" : "no");
}

static char * float_toString(struct Option * opt)
{
  int len = snprintf(NULL, 0, "%f", opt->value.x_float);
  char * ret = malloc(len + 1);
  sprintf(ret, "%f", opt->value.x_float);
  return ret;
}

static char * string_toString(struct Option * opt)
{
  if (!opt->value.x_string)
    return NULL;

  return strdup(opt->value.x_string);
}

bool option_register(struct Option options[])
{
  int new = 0;
  for(int i = 0; options[i].type != OPTION_TYPE_NONE; ++i)
    ++new;

  state.options = realloc(
    state.options,
    sizeof(*state.options) * (state.oCount + new)
  );

  for(int i = 0; options[i].type != OPTION_TYPE_NONE; ++i)
  {
    state.options[state.oCount + i] = malloc(sizeof(**state.options));
    struct Option * o = state.options[state.oCount + i];
    memcpy(o, &options[i], sizeof(*o));

    if (!o->parser)
    {
      switch(o->type)
      {
        case OPTION_TYPE_INT:
          o->parser = int_parser;
          break;

        case OPTION_TYPE_STRING:
          o->parser = string_parser;
          break;

        case OPTION_TYPE_BOOL:
          o->parser = bool_parser;
          break;

        case OPTION_TYPE_FLOAT:
          o->parser = float_parser;
          break;

        default:
          DEBUG_ERROR("BUG: Non int/string/bool option types must have a parser");
          continue;
      }
    }

    if (!o->toString)
    {
      switch(o->type)
      {
        case OPTION_TYPE_INT:
          o->toString = int_toString;
          break;

        case OPTION_TYPE_STRING:
          o->toString = string_toString;
          break;

        case OPTION_TYPE_BOOL:
          o->toString = bool_toString;
          break;

        case OPTION_TYPE_FLOAT:
          o->toString = float_toString;
          break;

        default:
          DEBUG_ERROR("BUG: Non int/string/bool option types must implement toString");
          continue;
      }
    }

    // ensure the string is locally allocated
    if (o->type == OPTION_TYPE_STRING)
    {
      if (o->value.x_string)
        o->value.x_string = strdup(o->value.x_string);
    }

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
        sizeof(*group->options) * (group->count + 1)
      );
      group->options[group->count] = o;

      int len = strlen(o->name);
      if (len > group->pad)
        group->pad = len;

      ++group->count;
      break;
    }

    if (!found)
    {
      state.groups = realloc(
        state.groups,
        sizeof(*state.groups) * (state.gCount + 1)
      );

      struct OptionGroup * group = &state.groups[state.gCount];
      ++state.gCount;

      group->module     = o->module;
      group->options    = malloc(sizeof(*group->options));
      group->options[0] = o;
      group->count      = 1;
      group->pad        = strlen(o->name);
    }
  }

  state.oCount += new;
  return true;
};

void option_free(void)
{
  for(int i = 0; i < state.oCount; ++i)
  {
    struct Option * o = state.options[i];
    if (o->type == OPTION_TYPE_STRING)
      free(o->value.x_string);
    free(o);
  }
  free(state.options);
  state.options = NULL;
  state.oCount  = 0;

  for(int g = 0; g < state.gCount; ++g)
  {
    struct OptionGroup * group = &state.groups[g];
    if (group->options)
      free(group->options);
  }
  free(state.groups);
  state.groups  = NULL;
  state.gCount  = 0;
}

static bool option_set(struct Option * opt, const char * value)
{
  if (!opt->parser(opt, value))
  {
    opt->failed_set = true;
    return false;
  }

  opt->failed_set = false;
  return true;
}

bool option_parse(int argc, char * argv[])
{
  for(int a = 1; a < argc; ++a)
  {
    struct Option * o = NULL;
    char * value      = NULL;

    // emulate getopt for backwards compatability
    if (argv[a][0] == '-')
    {
      if (strcasecmp(argv[a], "-h") == 0 || strcasecmp(argv[a], "--help") == 0)
      {
        state.doHelp = DOHELP_MODE_YES;
        continue;
      }

      if (strcasecmp(argv[a], "--rst-help") == 0)
      {
        state.doHelp = DOHELP_MODE_RST;
        continue;
      }

      if (strlen(argv[a]) != 2)
      {
        DEBUG_WARN("Ignored invalid argument: %s", argv[a]);
        continue;
      }

      for(int i = 0; i < state.oCount; ++i)
      {
        if (state.options[i]->shortopt == argv[a][1])
        {
          o = state.options[i];
          if (o->type != OPTION_TYPE_BOOL && a < argc - 1)
          {
            ++a;
            value = strdup(argv[a]);
          }
          break;
        }
      }
    }
    else
    {
      char * arg    = strdup(argv[a]);
      char * module = strtok(arg , ":");
      char * name   = strtok(NULL, "=");
      value = strtok(NULL, "" );

      if (!module || !name)
      {
        DEBUG_WARN("Ignored invalid argument: %s", argv[a]);
        free(arg);
        continue;
      }

      o = option_get(module, name);
      if (value)
        value = strdup(value);

      free(arg);
    }

    if (!o)
    {
      DEBUG_WARN("Ignored unknown argument: %s", argv[a]);
      free(value);
      continue;
    }

    if (!value)
    {
      if (o->type == OPTION_TYPE_BOOL)
      {
        o->value.x_bool = !o->value.x_bool;
        continue;
      }
      else if (o->type != OPTION_TYPE_CUSTOM)
      {
        DEBUG_WARN("Ignored invalid argument, missing value: %s", argv[a]);
        continue;
      }
    }

    option_set(o, value);
    free(value);
  }

  return true;
}

static char * file_parse_module(FILE * fp)
{
  char * module = NULL;
  int    len    = 0;

  for(int c = fgetc(fp); !feof(fp); c = fgetc(fp))
  {
    switch(c)
    {
      case ']':
        if (module)
          module[len] = '\0';
        return module;

      case '\r':
      case '\n':
        free(module);
        return NULL;

      default:
        if (len % 32 == 0)
        {
          char * p = realloc(module, len + 32 + 1);
          if (!p)
          {
            DEBUG_ERROR("out of memory");
            free(module);
            return NULL;
          }
          module = p;
        }
        module[len++] = c;
    }
  }

  if (module)
    free(module);

  return NULL;
}

static bool process_option_line(const char * module, const char * name,
    char * value, int valueLen, int lineno)
{
  if (!module)
  {
    DEBUG_ERROR("Syntax error on line %d, module not specified for option", lineno);
    return false;
  }

  struct Option * o = option_get(module, name);
  if (!o)
    DEBUG_WARN("Ignored unknown option %s:%s", module, name);
  else
  {
    if (value)
      value[valueLen] = '\0';

    if (!option_set(o, value))
      DEBUG_ERROR("Failed to set the option value");
  }
  return true;
}

bool option_load(const char * filename)
{
  FILE * fp = fopen(filename, "r");
  if (!fp)
    return false;

  bool   result      = true;
  int    lineno      = 1;
  char * module      = NULL;
  bool   line        = true;
  bool   comment     = false;
  bool   expectLine  = false;
  bool   expectValue = false;
  char * name        = NULL;
  int    nameLen     = 0;
  char * value       = NULL;
  int    valueLen    = 0;

  char ** p   = &name;
  int  *  len = &nameLen;

  for(int c = fgetc(fp); !feof(fp); c = fgetc(fp))
  {
    if (comment && c != '\n')
      continue;
    comment = false;

    switch(c)
    {
      case '[':
        if (expectLine)
        {
          DEBUG_ERROR("Syntax error on line %d, expected new line", lineno);
          result = false;
          goto exit;
        }

        if (line)
        {
          free(module);
          module = file_parse_module(fp);
          if (!module)
          {
            DEBUG_ERROR("Syntax error on line %d, failed to parse the module", lineno);
            result = false;
            goto exit;
          }
          line       = false;
          expectLine = true;
          continue;
        }

        if (*len % 32 == 0)
        {
          char * tmp = realloc(*p, *len + 32 + 1);
          if (!tmp)
          {
            DEBUG_ERROR("out of memory");
            result = false;
            goto exit;
          }
          *p = tmp;
        }
        (*p)[(*len)++] = c;
        break;

      case '\r':
        continue;

      case '\n':
        if (name && !process_option_line(module, name, value, valueLen, lineno))
        {
          result = false;
          goto exit;
        }

        line        = true;
        expectLine  = false;
        expectValue = false;
        ++lineno;

        p   = &name;
        len = &nameLen;

        free(name);
        name     = NULL;
        nameLen  = 0;
        free(value);
        value    = NULL;
        valueLen = 0;
        break;

      case '=':
        if (!expectValue)
        {
          if (!name)
          {
            DEBUG_ERROR("Syntax error on line %d, expected option name", lineno);
            result = false;
            goto exit;
          }

          //rtrim
          while (nameLen > 1 && isspace(name[nameLen-1]))
            --nameLen;
          name[nameLen] = '\0';
          expectValue   = true;

          p   = &value;
          len = &valueLen;
          continue;
        }

        if (*len % 32 == 0)
        {
          char * tmp = realloc(*p, *len + 32 + 1);
          if (!tmp)
          {
            DEBUG_ERROR("out of memory");
            result = false;
            goto exit;
          }
          *p = tmp;
        }
        (*p)[(*len)++] = c;
        break;

      case ';':
        if (line)
        {
          comment = true;
          break;
        }
        // fallthrough

      default:
        // ignore non-typeable ascii characters
        if (c < 32 || c > 126)
          continue;

        if (expectLine)
        {
          DEBUG_ERROR("Syntax error on line %d, expected new line", lineno);
          result = false;
          goto exit;
        }
        line = false;

        //ltrim
        if (*len == 0 && isspace(c))
          break;

        if (*len % 32 == 0)
        {
          char * tmp = realloc(*p, *len + 32 + 1);
          if (!tmp)
          {
            DEBUG_ERROR("out of memory");
            result = false;
            goto exit;
          }
          *p = tmp;
        }
        (*p)[(*len)++] = c;
        break;
    }
  }

  if (name && !process_option_line(module, name, value, valueLen, lineno))
    result = false;

exit:
  fclose(fp);

  free(module);
  free(name  );
  free(value );

  return result;
}

bool option_validate(void)
{
  if (state.doHelp != DOHELP_MODE_NO)
  {
    option_print();
    return false;
  }

  // validate the option values
  bool ok = true;
  for(int i = 0; i < state.oCount; ++i)
  {
    struct Option * o = state.options[i];
    const char * error = NULL;
    bool invalid = o->failed_set;

    if (!invalid && o->validator)
      invalid = !o->validator(o, &error);

    if (invalid)
    {
      printf("\nInvalid value provided to the option: %s:%s\n", o->module, o->name);

      if (error)
        printf("\n Error: %s\n", error);

      if (o->getValues)
      {
        StringList values = o->getValues(o);
        printf("\nValid values are:\n\n");
        for(unsigned int v = 0; v < stringlist_count(values); ++v)
          printf("  * %s\n", stringlist_at(values, v));
        stringlist_free(&values);
      }

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

void option_print_hrule(char * headerLine, int maxLen, char ruleChar)
{
  printf("  +%c", ruleChar);
  for (int i = 0; i < maxLen; i++)
  {
    if(i < strlen(headerLine))
    {
      if (headerLine[i] == '|')
      {
        putc('+', stdout);
        continue;
      }
    }
    putc(ruleChar, stdout);
  }
  printf("%c+\n", ruleChar);
}

void option_print(void)
{
  printf(
    "The following is a complete list of options accepted by this application\n\n"
  );

  for(int g = 0; g < state.gCount; ++g)
  {
    StringList lines  = stringlist_new(true);
    StringList values = stringlist_new(true);
    int len;
    int maxLen;
    int valueLen = 5;
    char * line;
    char * headerLine;

    // ensure the pad length is atleast as wide as the heading
    if (state.groups[g].pad < 4)
      state.groups[g].pad = 4;

    // get the values and the max value length
    for(int i = 0; i < state.groups[g].count; ++i)
    {
      struct Option * o = state.groups[g].options[i];
      if (o->preset)
        continue;
      char * value = o->toString(o);
      if (!value)
      {
        value = strdup("NULL");
        len   = 4;
      }
      else
        len = strlen(value);

      if (len > valueLen)
        valueLen = len;

      stringlist_push(values, value);
    }

    // add the heading
    maxLen = alloc_sprintf(
      &line,
      "%-*s | Short | %-*s | Description",
      (int)(strlen(state.groups[g].module) + state.groups[g].pad + 1),
      "Long",
      valueLen,
      "Value"
    );

    DEBUG_ASSERT(maxLen > 0);
    headerLine = line;
    stringlist_push(lines, line);

    for(int i = 0; i < state.groups[g].count; ++i)
    {
      struct Option * o = state.groups[g].options[i];
      if (o->preset)
        continue;

      char * value = stringlist_at(values, i);
      len = alloc_sprintf(
        &line,
        "%s:%-*s | %c%c    | %-*s | %s",
        o->module,
        state.groups[g].pad,
        o->name,
        o->shortopt ? '-'         : ' ',
        o->shortopt ? o->shortopt : ' ',
        valueLen,
        value,
        o->description
      );

      DEBUG_ASSERT(len > 0);
      stringlist_push(lines, line);
      if (len > maxLen)
        maxLen = len;
    }

    stringlist_free(&values);

    if (stringlist_count(lines) <= 1)
    {
      stringlist_free(&lines);
      continue;
    }

    // print out the lines
    for(int i = 0; i < stringlist_count(lines); ++i)
    {
      if (i == 0)
      {
        option_print_hrule(headerLine, maxLen, '-');
      }

      char * line = stringlist_at(lines, i);
      printf("  | %-*s |\n", maxLen, line);

      if (i == 0)
      {
        option_print_hrule(headerLine, maxLen, state.doHelp == DOHELP_MODE_RST ? '=' : '-');
      }
      else if (state.doHelp == DOHELP_MODE_RST && i < stringlist_count(lines) - 1)
      {
        option_print_hrule(headerLine, maxLen, '-');
      }
    }

    option_print_hrule(headerLine, maxLen, '-');

    stringlist_free(&lines);

    printf("\n");
  }
}

// dump the options in ini format into the file
bool option_dump_preset(FILE * file)
{
  for (int g = 0; g < state.gCount; ++g)
  {
    bool hasPreset = false;
    for (int i = 0; i < state.groups[g].count; ++i)
      hasPreset |= state.groups[g].options[i]->preset;
    if (!hasPreset)
      continue;

    fprintf(file, "[%s]\n", state.groups[g].module);

    for (int i = 0; i < state.groups[g].count; ++i)
    {
      struct Option * o = state.groups[g].options[i];
      if (!o->preset)
        continue;

      char * value = o->toString(o);
      fprintf(file, "%s=%s\n", o->name, value);
      free(value);
    }
    fputc('\n', file);
  }

  return true;
}

struct Option * option_get(const char * module, const char * name)
{
  for(int i = 0; i < state.oCount; ++i)
  {
    struct Option * o = state.options[i];
    if ((strcasecmp(o->module, module) == 0) && (strcasecmp(o->name, name) == 0))
      return o;
  }
  return NULL;
}

int option_get_int(const char * module, const char * name)
{
  struct Option * o = option_get(module, name);
  if (!o)
  {
    DEBUG_ERROR("BUG: Failed to get the value for option %s:%s", module, name);
    return -1;
  }
  DEBUG_ASSERT(o->type == OPTION_TYPE_INT);
  return o->value.x_int;
}

const char * option_get_string(const char * module, const char * name)
{
  struct Option * o = option_get(module, name);
  if (!o)
  {
    DEBUG_ERROR("BUG: Failed to get the value for option %s:%s", module, name);
    return NULL;
  }
  DEBUG_ASSERT(o->type == OPTION_TYPE_STRING);
  return o->value.x_string;
}

bool option_get_bool(const char * module, const char * name)
{
  struct Option * o = option_get(module, name);
  if (!o)
  {
    DEBUG_ERROR("BUG: Failed to get the value for option %s:%s", module, name);
    return false;
  }
  DEBUG_ASSERT(o->type == OPTION_TYPE_BOOL);
  return o->value.x_bool;
}

float option_get_float(const char * module, const char * name)
{
  struct Option * o = option_get(module, name);
  if (!o)
  {
    DEBUG_ERROR("BUG: Failed to get the value for option %s:%s", module, name);
    return NAN;
  }
  DEBUG_ASSERT(o->type == OPTION_TYPE_FLOAT);
  return o->value.x_float;
}

void option_set_int(const char * module, const char * name, int value)
{
  struct Option * o = option_get(module, name);
  if (!o)
  {
    DEBUG_ERROR("BUG: Failed to set the value for option %s:%s", module, name);
    return;
  }
  DEBUG_ASSERT(o->type == OPTION_TYPE_INT);
  o->value.x_int = value;
}

void option_set_string(const char * module, const char * name, const char * value)
{
  struct Option * o = option_get(module, name);
  if (!o)
  {
    DEBUG_ERROR("BUG: Failed to set the value for option %s:%s", module, name);
    return;
  }
  DEBUG_ASSERT(o->type == OPTION_TYPE_STRING);
  free(o->value.x_string);
  o->value.x_string = strdup(value);
}

void option_set_bool(const char * module, const char * name, bool value)
{
  struct Option * o = option_get(module, name);
  if (!o)
  {
    DEBUG_ERROR("BUG: Failed to set the value for option %s:%s", module, name);
    return;
  }
  DEBUG_ASSERT(o->type == OPTION_TYPE_BOOL);
  o->value.x_bool = value;
}

void option_set_float(const char * module, const char * name, float value)
{
  struct Option * o = option_get(module, name);
  if (!o)
  {
    DEBUG_ERROR("BUG: Failed to set the value for option %s:%s", module, name);
    return;
  }
  DEBUG_ASSERT(o->type == OPTION_TYPE_FLOAT);
  o->value.x_float = value;
}
