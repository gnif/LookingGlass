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

#include "../renderers/EGL/postprocess_order.h"

static void checkFilterOrder(EGL_Filter ** filters, EGL_Filter ** expected,
    size_t count)
{
  for (size_t i = 0; i < count; ++i)
    CHECK(filters[i] == expected[i]);
}

int main(void)
{
  EGL_Filter first    = { .ops = { .id = "first"     } };
  EGL_Filter second   = { .ops = { .id = "second"    } };
  EGL_Filter third    = { .ops = { .id = "third"     } };
  EGL_Filter unknownA = { .ops = { .id = "unknown-a" } };
  EGL_Filter unknownB = { .ops = { .id = "unknown-b" } };

  EGL_Filter * filters [] =
  {
    &unknownA,
    &second,
    &unknownB,
    &first,
    &third,
  };
  EGL_Filter * expected[] =
  {
    &first,
    &second,
    &third,
    &unknownA,
    &unknownB,
  };

  StringList order = stringlist_new(false);
  CHECK(order);
  CHECK(stringlist_push(order, "first")  == 0);
  CHECK(stringlist_push(order, "second") == 1);
  CHECK(stringlist_push(order, "third")  == 2);

  const size_t count = sizeof(filters) / sizeof(*filters);
  egl_postProcessOrderFilters(filters, count, order);
  checkFilterOrder(filters, expected, count);

  stringlist_clear(order);
  egl_postProcessOrderFilters(filters, count, order);
  checkFilterOrder(filters, expected, count);

  stringlist_free(&order);
  return EXIT_SUCCESS;
}
