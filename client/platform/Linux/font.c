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

#include "common/debug.h"

#include <fontconfig/fontconfig.h>
#include <stdlib.h>
#include <string.h>

struct FontPlatform
{
  FcConfig  * config;
  FcPattern * query;
  FcPattern * primary;
};

static bool font_platformAddPattern(const FcPattern * pattern,
    uint32_t * remaining,
    size_t * remainingCount, FontPlatformAddFaceFn addFace, void * opaque)
{
  FcChar8   * file;
  FcCharSet * charset;
  FcBool      outline = FcTrue;
  int         index   = 0;

  if (FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch ||
      FcPatternGetCharSet(pattern, FC_CHARSET, 0, &charset) != FcResultMatch)
    return true;

  if (FcPatternGetBool(pattern, FC_OUTLINE, 0, &outline) == FcResultMatch &&
      !outline)
    return true;

  if (FcPatternGetInteger(pattern, FC_INDEX, 0, &index) == FcResultMatch)
    index &= 0xffff;

  if (*remainingCount > SIZE_MAX / sizeof(*remaining))
    return false;

  uint32_t * covered = malloc(*remainingCount * sizeof(*covered));
  if (!covered && *remainingCount)
    return false;

  size_t coveredCount = 0;
  for (size_t i = 0; i < *remainingCount; ++i)
    if (FcCharSetHasChar(charset, remaining[i]))
      covered[coveredCount++] = remaining[i];

  if (!coveredCount)
  {
    free(covered);
    return true;
  }

  const FontPlatformFaceResult result = addFace(opaque,
      (const char *)file, index, covered, coveredCount);
  if (result == FONT_PLATFORM_FACE_ERROR)
  {
    free(covered);
    return false;
  }

  if (result == FONT_PLATFORM_FACE_ACCEPT)
  {
    size_t src       = 0;
    size_t dst       = 0;
    size_t coveredAt = 0;

    while (src < *remainingCount)
    {
      if (coveredAt < coveredCount &&
          remaining[src] == covered[coveredAt])
      {
        ++src;
        ++coveredAt;
        continue;
      }

      remaining[dst++] = remaining[src++];
    }

    *remainingCount = dst;
  }

  free(covered);
  return true;
}

bool font_platformInit(FontPlatform ** platform)
{
  if (!platform)
    return false;

  *platform = NULL;

  FontPlatform * state = calloc(1, sizeof(*state));
  if (!state)
    return false;

  state->config = FcInitLoadConfigAndFonts();
  if (!state->config)
  {
    DEBUG_ERROR("FcInitLoadConfigAndFonts failed");
    free(state);
    return false;
  }

  *platform = state;
  return true;
}

void font_platformFree(FontPlatform * platform)
{
  if (!platform)
    return;

  if (platform->query)
    FcPatternDestroy(platform->query);
  if (platform->primary)
    FcPatternDestroy(platform->primary);
  if (platform->config)
    FcConfigDestroy(platform->config);

  free(platform);
  FcFini();
}

char * font_platformSelect(FontPlatform * platform, const char * fontName)
{
  if (!platform || !fontName)
    return NULL;

  FcPattern * pattern = FcNameParse((const FcChar8 *)fontName);
  if (!pattern)
  {
    DEBUG_ERROR("FcNameParse failed");
    return NULL;
  }

  FcConfigSubstitute(platform->config, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);

  FcResult    result;
  FcPattern * match = FcFontMatch(platform->config, pattern, &result);
  if (!match)
  {
    DEBUG_ERROR("FcFontMatch failed");
    FcPatternDestroy(pattern);
    return NULL;
  }

  FcChar8 * file;
  if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch)
  {
    DEBUG_ERROR("Failed to locate the requested font: %s", fontName);
    FcPatternDestroy(match);
    FcPatternDestroy(pattern);
    return NULL;
  }

  FcPattern * query      = FcPatternDuplicate(pattern);
  FcPattern * primary    = FcPatternDuplicate(match);
  char      * resultPath = strdup((const char *)file);
  if (!query || !primary || !resultPath)
  {
    if (query)
      FcPatternDestroy(query);
    if (primary)
      FcPatternDestroy(primary);
    free(resultPath);
    FcPatternDestroy(match);
    FcPatternDestroy(pattern);
    return NULL;
  }

  if (platform->query)
    FcPatternDestroy(platform->query);
  if (platform->primary)
    FcPatternDestroy(platform->primary);

  platform->query   = query;
  platform->primary = primary;

  FcPatternDestroy(match);
  FcPatternDestroy(pattern);
  return resultPath;
}

bool font_platformResolve(FontPlatform * platform,
    const uint32_t * glyphs, size_t glyphCount,
    FontPlatformAddFaceFn addFace, void * opaque, size_t * unresolved)
{
  if (!platform || !platform->query || !platform->primary ||
      (!glyphs && glyphCount) || !addFace || !unresolved ||
      glyphCount > SIZE_MAX / sizeof(*glyphs))
    return false;

  uint32_t * remaining = malloc(glyphCount * sizeof(*remaining));
  if (!remaining && glyphCount)
    return false;

  if (glyphCount)
    memcpy(remaining, glyphs, glyphCount * sizeof(*remaining));
  size_t remainingCount = glyphCount;

  if (!font_platformAddPattern(platform->primary, remaining,
        &remainingCount, addFace, opaque))
    goto fail;

  if (remainingCount)
  {
    FcCharSet * charset = FcCharSetCreate();
    FcPattern * query   = FcPatternDuplicate(platform->query);
    if (!charset || !query)
    {
      if (charset)
        FcCharSetDestroy(charset);
      if (query)
        FcPatternDestroy(query);
      goto fail;
    }

    for (size_t i = 0; i < remainingCount; ++i)
      if (!FcCharSetAddChar(charset, remaining[i]))
      {
        FcPatternDestroy(query);
        FcCharSetDestroy(charset);
        goto fail;
      }

    FcPatternDel(query, FC_CHARSET);
    if (!FcPatternAddCharSet(query, FC_CHARSET, charset))
    {
      FcPatternDestroy(query);
      FcCharSetDestroy(charset);
      goto fail;
    }

    FcResult   result;
    FcFontSet * fonts = FcFontSort(
        platform->config, query, FcFalse, NULL, &result);
    FcPatternDestroy(query);
    FcCharSetDestroy(charset);
    if (!fonts)
      goto fail;

    for (int i = 0; i < fonts->nfont && remainingCount; ++i)
      if (!font_platformAddPattern(fonts->fonts[i], remaining,
            &remainingCount, addFace, opaque))
      {
        FcFontSetSortDestroy(fonts);
        goto fail;
      }

    FcFontSetSortDestroy(fonts);
  }

  *unresolved = remainingCount;
  free(remaining);
  return true;

fail:
  free(remaining);
  return false;
}
