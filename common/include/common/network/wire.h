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

#ifndef _H_LG_COMMON_NETWORK_WIRE_
#define _H_LG_COMMON_NETWORK_WIRE_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum LGNetParseResult
{
  LG_NET_PARSE_OK,
  LG_NET_PARSE_TRUNCATED,
  LG_NET_PARSE_INVALID_MAGIC,
  LG_NET_PARSE_INVALID_VERSION,
  LG_NET_PARSE_INVALID_HEADER,
  LG_NET_PARSE_INVALID_LENGTH,
  LG_NET_PARSE_INVALID_VALUE,
}
LGNetParseResult;

typedef struct LGNetWriter
{
  uint8_t * data;
  size_t    capacity;
  size_t    offset;
  bool      failed;
}
LGNetWriter;

typedef struct LGNetReader
{
  const uint8_t * data;
  size_t          size;
  size_t          offset;
  bool            failed;
}
LGNetReader;

void lgNetWriterInit(LGNetWriter * writer, void * data, size_t capacity);
size_t lgNetWriterSize(const LGNetWriter * writer);
size_t lgNetWriterRemaining(const LGNetWriter * writer);
bool lgNetWriterValid(const LGNetWriter * writer);
bool lgNetWriterU8(LGNetWriter * writer, uint8_t value);
bool lgNetWriterU16(LGNetWriter * writer, uint16_t value);
bool lgNetWriterU32(LGNetWriter * writer, uint32_t value);
bool lgNetWriterU64(LGNetWriter * writer, uint64_t value);
bool lgNetWriterI32(LGNetWriter * writer, int32_t value);
bool lgNetWriterI64(LGNetWriter * writer, int64_t value);
bool lgNetWriterBytes(
  LGNetWriter * writer, const void * data, size_t size);
bool lgNetWriterZero(LGNetWriter * writer, size_t size);

void lgNetReaderInit(
  LGNetReader * reader, const void * data, size_t size);
size_t lgNetReaderConsumed(const LGNetReader * reader);
size_t lgNetReaderRemaining(const LGNetReader * reader);
bool lgNetReaderValid(const LGNetReader * reader);
bool lgNetReaderU8(LGNetReader * reader, uint8_t * value);
bool lgNetReaderU16(LGNetReader * reader, uint16_t * value);
bool lgNetReaderU32(LGNetReader * reader, uint32_t * value);
bool lgNetReaderU64(LGNetReader * reader, uint64_t * value);
bool lgNetReaderI32(LGNetReader * reader, int32_t * value);
bool lgNetReaderI64(LGNetReader * reader, int64_t * value);
bool lgNetReaderBytes(LGNetReader * reader, void * data, size_t size);
bool lgNetReaderView(
  LGNetReader * reader, const uint8_t ** data, size_t size);
bool lgNetReaderSkip(LGNetReader * reader, size_t size);
bool lgNetReaderZero(LGNetReader * reader, size_t size);

#ifdef __cplusplus
}
#endif

#endif
