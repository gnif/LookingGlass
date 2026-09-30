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

#include "common/network/wire.h"

#include <string.h>

static bool writerReserve(LGNetWriter * writer, size_t size)
{
  if (!writer || writer->failed || writer->offset > writer->capacity ||
      size > writer->capacity - writer->offset)
  {
    if (writer)
      writer->failed = true;
    return false;
  }

  return true;
}

static bool readerReserve(LGNetReader * reader, size_t size)
{
  if (!reader || reader->failed || reader->offset > reader->size ||
      size > reader->size - reader->offset)
  {
    if (reader)
      reader->failed = true;
    return false;
  }

  return true;
}

void lgNetWriterInit(LGNetWriter * writer, void * data, size_t capacity)
{
  if (!writer)
    return;

  writer->data     = data;
  writer->capacity = capacity;
  writer->offset   = 0;
  writer->failed   = capacity && !data;
}

size_t lgNetWriterSize(const LGNetWriter * writer)
{
  return writer ? writer->offset : 0;
}

size_t lgNetWriterRemaining(const LGNetWriter * writer)
{
  return writer && !writer->failed && writer->offset <= writer->capacity ?
    writer->capacity - writer->offset : 0;
}

bool lgNetWriterValid(const LGNetWriter * writer)
{
  return writer && !writer->failed && writer->offset <= writer->capacity &&
    (!writer->capacity || writer->data);
}

bool lgNetWriterU8(LGNetWriter * writer, uint8_t value)
{
  if (!writerReserve(writer, 1))
    return false;

  writer->data[writer->offset++] = value;
  return true;
}

bool lgNetWriterU16(LGNetWriter * writer, uint16_t value)
{
  if (!writerReserve(writer, 2))
    return false;

  uint8_t * data = writer->data + writer->offset;
  data[0] = (uint8_t)(value      );
  data[1] = (uint8_t)(value >>  8);
  writer->offset += 2;
  return true;
}

bool lgNetWriterU32(LGNetWriter * writer, uint32_t value)
{
  if (!writerReserve(writer, 4))
    return false;

  uint8_t * data = writer->data + writer->offset;
  data[0] = (uint8_t)(value      );
  data[1] = (uint8_t)(value >>  8);
  data[2] = (uint8_t)(value >> 16);
  data[3] = (uint8_t)(value >> 24);
  writer->offset += 4;
  return true;
}

bool lgNetWriterU64(LGNetWriter * writer, uint64_t value)
{
  if (!writerReserve(writer, 8))
    return false;

  uint8_t * data = writer->data + writer->offset;
  data[0] = (uint8_t)(value      );
  data[1] = (uint8_t)(value >>  8);
  data[2] = (uint8_t)(value >> 16);
  data[3] = (uint8_t)(value >> 24);
  data[4] = (uint8_t)(value >> 32);
  data[5] = (uint8_t)(value >> 40);
  data[6] = (uint8_t)(value >> 48);
  data[7] = (uint8_t)(value >> 56);
  writer->offset += 8;
  return true;
}

bool lgNetWriterI32(LGNetWriter * writer, int32_t value)
{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return lgNetWriterU32(writer, bits);
}

bool lgNetWriterI64(LGNetWriter * writer, int64_t value)
{
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return lgNetWriterU64(writer, bits);
}

bool lgNetWriterBytes(
    LGNetWriter * writer, const void * data, size_t size)
{
  if (!writerReserve(writer, size) || (size && !data))
  {
    if (writer)
      writer->failed = true;
    return false;
  }

  if (size)
    memmove(writer->data + writer->offset, data, size);
  writer->offset += size;
  return true;
}

bool lgNetWriterZero(LGNetWriter * writer, size_t size)
{
  if (!writerReserve(writer, size))
    return false;

  if (size)
    memset(writer->data + writer->offset, 0, size);
  writer->offset += size;
  return true;
}

void lgNetReaderInit(
    LGNetReader * reader, const void * data, size_t size)
{
  if (!reader)
    return;

  reader->data   = data;
  reader->size   = size;
  reader->offset = 0;
  reader->failed = size && !data;
}

