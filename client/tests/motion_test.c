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

#include "motion.h"
#include "test.h"

int main(void)
{
  struct WlMotion absFirst = { 0 };
  struct WlMotion relFirst = { 0 };

  wlMotionAbs(&absFirst, 100, 50);
  wlMotionRel(&absFirst, 5, -2);
  wlMotionRel(&relFirst, 5, -2);
  wlMotionAbs(&relFirst, 100, 50);

  CHECK(absFirst.x == 105);
  CHECK(absFirst.y == 48);
  CHECK(relFirst.x == 100);
  CHECK(relFirst.y == 50);
  return 0;
}
