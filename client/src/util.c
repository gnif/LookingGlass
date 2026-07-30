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

#include "util.h"
#include "main.h"

#include "common/debug.h"
#include "common/stringutils.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fontconfig/fontconfig.h>

struct UIFontFace
{
  char * file;
  int index;
  ImVector_ImWchar ranges;
};

static FcConfig * FontConfig = NULL;
static FcPattern * UIFontQuery = NULL;
static FcPattern * UIFontPrimary = NULL;
static FcCharSet * UIFontChars = NULL;
static FcCharSet * UIFontRequested = NULL;
static struct UIFontFace * UIFontFaces = NULL;
static unsigned int UIFontFaceCount = 0;
static LG_Lock UIFontLock;
static bool UIFontLockInitialized = false;

static void uiFontClearFaces(void)
{
  for (unsigned int i = 0; i < UIFontFaceCount; ++i)
  {
    free(UIFontFaces[i].file);
    ImVector_ImWchar_UnInit(&UIFontFaces[i].ranges);
  }

  free(UIFontFaces);
  UIFontFaces = NULL;
  UIFontFaceCount = 0;
}

static void uiFontAddRanges(const ImWchar * ranges)
{
  for (; ranges[0]; ranges += 2)
    for (FcChar32 c = ranges[0]; c <= (FcChar32)ranges[1]; ++c)
      FcCharSetAddChar(UIFontChars, c);
}

static bool uiFontCharsetToRanges(
    const FcCharSet * charset, ImVector_ImWchar * ranges)
{
  ImFontGlyphRangesBuilder * builder =
    ImFontGlyphRangesBuilder_ImFontGlyphRangesBuilder();
  if (!builder)
    return false;

  FcChar32 map[FC_CHARSET_MAP_SIZE];
  FcChar32 next;
  for (FcChar32 base = FcCharSetFirstPage(charset, map, &next);
       base != FC_CHARSET_DONE;
       base = FcCharSetNextPage(charset, map, &next))
  {
    for (unsigned int i = 0; i < FC_CHARSET_MAP_SIZE; ++i)
      for (unsigned int bit = 0; bit < 32; ++bit)
      {
        if (!(map[i] & (UINT32_C(1) << bit)))
          continue;

        const FcChar32 c = base + i * 32 + bit;
        if (c >= 0x20 && c <= IM_UNICODE_CODEPOINT_MAX)
          ImFontGlyphRangesBuilder_AddChar(builder, (ImWchar)c);
      }
  }

  ImVector_ImWchar_Init(ranges);
  ImFontGlyphRangesBuilder_BuildRanges(builder, ranges);
  ImFontGlyphRangesBuilder_destroy(builder);
  return ranges->Size > 1;
}

static bool uiFontAddFace(const FcPattern * pattern, FcCharSet ** remainingPtr)
{
  FcCharSet * remaining = *remainingPtr;
  FcChar8 * file;
  FcCharSet * charset;
  FcBool outline = FcTrue;

  if (FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch ||
      FcPatternGetCharSet(pattern, FC_CHARSET, 0, &charset) != FcResultMatch)
    return true;

  if (FcPatternGetBool(pattern, FC_OUTLINE, 0, &outline) == FcResultMatch &&
      !outline)
    return true;

  FcCharSet * covered = FcCharSetIntersect(remaining, charset);
  if (!covered)
    return false;

  if (FcCharSetCount(covered) == 0)
  {
    FcCharSetDestroy(covered);
    return true;
  }

  struct UIFontFace * faces = realloc(UIFontFaces,
      sizeof(*UIFontFaces) * (UIFontFaceCount + 1));
  if (!faces)
  {
    FcCharSetDestroy(covered);
    return false;
  }
  UIFontFaces = faces;

  struct UIFontFace * face = &UIFontFaces[UIFontFaceCount];
  memset(face, 0, sizeof(*face));
  face->file = strdup((const char *)file);
  if (!face->file || !uiFontCharsetToRanges(covered, &face->ranges))
  {
    free(face->file);
    face->file = NULL;
    FcCharSetDestroy(covered);
    return false;
  }

  int index = 0;
  if (FcPatternGetInteger(pattern, FC_INDEX, 0, &index) == FcResultMatch)
    face->index = index & 0xFFFF;

  ++UIFontFaceCount;

  FcCharSet * next = FcCharSetSubtract(remaining, covered);
  FcCharSetDestroy(covered);
  if (!next)
    return false;

  FcCharSetDestroy(remaining);
  *remainingPtr = next;
  return true;
}

