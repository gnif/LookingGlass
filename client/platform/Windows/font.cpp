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

#include <windows.h>
#include <dwrite.h>

#include "font_platform.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C"
{
  #include "common/debug.h"
  #include "common/win32.h"
}

struct FontPlatform
{
  IDWriteFactory        * factory;
  IDWriteFontCollection * collection;
  IDWriteFont           * primary;
  UINT32                  primaryFamily;
  bool                    comInitialized;
};

static bool font_platformGetFacePath(
    IDWriteFontFace * face, char ** path, int * index)
{
  *path  = NULL;
  *index = 0;

  UINT32  fileCount = 0;
  HRESULT result    = face->GetFiles(&fileCount, NULL);
  if (FAILED(result) || fileCount != 1)
    return false;

  IDWriteFontFile * file = NULL;
  result = face->GetFiles(&fileCount, &file);
  if (FAILED(result) || !file)
    return false;

  IDWriteFontFileLoader * loader = NULL;
  result = file->GetLoader(&loader);
  if (FAILED(result) || !loader)
  {
    file->Release();
    return false;
  }

  IDWriteLocalFontFileLoader * local = NULL;
  result = loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
      reinterpret_cast<void **>(&local));
  if (FAILED(result) || !local)
  {
    loader->Release();
    file->Release();
    return false;
  }

  const void * key          = NULL;
  UINT32       keySize      = 0;
  UINT32       pathLength   = 0;
  UINT32       pathCapacity = 0;
  size_t       pathSize     = 0;
  wchar_t    * widePath     = NULL;
  UINT32       faceIndex    = 0;

  result = file->GetReferenceKey(&key, &keySize);
  if (FAILED(result))
    goto done;

  result = local->GetFilePathLengthFromKey(key, keySize, &pathLength);
  if (FAILED(result))
    goto done;

  pathCapacity = pathLength + 1;
  if (!pathCapacity)
    goto done;

  pathSize = static_cast<size_t>(pathCapacity) * sizeof(wchar_t);
  if (pathSize / sizeof(wchar_t) != pathCapacity)
    goto done;

  widePath = static_cast<wchar_t *>(malloc(pathSize));
  if (!widePath)
    goto done;

  result = local->GetFilePathFromKey(
      key, keySize, widePath, pathCapacity);
  if (SUCCEEDED(result))
    *path = lgWideToUtf8(widePath);
  free(widePath);

  faceIndex = face->GetIndex();
  if (!*path || faceIndex > INT_MAX)
  {
    free(*path);
    *path = NULL;
    goto done;
  }

  *index = static_cast<int>(faceIndex);

done:
  local->Release();
  loader->Release();
  file->Release();
  return *path != NULL;
}

static bool font_platformGetFontPath(
    IDWriteFont * font, char ** path, int * index)
{
  IDWriteFontFace * face   = NULL;
  const HRESULT     result = font->CreateFontFace(&face);
  if (FAILED(result) || !face)
    return false;

  const bool success = font_platformGetFacePath(face, path, index);
  face->Release();
  return success;
}

static bool font_platformGetFamilyFont(FontPlatform * platform,
    UINT32 familyIndex, IDWriteFont ** font)
{
  *font = NULL;

  IDWriteFontFamily * family = NULL;
  HRESULT             result = platform->collection->GetFontFamily(
      familyIndex, &family);
  if (FAILED(result) || !family)
    return false;

  result = family->GetFirstMatchingFont(
      DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STRETCH_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, font);
  family->Release();
  return SUCCEEDED(result) && *font;
}

static bool font_platformFindFamily(FontPlatform * platform,
    const wchar_t * name, UINT32 * familyIndex)
{
  BOOL          exists = FALSE;
  const HRESULT result = platform->collection->FindFamilyName(
      name, familyIndex, &exists);
  return SUCCEEDED(result) && exists;
}

static bool font_platformAddFont(IDWriteFont * font,
    uint32_t * remaining, size_t * remainingCount,
    FontPlatformAddFaceFn addFace, void * opaque)
{
  if (!*remainingCount)
    return true;

  if (*remainingCount > UINT32_MAX ||
      *remainingCount > SIZE_MAX / sizeof(uint16_t) ||
      *remainingCount > SIZE_MAX / sizeof(uint32_t))
    return false;

  IDWriteFontFace * face   = NULL;
  HRESULT           result = font->CreateFontFace(&face);
  if (FAILED(result) || !face)
    return true;

  uint16_t * indices = static_cast<uint16_t *>(
      malloc(*remainingCount * sizeof(*indices)));
  uint32_t * covered = static_cast<uint32_t *>(
      malloc(*remainingCount * sizeof(*covered)));
  if (!indices || !covered)
  {
    free(indices);
    free(covered);
    face->Release();
    return false;
  }

  result = face->GetGlyphIndices(
      remaining, static_cast<UINT32>(*remainingCount), indices);
  if (FAILED(result))
  {
    free(indices);
    free(covered);
    face->Release();
    return true;
  }

  size_t coveredCount = 0;
  for (size_t i = 0; i < *remainingCount; ++i)
    if (indices[i])
      covered[coveredCount++] = remaining[i];
  free(indices);

  if (!coveredCount)
  {
    free(covered);
    face->Release();
    return true;
  }

  char * path;
  int    index;
  if (!font_platformGetFacePath(face, &path, &index))
  {
    free(covered);
    face->Release();
    return true;
  }

  face->Release();
  const FontPlatformFaceResult faceResult = addFace(
      opaque, path, index, covered, coveredCount);
  free(path);
  if (faceResult == FONT_PLATFORM_FACE_ERROR)
  {
    free(covered);
    return false;
  }

  if (faceResult == FONT_PLATFORM_FACE_ACCEPT)
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

  FontPlatform * state = static_cast<FontPlatform *>(
      calloc(1, sizeof(*state)));
  if (!state)
    return false;

  HRESULT result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (SUCCEEDED(result))
    state->comInitialized = true;
  else if (result != RPC_E_CHANGED_MODE)
  {
    DEBUG_ERROR("CoInitializeEx failed: 0x%08lx",
        static_cast<unsigned long>(result));
    free(state);
    return false;
  }

  result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
      __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown **>(&state->factory));
  if (FAILED(result) || !state->factory)
  {
    DEBUG_ERROR("DWriteCreateFactory failed: 0x%08lx",
        static_cast<unsigned long>(result));
    if (state->comInitialized)
      CoUninitialize();
    free(state);
    return false;
  }

  result = state->factory->GetSystemFontCollection(
      &state->collection, FALSE);
  if (FAILED(result) || !state->collection)
  {
    DEBUG_ERROR("GetSystemFontCollection failed: 0x%08lx",
        static_cast<unsigned long>(result));
    state->factory->Release();
    if (state->comInitialized)
      CoUninitialize();
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

  if (platform->primary)
    platform->primary->Release();
  if (platform->collection)
    platform->collection->Release();
  if (platform->factory)
    platform->factory->Release();
  if (platform->comInitialized)
    CoUninitialize();

  free(platform);
}

