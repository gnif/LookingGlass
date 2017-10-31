/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <stdio.h>

#ifdef DEBUG
  #define DEBUG_PRINT(type, fmt, args...) do {fprintf(stderr, type " %20s:%-5u | %-24s | " fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args);} while (0)
#else
  #define DEBUG_PRINT(type, fmt, args...) do {} while(0)
#endif

#define DEBUG_INFO(fmt, args...) DEBUG_PRINT("[I]", fmt, ##args)
#define DEBUG_WARN(fmt, args...) DEBUG_PRINT("[W]", fmt, ##args)
#define DEBUG_ERROR(fmt, args...) DEBUG_PRINT("[E]", fmt, ##args)
#define DEBUG_FIXME(fmt, args...) DEBUG_PRINT("[F]", fmt, ##args)

#if defined(DEBUG_SPICE) | defined(DEBUG_IVSHMEM)
  #define DEBUG_PROTO(fmt, args...) DEBUG_PRINT("[P]", fmt, ##args)
#else
  #define DEBUG_PROTO(fmt, args...) do {} while(0)
#endif