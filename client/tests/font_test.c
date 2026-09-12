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

#include "font.h"
#include "font_platform_stub.h"
#include "test.h"

#include "common/debug.h"

#include "cimgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void testInvalidFontData(void)
{
  const unsigned char emptySfnt[12] = {
    0x00, 0x01, 0x00, 0x00,
  };
  CHECK(!font_isStbTruetypeCompatible(
        emptySfnt, sizeof(emptySfnt), 0));

  const unsigned char truncatedTableDirectory[12] = {
    0x00, 0x01, 0x00, 0x00,
    0x00, 0x01,
  };
  CHECK(!font_isStbTruetypeCompatible(
        truncatedTableDirectory, sizeof(truncatedTableDirectory), 0));
}

static void testValidFontData(void)
{
  FILE * file = fopen(FONT_TEST_FILE, "rb");
  CHECK(file);
  CHECK(fseek(file, 0, SEEK_END) == 0);
  const long length = ftell(file);
  CHECK(length > 0);
  CHECK(fseek(file, 0, SEEK_SET) == 0);

  void * data = malloc((size_t)length);
  CHECK(data);
  CHECK(fread(data, 1, (size_t)length, file) == (size_t)length);
  CHECK(fclose(file) == 0);

  CHECK(font_isStbTruetypeCompatible(data, (size_t)length, 0));
  CHECK(!font_isStbTruetypeCompatible(data, (size_t)length, 1));
  free(data);
}

static void testFontLifecycle(void)
{
  const FontPlatformStubStats * stats = font_platformStubStats();

  CHECK(!font_addText("before init"));
  CHECK(font_init());
  CHECK(font_init());
  CHECK(stats->initCount == 1);

  char * path = font_get("test font");
  CHECK(path);
  CHECK(strcmp(path, FONT_TEST_FILE) == 0);
  free(path);
  CHECK(stats->selectCount == 1);

  const char text[] = "fallback \xE4\xB8\xAD";
  CHECK(font_addText(text));
  CHECK(!font_addText(text));

  ImFontAtlas * atlas = ImFontAtlas_ImFontAtlas();
  CHECK(atlas);

  ImFont * large = NULL;
  CHECK(font_buildAtlas(atlas, 14.0f, &large));
  CHECK(large);
  CHECK(stats->resolveCount == 1);
  CHECK(stats->incompatibleSkipCount == 1);
  CHECK(stats->fallbackAcceptCount == 1);

  large = NULL;
  CHECK(font_buildAtlas(atlas, 18.0f, &large));
  CHECK(large);
  CHECK(stats->resolveCount == 2);
  CHECK(stats->incompatibleSkipCount == 2);
  CHECK(stats->fallbackAcceptCount == 2);

  ImFontAtlas_destroy(atlas);
  font_free();
  font_free();
  CHECK(stats->freeCount == 1);
  CHECK(!font_addText("after free"));

  CHECK(font_init());
  CHECK(stats->initCount == 2);
  font_free();
  CHECK(stats->freeCount == 2);
}

int main(void)
{
  debug_init();
  testInvalidFontData();
  testValidFontData();
  testFontLifecycle();
  return 0;
}
