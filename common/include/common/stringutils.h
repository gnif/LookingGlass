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

#ifndef _H_LG_COMMON_STRINGUTILS
#define _H_LG_COMMON_STRINGUTILS

#include <stdbool.h>

// vsprintf but with buffer allocation
int valloc_sprintf(char ** str, const char * format, va_list ap)
  __attribute__ ((format (printf, 2, 0)));

// sprintf but with buffer allocation
int alloc_sprintf(char ** str, const char * format, ...)
  __attribute__ ((format (printf, 2, 3)));

// Find value in a list separated by delimiter.
bool str_containsValue(const char * list, char delimiter, const char * value);

#endif
