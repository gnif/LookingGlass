/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include "common/debug.h"

#include <unistd.h>

#define COLOR_RESET  "\033[0m"
#define COLOR_YELLOW "\033[0;33m"
#define COLOR_RED    "\033[0;31m"
#define COLOR_CYAN   "\033[0;36m"
#define COLOR_WHITE  "\033[0;37m"

const char ** debug_lookup = NULL;

void debug_init(void)
{
  static const char * colorLookup[] =
  {
    COLOR_RESET        , // DEBUG_LEVEL_NONE
    COLOR_RESET  "[I] ", // DEBUG_LEVEL_INFO
    COLOR_YELLOW "[W] ", // DEBUG_LEVEL_WARN
    COLOR_RED    "[E] ", // DEBUG_LEVEL_ERROR
    COLOR_CYAN   "[F] ", // DEBUG_LEVEL_FIXME
    COLOR_WHITE  "[!] "  // DEBUG_LEVEL_FATAL
  };

  static const char * plainLookup[] =
  {
    ""    , // DEBUG_LEVEL_NONE
    "[I] ", // DEBUG_LEVEL_INFO
    "[W] ", // DEBUG_LEVEL_WARN
    "[E] ", // DEBUG_LEVEL_ERROR
    "[F] ", // DEBUG_LEVEL_FIXME
    "[!] "  // DEBUG_LEVEL_FATAL
  };

  debug_lookup = (isatty(STDERR_FILENO) == 1) ? colorLookup : plainLookup;
}
