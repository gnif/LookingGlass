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

#pragma once

static const DWORD DefaultDisplayModes[][2] =
{
  {7680, 4800}, {7680, 4320}, {6016, 3384}, {5760, 3600},
  {5760, 3240}, {5120, 2800}, {4096, 2560}, {4096, 2304},
  {3840, 2400}, {3840, 2160}, {3200, 2400}, {3200, 1800},
  {3008, 1692}, {2880, 1800}, {2880, 1620}, {2560, 1600},
  {2560, 1440}, {1920, 1440}, {1920, 1200}, {1920, 1080},
  {1600, 1200}, {1600, 1024}, {1600, 1050}, {1600, 900 },
  {1440, 900 }, {1400, 1050}, {1366, 768 }, {1360, 768 },
  {1280, 1024}, {1280, 960 }, {1280, 800 }, {1280, 768 },
  {1280, 720 }, {1280, 600 }, {1152, 864 }, {1024, 768 },
  {800 , 600 }, {640 , 480 }
};

static const DWORD DefaultPreferredDisplayMode = 19;
