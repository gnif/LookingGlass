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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static int valloc_sprintf(char ** str, const char * format, va_list ap)
{
  if (!str)
    return -1;

  *str = NULL;

  va_list ap1;
  va_copy(ap1, ap);
  int len = vsnprintf(NULL, 0, format, ap1);
  va_end(ap1);

  if (len < 0)
    return len;

  *str = malloc(len+1);

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