size_t lgNetReaderConsumed(const LGNetReader * reader)
{
  return reader ? reader->offset : 0;
}

size_t lgNetReaderRemaining(const LGNetReader * reader)
{
  return reader && !reader->failed && reader->offset <= reader->size ?
    reader->size - reader->offset : 0;
}

bool lgNetReaderValid(const LGNetReader * reader)
{
  return reader && !reader->failed && reader->offset <= reader->size &&
    (!reader->size || reader->data);
}

bool lgNetReaderU8(LGNetReader * reader, uint8_t * value)
{
  if (!readerReserve(reader, 1) || !value)
  {
    if (reader)
      reader->failed = true;
    return false;
  }

  *value = reader->data[reader->offset++];
  return true;
}

bool lgNetReaderU16(LGNetReader * reader, uint16_t * value)
{
  if (!readerReserve(reader, 2) || !value)
  {
    if (reader)
      reader->failed = true;
    return false;
  }

  const uint8_t * data = reader->data + reader->offset;
  *value =
    (uint16_t)data[0]       |
    (uint16_t)data[1] << 8;
  reader->offset += 2;
  return true;
}

bool lgNetReaderU32(LGNetReader * reader, uint32_t * value)
{
  if (!readerReserve(reader, 4) || !value)
  {
    if (reader)
      reader->failed = true;
    return false;
  }

  const uint8_t * data = reader->data + reader->offset;
  *value =
    (uint32_t)data[0]        |
    (uint32_t)data[1] <<  8  |
    (uint32_t)data[2] << 16  |
    (uint32_t)data[3] << 24;
  reader->offset += 4;
  return true;
}

bool lgNetReaderU64(LGNetReader * reader, uint64_t * value)
{
  if (!readerReserve(reader, 8) || !value)
  {
    if (reader)
      reader->failed = true;
    return false;
  }

  const uint8_t * data = reader->data + reader->offset;
  *value =
    (uint64_t)data[0]        |
    (uint64_t)data[1] <<  8  |
    (uint64_t)data[2] << 16  |
    (uint64_t)data[3] << 24  |
    (uint64_t)data[4] << 32  |
    (uint64_t)data[5] << 40  |
    (uint64_t)data[6] << 48  |
    (uint64_t)data[7] << 56;
  reader->offset += 8;
  return true;
}

bool lgNetReaderI32(LGNetReader * reader, int32_t * value)
{
  uint32_t bits;
  if (!value || !lgNetReaderU32(reader, &bits))
  {
    if (reader)
      reader->failed = true;
    return false;
  }

  memcpy(value, &bits, sizeof(*value));
  return true;
}

bool lgNetReaderI64(LGNetReader * reader, int64_t * value)
{
  uint64_t bits;
  if (!value || !lgNetReaderU64(reader, &bits))
  {
    if (reader)
      reader->failed = true;
    return false;
  }

  memcpy(value, &bits, sizeof(*value));
  return true;
}

bool lgNetReaderBytes(LGNetReader * reader, void * data, size_t size)
{
  if (!readerReserve(reader, size) || (size && !data))
  {
    if (reader)
      reader->failed = true;
    return false;
  }

  if (size)
    memcpy(data, reader->data + reader->offset, size);
  reader->offset += size;
  return true;
}

bool lgNetReaderView(
    LGNetReader * reader, const uint8_t ** data, size_t size)
{
  if (!readerReserve(reader, size) || !data)
  {
    if (reader)
      reader->failed = true;
    return false;
  }

  *data = size ? reader->data + reader->offset : NULL;
  reader->offset += size;
  return true;
}

bool lgNetReaderSkip(LGNetReader * reader, size_t size)
{
  if (!readerReserve(reader, size))
    return false;

  reader->offset += size;
  return true;
}

bool lgNetReaderZero(LGNetReader * reader, size_t size)
{
  if (!readerReserve(reader, size))
    return false;

  for (size_t i = 0; i < size; ++i)
    if (reader->data[reader->offset + i])
    {
      reader->failed = true;
      return false;
    }

  reader->offset += size;
  return true;
}
