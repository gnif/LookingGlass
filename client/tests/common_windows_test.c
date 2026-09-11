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

#include "test.h"

#include "common/win32.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void testUtfConversion(void)
{
  static const char input[] = "Looking Glass \xF0\x9F\x96\xA5";

  wchar_t * wide = lgUtf8ToWide(input);
  CHECK(wide);
  CHECK(wcslen(wide) > 0);

  char * output = lgWideToUtf8(wide);
  CHECK(output);
  CHECK(strcmp(output, input) == 0);
  free(output);
  free(wide);

  CHECK(!lgUtf8ToWide("\xC0\xAF"));
}

int main(void)
{
  testUtfConversion();
  return 0;
}
