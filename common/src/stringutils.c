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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "common/stringutils.h"

int valloc_sprintf(char ** str, const char * format, va_list ap)
{
  if (!str)
    return -1;

  *str = NULL;

  va_list ap1;
  va_copy(ap1, ap);

  // for some reason some versions of GCC warn about format being NULL when any
  // kind of optimization is enabled, this is a false positive.
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wformat-truncation"
  const int len = vsnprintf(*str, 0, format, ap1);
  #pragma GCC diagnostic pop

  va_end(ap1);

  if (len < 0)
    return len;

  *str = malloc(len + 1);

  int ret = vsnprintf(*str, len + 1, format, ap);
  if (ret < 0)
  {
    free(*str);
    *str = NULL;
    return ret;
  }

  return ret;
}

int alloc_sprintf(char ** str, const char * format, ...)
{
  va_list ap;
  va_start(ap, format);
  int ret = valloc_sprintf(str, format, ap);
  va_end(ap);
  return ret;
}

bool str_containsValue(const char * list, char delimiter, const char * value)
{
  size_t len = strlen(value);
  const char span[] = {delimiter, '\0'};

  while (*list)
  {
    if (*list == delimiter)
    {
      ++list;
      continue;
    }
    size_t n = strcspn(list, span);
    if (n == len && strncmp(value, list, n) == 0)
      return true;
    list += n;
  }
  return false;
}
