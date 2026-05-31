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
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include "common/stringutils.h"
#include "common/debug.h"

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

char * lg_strdup(const char *s)
{
  if (!s)
  {
    errno = EINVAL;
    return NULL;
  }

  const ssize_t len = strlen(s) + 1;
  char * out = malloc(len);
  if (!out)
  {
    errno = ENOMEM;
    return NULL;
  }

  memcpy(out, s, len);
  return out;
}

const char * memsearch(
    const char * haystack, size_t haystackSize,
    const char * needle  , size_t needleSize  ,
    const char * offset)
{
  int i = 0;
  if (offset)
  {
    DEBUG_ASSERT(offset >= haystack);
    DEBUG_ASSERT(offset < haystack + haystackSize);
    i = offset - haystack;
  }

  const int searchSize = haystackSize - needleSize + 1;
  for(; i < searchSize; ++i)
    if (memcmp(haystack + i, needle, needleSize) == 0)
      return haystack + i;

  return NULL;
}
