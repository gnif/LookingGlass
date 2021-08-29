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

#include "common/appstrings.h"

const char * LG_COPYRIGHT_STR =
  "Copyright © 2017-2021 The Looking Glass Authors";

const char * LG_WEBSITE_STR =
  "https://looking-glass.io";

const char * LG_LICENSE_STR =
  "This program is free software; you can redistribute it and/or modify it "
  "under the terms of the GNU General Public License as published by the Free "
  "Software Foundation; either version 2 of the License, or (at your option) "
  "any later version.\n"
  "\n"
  "This program is distributed in the hope that it will be useful, but WITHOUT "
  "ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or "
  "FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for "
  "more details.\n"
  "\n"
  "You should have received a copy of the GNU General Public License along "
  "with this program; if not, write to the Free Software Foundation, Inc., 59 "
  "Temple Place, Suite 330, Boston, MA 02111-1307 USA";

const StringPair LG_HELP_LINKS[] =
{
  {
    .name = "Documentation",
    .value = "https://looking-glass.io/docs"
  },
  {
    .name  = "Wiki",
    .value = "https://looking-glass.io/wiki"
  },
  {
    .name  = "Discord",
    .value = "https://looking-glass.io/discord"
  },
  {
    .name  = "Level1Techs",
    .value = "https://level1techs.com/lg"
  },
  { 0 }
};

const struct LGTeamMember LG_TEAM[] =
{
  {
    .name  = "Geoffrey McRae (gnif)",
    .blurb =
      "Project lead and core developer of this program, from its initial "
      "creation through to what it is today. His roles include development "
      "and direction of the project as a whole, focusing on the X11 "
      "platform as well as maintaining the Looking Glass community on "
      "Discord and the Level1Tech forums.",
    .donate =
    {
      {
        .name  = "GitHub Sponsors",
        .value = "https://github.com/sponsors/gnif"
      },
      {
        .name  = "Patreon",
        .value = "https://www.patreon.com/gnif"
      },
      {
        .name  = "Ko-Fi",
        .value = "https://ko-fi.com/lookingglass"
      },
      {
        .name  = "Bitcoin (BTC)",
        .value = "14ZFcYjsKPiVreHqcaekvHGL846u3ZuT13"
      },
      {
        .name  = "Ethereum (ETH)",
        .value = "0x6f8aEe454384122bF9ed28f025FBCe2Bce98db85"
      },
      { 0 }
    }
  },
  {
    .name   = "Guanzhong Chen (quantum)",
    .blurb  =
      "Major code contributor to Looking Glass, with a specific focus on "
      "improving Wayland support and the Windows side of things. He works "
      "on many things, from small cosmetic issues to major features. "
      "He implemented much of the Wayland backend, VM->Host DMABUF import, "
      "and damage tracking.",
    .donate = { { 0 } }
  },
  {
    .name   = "Tudor Brindus (Xyene)",
    .blurb  =
      "Wayland developer. Thinks a lot about latency, and occasionally turns "
      "those thoughts into code.",
    .donate = { { 0 } }
  },
  {
    .name   = "Jonathan Rubenstein (JJRcop)",
    .blurb  = "Documentation Guru and Discord Community Manager",
    .donate = { { 0 } }
  },
  { 0 }
};
