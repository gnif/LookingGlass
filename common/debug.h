/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

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

#if _WIN32
  #define DIRECTORY_SEPARATOR '\\'
#else
  #define DIRECTORY_SEPARATOR '/'
#endif

#define STRIPPATH(s) ( \
  sizeof(s) >  2 && (s)[sizeof(s)- 3] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) -  2 : \
  sizeof(s) >  3 && (s)[sizeof(s)- 4] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) -  3 : \
  sizeof(s) >  4 && (s)[sizeof(s)- 5] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) -  4 : \
  sizeof(s) >  5 && (s)[sizeof(s)- 6] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) -  5 : \
  sizeof(s) >  6 && (s)[sizeof(s)- 7] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) -  6 : \
  sizeof(s) >  7 && (s)[sizeof(s)- 8] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) -  7 : \
  sizeof(s) >  8 && (s)[sizeof(s)- 9] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) -  8 : \
  sizeof(s) >  9 && (s)[sizeof(s)-10] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) -  9 : \
  sizeof(s) > 10 && (s)[sizeof(s)-11] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) - 10 : \
  sizeof(s) > 11 && (s)[sizeof(s)-12] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) - 11 : \
  sizeof(s) > 12 && (s)[sizeof(s)-13] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) - 12 : \
  sizeof(s) > 13 && (s)[sizeof(s)-14] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) - 13 : \
  sizeof(s) > 14 && (s)[sizeof(s)-15] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) - 14 : \
  sizeof(s) > 15 && (s)[sizeof(s)-16] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) - 15 : \
  sizeof(s) > 16 && (s)[sizeof(s)-17] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) - 16 : \
  sizeof(s) > 17 && (s)[sizeof(s)-18] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) - 17 : \
  sizeof(s) > 18 && (s)[sizeof(s)-19] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) - 18 : \
  sizeof(s) > 19 && (s)[sizeof(s)-20] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) - 19 : \
  sizeof(s) > 20 && (s)[sizeof(s)-21] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) - 20 : \
  sizeof(s) > 21 && (s)[sizeof(s)-22] == DIRECTORY_SEPARATOR ? (s) + sizeof(s) - 21 : (s))

#define DEBUG_PRINT(type, fmt, ...) do {fprintf(stderr, type " %20s:%-4u | %-30s | " fmt "\n", STRIPPATH(__FILE__), __LINE__, __FUNCTION__, ##__VA_ARGS__);} while (0)

#define DEBUG_INFO(fmt, ...) DEBUG_PRINT("[I]", fmt, ##__VA_ARGS__)
#define DEBUG_WARN(fmt, ...) DEBUG_PRINT("[W]", fmt, ##__VA_ARGS__)
#define DEBUG_ERROR(fmt, ...) DEBUG_PRINT("[E]", fmt, ##__VA_ARGS__)
#define DEBUG_FIXME(fmt, ...) DEBUG_PRINT("[F]", fmt, ##__VA_ARGS__)

#if defined(DEBUG_SPICE) | defined(DEBUG_IVSHMEM)
  #define DEBUG_PROTO(fmt, args...) DEBUG_PRINT("[P]", fmt, ##args)
#else
  #define DEBUG_PROTO(fmt, ...) do {} while(0)
#endif
