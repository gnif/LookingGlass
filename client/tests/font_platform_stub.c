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

#include "font_platform.h"
#include "font_platform_stub.h"

#include <stdlib.h>
#include <string.h>

struct FontPlatform
{
  unsigned int unused;
};

static FontPlatformStubStats stats;

const FontPlatformStubStats * font_platformStubStats(void)
{
  return &stats;
}

bool font_platformInit(FontPlatform ** platform)
{
  if (!platform)
    return false;

  *platform = calloc(1, sizeof(**platform));
  if (!*platform)
    return false;

  ++stats.initCount;
  return true;
}

void font_platformFree(FontPlatform * platform)
{
  if (!platform)
    return;

  ++stats.freeCount;
  free(platform);
}

char * font_platformSelect(FontPlatform * platform, const char * fontName)
{
  if (!platform || !fontName)
    return NULL;

  ++stats.selectCount;
  const size_t length = strlen(FONT_TEST_FILE) + 1;
  char       * path   = malloc(length);
  if (path)
    memcpy(path, FONT_TEST_FILE, length);
  return path;
}

bool font_platformResolve(FontPlatform * platform,
    const uint32_t * glyphs, size_t glyphCount,
    FontPlatformAddFaceFn addFace, void * opaque, size_t * unresolved)
{
  if (!platform || (!glyphs && glyphCount) || !addFace || !unresolved ||
      glyphCount > SIZE_MAX / sizeof(*glyphs))
    return false;

  ++stats.resolveCount;
  uint32_t * primary  = malloc(glyphCount * sizeof(*primary));
  uint32_t * fallback = malloc(glyphCount * sizeof(*fallback));
  if ((!primary || !fallback) && glyphCount)
  {
    free(primary);
    free(fallback);
    return false;
  }

  size_t primaryCount  = 0;
  size_t fallbackCount = 0;
  for (size_t i = 0; i < glyphCount; ++i)
    if (glyphs[i] <= 0xff)
      primary[primaryCount++] = glyphs[i];
    else
      fallback[fallbackCount++] = glyphs[i];

  FontPlatformFaceResult result = FONT_PLATFORM_FACE_ACCEPT;
  if (primaryCount)
    result = addFace(
        opaque, FONT_TEST_FILE, 0, primary, primaryCount);
  if (result == FONT_PLATFORM_FACE_ERROR)
    goto fail;

  size_t missing = result == FONT_PLATFORM_FACE_ACCEPT ? 0 : primaryCount;
  if (fallbackCount)
  {
    result = addFace(opaque, FONT_TEST_FILE, 1,
        fallback, fallbackCount);
    if (result == FONT_PLATFORM_FACE_ERROR)
      goto fail;
    if (result == FONT_PLATFORM_FACE_SKIP)
      ++stats.incompatibleSkipCount;
    else
      goto fail;

    result = addFace(opaque, FONT_TEST_FILE, 0,
        fallback, fallbackCount);
    if (result == FONT_PLATFORM_FACE_ERROR)
      goto fail;
    if (result == FONT_PLATFORM_FACE_ACCEPT)
    {
      ++stats.fallbackAcceptCount;
    }
    else
      missing += fallbackCount;
  }

  *unresolved = missing;
  free(primary);
  free(fallback);
  return true;

fail:
  free(primary);
  free(fallback);
  return false;
}
