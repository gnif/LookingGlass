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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void testInvalidFontData(void)
{
  const unsigned char emptySfnt[12] = {
    0x00, 0x01, 0x00, 0x00,
  };
  assert(!font_isStbTruetypeCompatible(
        emptySfnt, sizeof(emptySfnt), 0));

  const unsigned char truncatedTableDirectory[12] = {
    0x00, 0x01, 0x00, 0x00,
    0x00, 0x01,
  };
  assert(!font_isStbTruetypeCompatible(
        truncatedTableDirectory, sizeof(truncatedTableDirectory), 0));
}

static void testValidFontData(void)
{
  FILE * file = fopen(FONT_TEST_FILE, "rb");
  assert(file);
  assert(fseek(file, 0, SEEK_END) == 0);
  const long length = ftell(file);
  assert(length > 0);
  assert(fseek(file, 0, SEEK_SET) == 0);

  void * data = malloc((size_t)length);
  assert(data);
  assert(fread(data, 1, (size_t)length, file) == (size_t)length);
  assert(fclose(file) == 0);

  assert(font_isStbTruetypeCompatible(data, (size_t)length, 0));
  assert(!font_isStbTruetypeCompatible(data, (size_t)length, 1));
  free(data);
}

int main(void)
{
  testInvalidFontData();
  testValidFontData();
  return 0;
}
