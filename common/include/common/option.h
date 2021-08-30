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

#ifndef _H_COMMON_OPTION_
#define _H_COMMON_OPTION_

#include <stdbool.h>
#include <stdio.h>
#include "common/stringlist.h"

enum OptionType
{
  OPTION_TYPE_NONE = 0,
  OPTION_TYPE_INT,
  OPTION_TYPE_STRING,
  OPTION_TYPE_BOOL,
  OPTION_TYPE_FLOAT,
  OPTION_TYPE_CUSTOM
};

enum doHelpMode
{
  DOHELP_MODE_NO = 0,
  DOHELP_MODE_YES,
  DOHELP_MODE_RST
};

struct Option;

struct Option
{
  char * module;
  char * name;
  char * description;
  const char shortopt;
  bool preset;

  enum OptionType type;
  union
  {
    int    x_int;
    char * x_string;
    bool   x_bool;
    float  x_float;
    void * x_custom;
  }
  value;

  bool         (*parser   )(struct Option * opt, const char * str);
  bool         (*validator)(struct Option * opt, const char ** error);
  char       * (*toString )(struct Option * opt);
  StringList   (*getValues)(struct Option * opt);

  void    (*printHelp)(void);

  // internal use only
  bool failed_set;
};

// register an NULL terminated array of options
bool option_register(struct Option options[]);

// lookup the value of an option
struct Option * option_get       (const char * module, const char * name);
int             option_get_int   (const char * module, const char * name);
const char *    option_get_string(const char * module, const char * name);
bool            option_get_bool  (const char * module, const char * name);
float           option_get_float (const char * module, const char * name);

// update the value of an option
void option_set_int   (const char * module, const char * name, int value);
void option_set_string(const char * module, const char * name, const char * value);
void option_set_bool  (const char * module, const char * name, bool value);
void option_set_float (const char * module, const char * name, float value);

// called by the main application to parse the command line arguments
bool option_parse(int argc, char * argv[]);

// called by the main application to load configuration from a file
bool option_load(const char * filename);

// called by the main application to validate the option values
bool option_validate(void);

// print out the options, help, and their current values
void option_print(void);

// dump preset options in ini format into the file
bool option_dump_preset(FILE * file);

// final cleanup
void option_free(void);

#endif
