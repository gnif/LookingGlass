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

#include "postprocess_order.h"

#include <limits.h>
#include <string.h>

static unsigned int stringListIndex(StringList list, const char * str)
{
  const unsigned int count = stringlist_count(list);
  for (unsigned int i = 0; i < count; ++i)
    if (strcmp(stringlist_at(list, i), str) == 0)
      return i;
  return UINT_MAX;
}

static bool filterAfter(const EGL_Filter * a, const EGL_Filter * b,
    StringList order)
{
  const unsigned int aIndex = stringListIndex(order, a->ops.id);
  const unsigned int bIndex = stringListIndex(order, b->ops.id);
  return aIndex > bIndex;
}

void egl_postProcessOrderFilters(EGL_Filter ** filters, size_t count,
    StringList order)
{
  for (size_t i = 1; i < count; ++i)
  {
    EGL_Filter * filter = filters[i];
    size_t       j      = i;
    while (j > 0 && filterAfter(filters[j - 1], filter, order))
    {
      filters[j] = filters[j - 1];
      --j;
    }
    filters[j] = filter;
  }
}
