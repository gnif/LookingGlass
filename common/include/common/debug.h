/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#ifndef _H_LG_COMMON_DEBUG_
#define _H_LG_COMMON_DEBUG_

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include "time.h"

enum DebugLevel
{
  DEBUG_LEVEL_NONE,
  DEBUG_LEVEL_INFO,
  DEBUG_LEVEL_WARN,
  DEBUG_LEVEL_ERROR,
  DEBUG_LEVEL_FIXME,
  DEBUG_LEVEL_FATAL
};

extern const char ** debug_lookup;

void debug_init(void);

#ifdef ENABLE_BACKTRACE
void printBacktrace(void);
#define DEBUG_PRINT_BACKTRACE() printBacktrace()
#else
#define DEBUG_PRINT_BACKTRACE()
#endif

#if defined(_WIN32) && !defined(__GNUC__)
  #define DIRECTORY_SEPARATOR '\\'
#else
  #define DIRECTORY_SEPARATOR '/'
#endif

#ifdef __GNUC__
  #define DEBUG_UNREACHABLE_MARKER() __builtin_unreachable()
#elif defined(_MSC_VER)
  #define DEBUG_UNREACHABLE_MARKER() __assume(0)
#else
  #define DEBUG_UNREACHABLE_MARKER()
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

#define DEBUG_PRINT(level, fmt, ...) do { \
  fprintf(stderr, "%s%12" PRId64 "%20s:%-4u | %-30s | " fmt "%s\n", \
      debug_lookup[level], microtime(), STRIPPATH(__FILE__), \
      __LINE__, __FUNCTION__, ##__VA_ARGS__, debug_lookup[DEBUG_LEVEL_NONE]); \
} while (0)

#define DEBUG_BREAK() DEBUG_PRINT(DEBUG_LEVEL_INFO, "================================================================================")
#define DEBUG_INFO(fmt, ...) DEBUG_PRINT(DEBUG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define DEBUG_WARN(fmt, ...) DEBUG_PRINT(DEBUG_LEVEL_WARN, fmt, ##__VA_ARGS__)
#define DEBUG_ERROR(fmt, ...) DEBUG_PRINT(DEBUG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define DEBUG_FIXME(fmt, ...) DEBUG_PRINT(DEBUG_LEVEL_FIXME, fmt, ##__VA_ARGS__)
#define DEBUG_FATAL(fmt, ...) do { \
  DEBUG_BREAK(); \
  DEBUG_PRINT(DEBUG_LEVEL_FATAL, fmt, ##__VA_ARGS__); \
  DEBUG_PRINT_BACKTRACE(); \
  abort(); \
  DEBUG_UNREACHABLE_MARKER(); \
} while(0)

#define DEBUG_ASSERT_PRINT(...) DEBUG_ERROR("Assertion failed: %s", #__VA_ARGS__)

#ifdef NDEBUG
  #define DEBUG_ASSERT(...) do { \
    if (!(__VA_ARGS__)) \
      DEBUG_ASSERT_PRINT(__VA_ARGS__); \
  } while (0)
#else
  #define DEBUG_ASSERT(...) do { \
    if (!(__VA_ARGS__)) \
    { \
      DEBUG_ASSERT_PRINT(__VA_ARGS__); \
      abort(); \
    } \
  } while (0)
#endif

#define DEBUG_UNREACHABLE() DEBUG_FATAL("Unreachable code reached")

#if defined(DEBUG_SPICE) | defined(DEBUG_IVSHMEM)
  #define DEBUG_PROTO(fmt, args...) DEBUG_PRINT("[P]", fmt, ##args)
#else
  #define DEBUG_PROTO(fmt, ...) do {} while(0)
#endif

void debug_info(const char * file, unsigned int line, const char * function,
    const char * format, ...) __attribute__((format (printf, 4, 5)));

void debug_warn(const char * file, unsigned int line, const char * function,
    const char * format, ...) __attribute__((format (printf, 4, 5)));

void debug_error(const char * file, unsigned int line, const char * function,
    const char * format, ...) __attribute__((format (printf, 4, 5)));

#endif