static bool uiFontBuildFaces(ImFontAtlas * atlas)
{
  uiFontClearFaces();

  uiFontAddRanges(ImFontAtlas_GetGlyphRangesDefault(atlas));
  uiFontAddRanges((ImWchar[]) {
    0x2190, 0x2193, // four directional arrows
    0,
  });

  FcCharSet * remaining = FcCharSetCopy(UIFontChars);
  if (!remaining)
    return false;

  if (!uiFontAddFace(UIFontPrimary, &remaining))
    goto fail;

  if (FcCharSetCount(remaining) > 0)
  {
    FcPattern * query = FcPatternDuplicate(UIFontQuery);
    if (!query)
      goto fail;

    FcPatternDel(query, FC_CHARSET);
    FcPatternAddCharSet(query, FC_CHARSET, remaining);

    FcResult result;
    FcFontSet * fonts = FcFontSort(FontConfig, query, FcTrue, NULL, &result);
    FcPatternDestroy(query);
    if (!fonts)
      goto fail;

    for (int i = 0; i < fonts->nfont && FcCharSetCount(remaining) > 0; ++i)
      if (!uiFontAddFace(fonts->fonts[i], &remaining))
      {
        FcFontSetSortDestroy(fonts);
        goto fail;
      }

    FcFontSetSortDestroy(fonts);
  }

  if (FcCharSetCount(remaining) > 0)
  {
    FcCharSet * missing = FcCharSetIntersect(remaining, UIFontRequested);
    if (missing)
    {
      if (FcCharSetCount(missing) > 0)
        DEBUG_WARN("%u requested UI glyphs have no usable outline font",
            FcCharSetCount(missing));
      FcCharSetDestroy(missing);
    }
  }

  FcCharSetDestroy(remaining);
  return UIFontFaceCount > 0;

fail:
  FcCharSetDestroy(remaining);
  uiFontClearFaces();
  return false;
}

static ImFont * uiFontAddSize(ImFontAtlas * atlas, float size)
{
  ImFont * result = NULL;

  for (unsigned int i = 0; i < UIFontFaceCount; ++i)
  {
    ImFontConfig * config = ImFontConfig_ImFontConfig();
    if (!config)
      return NULL;

    config->MergeMode = result != NULL;
    config->FontNo = UIFontFaces[i].index;

    ImFont * font = ImFontAtlas_AddFontFromFileTTF(atlas,
        UIFontFaces[i].file, size, config, UIFontFaces[i].ranges.Data);
    ImFontConfig_destroy(config);

    if (!font)
    {
      DEBUG_WARN("Failed to add UI font face: %s", UIFontFaces[i].file);
      continue;
    }

    if (!result)
      result = font;
  }

  return result;
}

bool util_fileGetContents(const char * filename, char ** buffer, size_t * length)
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
  if (fsize < 0)
  {
    DEBUG_ERROR("Failed to get the size");
    fclose(fh);
    return false;
  }

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
  (*buffer)[fsize] = 0;
  *length = fsize;
  return true;
}

void util_cursorToInt(double ex, double ey, int *x, int *y)
{
  /* only smooth if enabled and not using raw mode */
  if (g_params.mouseSmoothing && !(g_cursor.grab && g_params.rawMouse))
  {
    static struct DoublePoint last = { 0 };

    /* only apply smoothing to small deltas */
    if (fabs(ex - last.x) < 5.0 && fabs(ey - last.y) < 5.0)
    {
      ex = last.x = (last.x + ex) / 2.0;
      ey = last.y = (last.y + ey) / 2.0;
    }
    else
    {
      last.x = ex;
      last.y = ey;
    }
  }

  /* convert to int accumulating the fractional error */
  ex += g_cursor.acc.x;
  ey += g_cursor.acc.y;
  g_cursor.acc.x = modf(ex, &ex);
  g_cursor.acc.y = modf(ey, &ey);

  *x = (int)ex;
  *y = (int)ey;
}

