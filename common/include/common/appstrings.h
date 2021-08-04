/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include "common/types.h"

extern const char * LG_COPYRIGHT_STR;
extern const char * LG_WEBSITE_STR;
extern const char * LG_LICENSE_STR;
extern const StringPair LG_HELP_LINKS[];

struct LGTeamMember
{
  const char * name;
  const char * blurb;
  const StringPair donate[10];
};

extern const struct LGTeamMember LG_TEAM[];
