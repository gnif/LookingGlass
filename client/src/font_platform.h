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

#ifndef _H_LG_FONT_PLATFORM_
#define _H_LG_FONT_PLATFORM_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct FontPlatform FontPlatform;

typedef enum FontPlatformFaceResult
{
  FONT_PLATFORM_FACE_ERROR,
  FONT_PLATFORM_FACE_SKIP,
  FONT_PLATFORM_FACE_ACCEPT,
}
FontPlatformFaceResult;

typedef FontPlatformFaceResult (*FontPlatformAddFaceFn)(
    void * opaque, const char * file, int index,
    const uint32_t * glyphs, size_t glyphCount);

bool font_platformInit(FontPlatform ** platform);
void font_platformFree(FontPlatform * platform);

char * font_platformSelect(FontPlatform * platform, const char * fontName);

bool font_platformResolve(FontPlatform * platform,
    const uint32_t * glyphs, size_t glyphCount,
    FontPlatformAddFaceFn addFace, void * opaque, size_t * unresolved);

#ifdef __cplusplus
}
#endif

#endif
