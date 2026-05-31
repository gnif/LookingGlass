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

#include "cursor.h"
#include "common/util.h"

#include <string.h>
#include <errno.h>

struct MemFile
{
  const char * data;
  int          size;
  int          pos;
};

static int x11cursor_read(XcursorFile *file, unsigned char * buf, int len)
{
  struct MemFile * f = (struct MemFile *)file->closure;

  if (f->pos == f->size)
    return 0;

  len = min(f->size - f->pos, len);
  memcpy(buf, f->data + f->pos, len);
  f->pos += len;

  return len;
}

static int x11cursor_write(XcursorFile *file, unsigned char * buf, int len)
{
  errno = -EINVAL;
  return -1;
}

static int x11cursor_seek(XcursorFile *file, long offset, int whence)
{
  struct MemFile * f = (struct MemFile *)file->closure;
  long target;

  switch(whence)
  {
    case SEEK_SET:
      target = offset;
      break;

    case SEEK_CUR:
      target = f->pos + offset;
      break;

    case SEEK_END:
      target = f->size + offset;
      break;

    default:
      errno = -EINVAL;
      return -1;
  }

  if (target < 0 || target > f->size)
  {
    errno = -EINVAL;
    return -1;
  }
  f->pos = target;
  return target;
}

XcursorImages * x11cursor_load(const char * cursor, int size)
{
  struct MemFile closure =
  {
    .data = cursor,
    .size = size,
    .pos  = 0
  };

  XcursorFile f =
  {
    .closure = &closure,
    .read    = x11cursor_read,
    .write   = x11cursor_write,
    .seek    = x11cursor_seek
  };

  return XcursorXcFileLoadAllImages(&f);
}
