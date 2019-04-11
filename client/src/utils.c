/*
Looking Glass - KVM FrameRelay (KVMFR) Client
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

#include "utils.h"
#include "common/debug.h"

#include <stdlib.h>
#include <stdio.h>

bool file_get_contents(const char * filename, char ** buffer, size_t * length)
{
  FILE * fh = fopen(filename, "r");
  if (!fh)
  {
    DEBUG_ERROR("Failed to open the file: %s", filename);
    return false;
  }

  if (fseek(fh, 0, SEEK_END) != 0)
  {
    DEBUG_ERROR("Failed to seek");
    fclose(fh);
    return false;
  }

  long fsize = ftell(fh);
  if (fseek(fh, 0, SEEK_SET) != 0)
  {
    DEBUG_ERROR("Failed to seek");
    fclose(fh);
    return false;
  }

  *buffer = malloc(fsize + 1);
  if (!*buffer)
  {
    DEBUG_ERROR("Failed to allocate buffer of %lu bytes", fsize + 1);
    fclose(fh);
    return false;
  }

  if (fread(*buffer, 1, fsize, fh) != fsize)
  {
    DEBUG_ERROR("Failed to read the entire file");
    fclose(fh);
    free(*buffer);
    return false;
  }

  fclose(fh);
  buffer[fsize] = 0;
  *length = fsize;
  return true;
}