char * font_platformSelect(FontPlatform * platform, const char * fontName)
{
  if (!platform || !fontName)
    return NULL;

  const size_t familyLength = strcspn(fontName, ":");
  if (!familyLength || familyLength == SIZE_MAX)
    return NULL;

  char * familyName = static_cast<char *>(malloc(familyLength + 1));
  if (!familyName)
    return NULL;
  memcpy(familyName, fontName, familyLength);
  familyName[familyLength] = '\0';

  wchar_t * wideName = lgUtf8ToWide(familyName);
  free(familyName);
  if (!wideName)
    return NULL;

  static const wchar_t * fallbackNames[] =
  {
    L"Consolas",
    L"Segoe UI",
  };

  IDWriteFont * selected       = NULL;
  UINT32        selectedFamily = 0;
  char        * selectedPath   = NULL;
  int           selectedIndex;

  UINT32 familyIndex;
  if (font_platformFindFamily(platform, wideName, &familyIndex) &&
      font_platformGetFamilyFont(platform, familyIndex, &selected) &&
      font_platformGetFontPath(selected, &selectedPath, &selectedIndex))
    selectedFamily = familyIndex;
  else
  {
    if (selected)
    {
      selected->Release();
      selected = NULL;
    }

    for (size_t i = 0;
         i < sizeof(fallbackNames) / sizeof(fallbackNames[0]); ++i)
    {
      if (!font_platformFindFamily(
            platform, fallbackNames[i], &familyIndex) ||
          !font_platformGetFamilyFont(platform, familyIndex, &selected) ||
          !font_platformGetFontPath(selected, &selectedPath, &selectedIndex))
      {
        if (selected)
        {
          selected->Release();
          selected = NULL;
        }
        continue;
      }

      selectedFamily = familyIndex;
      break;
    }

    if (!selected &&
        platform->collection->GetFontFamilyCount() &&
        font_platformGetFamilyFont(platform, 0, &selected) &&
        font_platformGetFontPath(selected, &selectedPath, &selectedIndex))
      selectedFamily = 0;
  }

  free(wideName);
  if (!selected || !selectedPath)
  {
    if (selected)
      selected->Release();
    DEBUG_ERROR("Failed to locate the requested font: %s", fontName);
    return NULL;
  }

  if (platform->primary)
    platform->primary->Release();
  platform->primary       = selected;
  platform->primaryFamily = selectedFamily;
  return selectedPath;
}

bool font_platformResolve(FontPlatform * platform,
    const uint32_t * glyphs, size_t glyphCount,
    FontPlatformAddFaceFn addFace, void * opaque, size_t * unresolved)
{
  if (!platform || !platform->primary || (!glyphs && glyphCount) ||
      !addFace || !unresolved ||
      glyphCount > SIZE_MAX / sizeof(*glyphs))
    return false;

  uint32_t * remaining = static_cast<uint32_t *>(
      malloc(glyphCount * sizeof(*remaining)));
  if (!remaining && glyphCount)
    return false;
  if (glyphCount)
    memcpy(remaining, glyphs, glyphCount * sizeof(*remaining));

  const UINT32 familyCount    = platform->collection->GetFontFamilyCount();
  size_t       remainingCount = glyphCount;
  if (!font_platformAddFont(platform->primary, remaining,
        &remainingCount, addFace, opaque))
    goto fail;

  for (UINT32 i = 0; i < familyCount && remainingCount; ++i)
  {
    if (i == platform->primaryFamily)
      continue;

    IDWriteFont * font = NULL;
    if (!font_platformGetFamilyFont(platform, i, &font))
      continue;

    const bool success = font_platformAddFont(
        font, remaining, &remainingCount, addFace, opaque);
    font->Release();
    if (!success)
      goto fail;
  }

  *unresolved = remainingCount;
  free(remaining);
  return true;

fail:
  free(remaining);
  return false;
}
