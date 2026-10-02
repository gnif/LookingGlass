/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

#ifndef _H_LG_COMMON_FILESYSTEM_
#define _H_LG_COMMON_FILESYSTEM_

#include "common/stringlist.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum LGFileType
{
  LG_FILE_TYPE_ERROR,
  LG_FILE_TYPE_MISSING,
  LG_FILE_TYPE_REGULAR,
  LG_FILE_TYPE_DIRECTORY,
  LG_FILE_TYPE_OTHER,
}
LGFileType;

LGFileType lgPathType        (const char * path);
LGFileType lgPathTypeNoFollow(const char * path);
bool       lgCreateDirectory (const char * path);
bool       lgDeleteFile      (const char * path);
bool       lgDeleteDirectory (const char * path);
FILE     * lgOpenFile        (const char * path, const char * mode);
bool       lgFileErrorString (int error, char * buffer, size_t size);

/* Returns an owned list of entry names, or NULL when the directory cannot be
 * opened or read. The special entries "." and ".." are omitted. */
StringList lgListDirectory   (const char * path);

#endif