bool util_guestCurToLocal(struct DoublePoint *local)
{
  if (!g_cursor.guest.valid || !g_state.posInfoValid)
    return false;

  const struct DoublePoint point =
  {
    .x = g_cursor.guest.x + g_cursor.guest.hx,
    .y = g_cursor.guest.y + g_cursor.guest.hy
  };

  switch((g_state.rotate + g_params.winRotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
      local->x = (point.x / g_cursor.scale.x) + g_state.dstRect.x;
      local->y = (point.y / g_cursor.scale.y) + g_state.dstRect.y;;
      break;

    case LG_ROTATE_90:
      local->x = (g_state.dstRect.x + g_state.dstRect.w) -
        point.y / g_cursor.scale.y;
      local->y = (point.x / g_cursor.scale.x) + g_state.dstRect.y;
      break;

    case LG_ROTATE_180:
      local->x = (g_state.dstRect.x + g_state.dstRect.w) -
        point.x / g_cursor.scale.x;
      local->y = (g_state.dstRect.y + g_state.dstRect.h) -
        point.y / g_cursor.scale.y;
      break;

    case LG_ROTATE_270:
      local->x = (point.y / g_cursor.scale.y) + g_state.dstRect.x;
      local->y = (g_state.dstRect.y + g_state.dstRect.h) -
        point.x / g_cursor.scale.x;
      break;
  }

  return true;
}

void util_localCurToGuest(struct DoublePoint *guest)
{
  const struct DoublePoint point =
    g_cursor.pos;

  switch((g_state.rotate + g_params.winRotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
      guest->x = (point.x - g_state.dstRect.x) * g_cursor.scale.x;
      guest->y = (point.y - g_state.dstRect.y) * g_cursor.scale.y;
      break;

    case LG_ROTATE_90:
      guest->x = (point.y - g_state.dstRect.y) * g_cursor.scale.y;
      guest->y = (g_state.dstRect.w - point.x + g_state.dstRect.x)
        * g_cursor.scale.x;
      break;

    case LG_ROTATE_180:
      guest->x = (g_state.dstRect.w - point.x + g_state.dstRect.x)
        * g_cursor.scale.x;
      guest->y = (g_state.dstRect.h - point.y + g_state.dstRect.y)
        * g_cursor.scale.y;
      break;

    case LG_ROTATE_270:
      guest->x = (g_state.dstRect.h - point.y + g_state.dstRect.y)
        * g_cursor.scale.y;
      guest->y = (point.x - g_state.dstRect.x) * g_cursor.scale.x;
      break;

    default:
      DEBUG_UNREACHABLE();
  }
}

void util_rotatePoint(struct DoublePoint *point)
{
  double temp;

  switch((g_state.rotate + g_params.winRotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
      break;

    case LG_ROTATE_90:
      temp = point->x;
      point->x =  point->y;
      point->y = -temp;
      break;

    case LG_ROTATE_180:
      point->x = -point->x;
      point->y = -point->y;
      break;

    case LG_ROTATE_270:
      temp = point->x;
      point->x = -point->y;
      point->y =  temp;
      break;
  }
}

bool util_hasGLExt(const char * exts, const char * ext)
{
  return str_containsValue(exts, ' ', ext);
}

bool util_initUIFonts(void)
{
  if (FontConfig)
    return true;

  FontConfig = FcInitLoadConfigAndFonts();

  if (!FontConfig)
  {
    DEBUG_ERROR("FcInitLoadConfigAndFonts Failed");
    return false;
  }

  UIFontChars = FcCharSetCreate();
  UIFontRequested = FcCharSetCreate();
  if (!UIFontChars || !UIFontRequested)
  {
    DEBUG_ERROR("FcCharSetCreate Failed");
    if (UIFontChars)
      FcCharSetDestroy(UIFontChars);
    if (UIFontRequested)
      FcCharSetDestroy(UIFontRequested);
    UIFontChars = NULL;
    UIFontRequested = NULL;
    FcConfigDestroy(FontConfig);
    FontConfig = NULL;
    FcFini();
    return false;
  }

  LG_LOCK_INIT(UIFontLock);
  UIFontLockInitialized = true;
  return true;
}

char * util_getUIFont(const char * fontName)
{
  char * ttf = NULL;

  FcPattern * pat = FcNameParse((const FcChar8*) fontName);
  if (!pat)
  {
    DEBUG_ERROR("FCNameParse failed");
    return NULL;
  }

  FcConfigSubstitute(FontConfig, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult result;
  FcChar8 * file = NULL;
  FcPattern * match = FcFontMatch(FontConfig, pat, &result);

  if (!match)
  {
    DEBUG_ERROR("FcFontMatch Failed");
    goto fail_parse;
  }

  FcPatternDestroy(UIFontQuery);
  FcPatternDestroy(UIFontPrimary);
  UIFontQuery = FcPatternDuplicate(pat);
  UIFontPrimary = FcPatternDuplicate(match);
  if (!UIFontQuery || !UIFontPrimary)
  {
    DEBUG_ERROR("Failed to save the UI font pattern");
    goto fail_match;
  }

  if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
    ttf = strdup((char *) file);
  else
    DEBUG_ERROR("Failed to locate the requested font: %s", fontName);

fail_match:
  FcPatternDestroy(match);

fail_parse:
  FcPatternDestroy(pat);
  return ttf;
}

bool util_uiFontAddText(const char * text)
{
  if (!UIFontChars || !text)
    return false;

  bool changed = false;
  LG_LOCK(UIFontLock);

  const char * pos = text;
  while (*pos)
  {
    unsigned int c;
    const int length = igImTextCharFromUtf8(&c, pos, NULL);
    if (length <= 0)
      break;
    pos += length;

    if (c < 0x20 || c > IM_UNICODE_CODEPOINT_MAX)
      continue;

    FcCharSetAddChar(UIFontRequested, c);
    if (!FcCharSetHasChar(UIFontChars, c) &&
        FcCharSetAddChar(UIFontChars, c))
      changed = true;
  }

  LG_UNLOCK(UIFontLock);
  return changed;
}

bool util_buildUIFontAtlas(
    ImFontAtlas * atlas, float size, ImFont ** large)
{
  if (!atlas || !UIFontQuery || !UIFontPrimary || !UIFontChars)
    return false;

  bool result = false;
  LG_LOCK(UIFontLock);

  ImFontAtlas_Clear(atlas);
  if (!uiFontBuildFaces(atlas))
  {
    DEBUG_ERROR("Failed to resolve UI fonts");
    goto done;
  }

  if (!uiFontAddSize(atlas, size))
  {
    DEBUG_ERROR("Failed to add the primary UI font");
    goto done;
  }

  *large = uiFontAddSize(atlas, size * 1.3f);
  if (!*large)
  {
    DEBUG_ERROR("Failed to add the large UI font");
    goto done;
  }

  result = ImFontAtlas_Build(atlas);

done:
  LG_UNLOCK(UIFontLock);
  return result;
}

void util_freeUIFonts(void)
{
  if (!FontConfig)
    return;

  if (UIFontLockInitialized)
    LG_LOCK(UIFontLock);

  uiFontClearFaces();
  FcCharSetDestroy(UIFontChars);
  FcCharSetDestroy(UIFontRequested);
  FcPatternDestroy(UIFontQuery);
  FcPatternDestroy(UIFontPrimary);
  UIFontChars = NULL;
  UIFontRequested = NULL;
  UIFontQuery = NULL;
  UIFontPrimary = NULL;

  if (UIFontLockInitialized)
  {
    LG_UNLOCK(UIFontLock);
    LG_LOCK_FREE(UIFontLock);
    UIFontLockInitialized = false;
  }

  FcConfigDestroy(FontConfig);
  FontConfig = NULL;
  FcFini();
}
