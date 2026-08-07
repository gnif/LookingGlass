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

#include "interface/transport.h"

#include "common/array.h"
#include "common/debug.h"
#include "common/option.h"
#include "common/time.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_BUFFER_COUNT 2
#define TEST_PQ_LUT_SIZE  4096

enum TestDamageMode
{
  TEST_DAMAGE_MOVING,
  TEST_DAMAGE_FULL,
  TEST_DAMAGE_OVERLAP,
  TEST_DAMAGE_MAX,
  TEST_DAMAGE_INVALID,
  TEST_DAMAGE_NULL,
  TEST_DAMAGE_ZERO,
};

struct TestFormat
{
  const char * name;
  FrameType    type;
  unsigned     bytesPerPixel;
  bool         hdr;
  bool         hdrPQ;
};

static const struct TestFormat testFormats[] =
{
  { "bgra"   , FRAME_TYPE_BGRA   , 4, false, false },
  { "rgba"   , FRAME_TYPE_RGBA   , 4, false, false },
  { "bgr32"  , FRAME_TYPE_BGR_32 , 4, false, false },
  { "rgb24"  , FRAME_TYPE_RGB_24 , 3, false, false },
  { "rgba10" , FRAME_TYPE_RGBA10 , 4, true , true  },
  { "rgba16f", FRAME_TYPE_RGBA16F, 8, true , false },
};

struct TestBuffer
{
  FrameBuffer * framebuffer;
};

struct LG_Transport
{
  unsigned                width;
  unsigned                height;
  unsigned                stride;
  unsigned                frameRate;
  unsigned                frameCount;
  bool                    realtime;
  bool                    cycleFormats;
  bool                    holdLastFrame;
  enum TestDamageMode     damageMode;
  unsigned                formatIndex;

  bool                    connected;
  bool                    framePending;
  uint64_t                serial;
  uint64_t                nextFrameTime;
  unsigned                bufferIndex;
  struct TestBuffer       buffers[TEST_BUFFER_COUNT];
  FrameDamageRect         damage[LG_TRANSPORT_MAX_DAMAGE_RECTS];
  LG_TransportFrameFormat format;
  uint16_t                pqLUT[TEST_PQ_LUT_SIZE + 1];
};

static void test_setup(void)
{
  static struct Option options[] =
  {
    {
      .module      = "test",
      .name        = "width",
      .description = "Generated frame width",
      .type        = OPTION_TYPE_INT,
      .value.x_int = 1920,
    },
    {
      .module      = "test",
      .name        = "height",
      .description = "Generated frame height",
      .type        = OPTION_TYPE_INT,
      .value.x_int = 1080,
    },
    {
      .module      = "test",
      .name        = "stride",
      .description =
        "Generated frame stride in storage elements, or zero for packed",
      .type        = OPTION_TYPE_INT,
      .value.x_int = 0,
    },
    {
      .module      = "test",
      .name        = "frameRate",
      .description = "Generated frames per second",
      .type        = OPTION_TYPE_INT,
      .value.x_int = 60,
    },
    {
      .module      = "test",
      .name        = "frameCount",
      .description = "Stop after this many frames, or zero to run forever",
      .type        = OPTION_TYPE_INT,
      .value.x_int = 0,
    },
    {
      .module       = "test",
      .name         = "realtime",
      .description  = "Pace generated frames in real time",
      .type         = OPTION_TYPE_BOOL,
      .value.x_bool = true,
    },
    {
      .module         = "test",
      .name           = "format",
      .description    = "Generated frame format: bgra, rgba, bgr32, rgb24, "
                        "rgba10, rgba16f, or cycle",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "bgra",
    },
    {
      .module         = "test",
      .name           = "damage",
      .description    = "Damage mode: moving, full, overlap, max, invalid, "
                        "null, or zero",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "moving",
    },
    {
      .module       = "test",
      .name         = "holdLastFrame",
      .description  =
        "Keep the session alive after generating frameCount frames",
      .type         = OPTION_TYPE_BOOL,
      .value.x_bool = false,
    },
    {
      .module         = "test",
      .name           = "captureFile",
      .description    = "Write a composed framebuffer capture to this file",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = NULL,
    },
    {
      .module      = "test",
      .name        = "captureFrame",
      .description = "Capture after this synthetic frame serial is submitted",
      .type        = OPTION_TYPE_INT,
      .value.x_int = 0,
    },
    {
      .module      = "test",
      .name        = "captureDelay",
      .description = "Stable render passes to wait before framebuffer capture",
      .type        = OPTION_TYPE_INT,
      .value.x_int = 4,
    },
    {0}
  };

  option_register(options);
}

static int test_findFormat(const char * name)
{
  for (unsigned i = 0; i < ARRAY_LENGTH(testFormats); ++i)
    if (strcmp(name, testFormats[i].name) == 0)
      return (int)i;

  return -1;
}

static int test_findDamageMode(const char * name)
{
  static const char * names[] =
  {
    [TEST_DAMAGE_MOVING]  = "moving",
    [TEST_DAMAGE_FULL]    = "full",
    [TEST_DAMAGE_OVERLAP] = "overlap",
    [TEST_DAMAGE_MAX]     = "max",
    [TEST_DAMAGE_INVALID] = "invalid",
    [TEST_DAMAGE_NULL]    = "null",
    [TEST_DAMAGE_ZERO]    = "zero",
  };

  for (unsigned i = 0; i < ARRAY_LENGTH(names); ++i)
    if (strcmp(name, names[i]) == 0)
      return (int)i;
  return -1;
}

static float test_linearToPQ(float linear)
{
  const float m1 = 2610.0f / 16384.0f;
  const float m2 = 2523.0f / 32.0f;
  const float c1 = 3424.0f / 4096.0f;
  const float c2 = 2413.0f / 128.0f;
  const float c3 = 2392.0f / 128.0f;
  const float p  = powf(linear, m1);
  return powf((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

static void test_initPQLUT(struct LG_Transport * this)
{
  for (unsigned i = 0; i <= TEST_PQ_LUT_SIZE; ++i)
  {
    /* scRGB uses 1.0 for 80 nits; PQ uses 1.0 for 10000 nits. */
    const float scRGB = (float)i * 4.0f / TEST_PQ_LUT_SIZE;
    const float pq    = test_linearToPQ(scRGB / 125.0f);
    this->pqLUT[i] = (uint16_t)(pq * 1023.0f + 0.5f);
  }
}

static uint16_t test_scRGBToPQ(const struct LG_Transport * this, float scRGB)
{
  const unsigned index = scRGB <= 0.0f ? 0 : scRGB >= 4.0f ?
    TEST_PQ_LUT_SIZE :
    (unsigned)(scRGB * (TEST_PQ_LUT_SIZE / 4.0f) + 0.5f);
  return this->pqLUT[index];
}

static void test_setHDRMetadata(LG_TransportFrameFormat * format)
{
  if (format->hdrPQ)
  {
    format->hdrDisplayPrimary[0][0] = 35400;
    format->hdrDisplayPrimary[0][1] = 14600;
    format->hdrDisplayPrimary[1][0] =  8500;
    format->hdrDisplayPrimary[1][1] = 39850;
    format->hdrDisplayPrimary[2][0] =  6550;
    format->hdrDisplayPrimary[2][1] =  2300;
    format->hdrMaxDisplayLuminance  = 1000;
    format->hdrMinDisplayLuminance  = 1;
  }
  else
  {
    format->hdrDisplayPrimary[0][0] = 13250;
    format->hdrDisplayPrimary[0][1] = 34500;
    format->hdrDisplayPrimary[1][0] =  7500;
    format->hdrDisplayPrimary[1][1] = 30000;
    format->hdrDisplayPrimary[2][0] = 34000;
    format->hdrDisplayPrimary[2][1] = 16000;
    format->hdrMaxDisplayLuminance  = 80;
    format->hdrMinDisplayLuminance  = 50;
  }

  format->hdrWhitePoint[0]             = 15635;
  format->hdrWhitePoint[1]             = 16450;
  format->hdrMaxContentLightLevel      = 1000;
  format->hdrMaxFrameAverageLightLevel = 400;
}

static bool test_setFormat(struct LG_Transport * this, unsigned index)
{
  if (this->format.version && this->formatIndex == index)
    return false;

  const struct TestFormat * testFormat  = &testFormats[index];
  const uint32_t            version     = this->format.version + 1;
  const bool                packedBGR   =
    testFormat->type == FRAME_TYPE_BGR_32;
  const unsigned            packedPitch = packedBGR ?
    ALIGN_PAD(this->width * 3, 4) :
    this->width * testFormat->bytesPerPixel;
  const unsigned            stride      = this->stride ? this->stride :
    packedBGR ? packedPitch / 4 : this->width;
  const unsigned            pitch       = this->stride ?
    stride * testFormat->bytesPerPixel :
    packedPitch;

  this->formatIndex = index;
  this->format      = (LG_TransportFrameFormat) {
    .version       = version,
    .type          = testFormat->type,
    .screenWidth   = this->width,
    .screenHeight  = this->height,
    .dataWidth     = packedBGR ? pitch / 4 : this->width,
    .dataHeight    = this->height,
    .frameWidth    = this->width,
    .frameHeight   = this->height,
    .rotation      = FRAME_ROT_0,
    .stride        = stride,
    .pitch         = pitch,
    .hdr           = testFormat->hdr,
    .hdrPQ         = testFormat->hdrPQ,
    .hdrMetadata   = testFormat->hdr,
    .sdrWhiteLevel = LG_SDR_WHITE_LEVEL_DEFAULT,
  };

  if (this->format.hdrMetadata)
    test_setHDRMetadata(&this->format);

  return true;
}

static bool test_create(LG_Transport ** result)
{
  struct LG_Transport * this = calloc(1, sizeof(*this));
  if (!this)
    return false;

  const int      width        = option_get_int("test", "width");
  const int      height       = option_get_int("test", "height");
  const int      stride       = option_get_int("test", "stride");
  const int      frameRate    = option_get_int("test", "frameRate");
  const int      frameCount   = option_get_int("test", "frameCount");
  const char *   format       = option_get_string("test", "format");
  const char *   damage       = option_get_string("test", "damage");
  const int      damageMode   = damage ? test_findDamageMode(damage) : -1;
  const bool     cycleFormats = strcmp(format, "cycle") == 0;
  const int      formatIndex  = cycleFormats ? 0 : test_findFormat(format);
  const unsigned maxBytesPerPixel  = formatIndex < 0 ? 0 :
    cycleFormats ? 8 : testFormats[formatIndex].bytesPerPixel;
  const size_t   bufferStride      = stride > 0 ? (size_t)stride :
    width > 0 ? (size_t)width : 0;
  if (width < 1 || height < 1 || !maxBytesPerPixel || damageMode < 0 ||
      stride < 0 || (stride && stride < width) ||
      bufferStride > UINT32_MAX / maxBytesPerPixel ||
      frameRate < 1 || frameRate > 1000000000 || frameCount < 0 ||
      bufferStride > (SIZE_MAX - sizeof(FrameBuffer)) /
        maxBytesPerPixel / (size_t)height)
  {
    DEBUG_ERROR(
        "Invalid test transport dimensions, rate, frame count, or format");
    free(this);
    return false;
  }

  this->width        = width;
  this->height       = height;
  this->stride       = stride;
  this->frameRate    = frameRate;
  this->frameCount   = frameCount;
  this->realtime     = option_get_bool("test", "realtime");
  this->cycleFormats = cycleFormats;
  this->holdLastFrame = option_get_bool("test", "holdLastFrame");
  this->damageMode    = damageMode;
  this->formatIndex  = (unsigned)formatIndex;
  test_initPQLUT(this);

  const size_t dataSize =
    bufferStride * this->height * maxBytesPerPixel;
  for (unsigned i = 0; i < TEST_BUFFER_COUNT; ++i)
  {
    this->buffers[i].framebuffer = malloc(sizeof(FrameBuffer) + dataSize);
    if (!this->buffers[i].framebuffer)
    {
      for (unsigned j = 0; j < i; ++j)
        free(this->buffers[j].framebuffer);
      free(this);
      return false;
    }
  }

  *result = this;
  return true;
}

static void test_destroy(LG_Transport ** transport)
{
  if (!transport || !*transport)
    return;

  struct LG_Transport * this = *transport;
  for (unsigned i = 0; i < TEST_BUFFER_COUNT; ++i)
    free(this->buffers[i].framebuffer);
  free(this);
  *transport = NULL;
}

static LG_TransportStatus test_connect(LG_Transport * this,
    LG_TransportSession * session)
{
  memset(session, 0, sizeof(*session));
  memcpy(session->version, "test", 5);
  session->os = LG_TRANSPORT_OS_OTHER;
  memcpy(session->capture, "test", 5);

  this->connected      = true;
  this->framePending   = false;
  this->serial         = 0;
  this->bufferIndex    = 0;
  this->nextFrameTime  = nanotime();
  this->format.version = 0;
  test_setFormat(this, this->formatIndex);
  return LG_TRANSPORT_OK;
}

static void test_disconnect(LG_Transport * this)
{
  this->connected    = false;
  this->framePending = false;
}

static bool test_sessionValid(LG_Transport * this)
{
  return this->connected;
}

static bool test_supportsDMA(LG_Transport * this)
{
  return false;
}

static bool test_attachRenderer(LG_Transport * this,
    const LG_RendererInterop * interop)
{
  return true;
}

static void test_detachRenderer(LG_Transport * this)
{
}

static uint16_t test_floatToHalf(float value)
{
  union
  {
    float    value;
    uint32_t bits;
  }
  input = { .value = value };

  const uint16_t sign     = (input.bits >> 16) & 0x8000;
  int            exponent = ((input.bits >> 23) & 0xff) - 127 + 15;
  uint32_t       mantissa = input.bits & 0x7fffff;
  if (exponent <= 0)
    return sign;
  if (exponent >= 31)
    return sign | 0x7c00;

  mantissa += 0x1000;
  if (mantissa & 0x800000)
  {
    mantissa = 0;
    if (++exponent >= 31)
      return sign | 0x7c00;
  }

  return sign | ((uint16_t)exponent << 10) | (mantissa >> 13);
}

static void test_getColor(const struct LG_Transport * this,
    unsigned x, unsigned y, uint8_t * r, uint8_t * g, uint8_t * b)
{
  const unsigned widthRange  = this->width  > 1 ? this->width  - 1 : 1;
  const unsigned heightRange = this->height > 1 ? this->height - 1 : 1;
  *r = (uint64_t)x * 255 / widthRange;
  *g = (uint64_t)y * 255 / heightRange;
  *b = ((x / 32) ^ (y / 32)) & 1 ? 0x30 : 0x90;

  unsigned boxSize = this->width < this->height ?
    this->width : this->height;
  if (boxSize > 64)
    boxSize = 64;
  const unsigned rangeX = this->width > boxSize ? this->width - boxSize : 1;
  const unsigned rangeY = this->height > boxSize ?
    this->height - boxSize : 1;
  const unsigned boxX   = (this->serial * 7) % rangeX;
  const unsigned boxY   = (this->serial * 5) % rangeY;
  if (x < boxX || y < boxY || x >= boxX + boxSize || y >= boxY + boxSize)
    return;

  const uint32_t color = this->serial * 2654435761U;
  *r = color >> 16;
  *g = color >> 8;
  *b = color;
}

static void test_generateFrame(struct LG_Transport * this, FrameBuffer * fb)
{
  uint8_t * data = framebuffer_get_data(fb);
  if (this->stride)
    memset(data, 0xa5 ^ (uint8_t)this->serial,
      (size_t)this->format.pitch * this->height);
  else if (this->format.type == FRAME_TYPE_BGR_32)
    memset(data, 0, (size_t)this->format.pitch * this->height);

  for (unsigned y = 0; y < this->height; ++y)
  {
    uint8_t * row = data + (size_t)y * this->format.pitch;
    for (unsigned x = 0; x < this->width; ++x)
    {
      uint8_t r, g, b;
      test_getColor(this, x, y, &r, &g, &b);
      switch (this->format.type)
      {
        case FRAME_TYPE_BGRA:
          ((uint32_t *)row)[x] =
            0xff000000U | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
          break;

        case FRAME_TYPE_BGR_32:
        {
          uint8_t * bgr = row + x * 3;
          bgr[0] = b;
          bgr[1] = g;
          bgr[2] = r;
          break;
        }

        case FRAME_TYPE_RGBA:
          ((uint32_t *)row)[x] =
            0xff000000U | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
          break;

        case FRAME_TYPE_RGB_24:
          row[x * 3    ] = r;
          row[x * 3 + 1] = g;
          row[x * 3 + 2] = b;
          break;

        case FRAME_TYPE_RGBA10:
        {
          const float r709 = (float)r * 4.0f / 255.0f;
          const float g709 = (float)g * 4.0f / 255.0f;
          const float b709 = (float)b * 4.0f / 255.0f;
          const float r2020 =
            r709 * 0.6274039f + g709 * 0.3292830f + b709 * 0.0433131f;
          const float g2020 =
            r709 * 0.0690973f + g709 * 0.9195404f + b709 * 0.0113623f;
          const float b2020 =
            r709 * 0.0163914f + g709 * 0.0880133f + b709 * 0.8955953f;
          ((uint32_t *)row)[x] =
            ((uint32_t)test_scRGBToPQ(this, r2020)      ) |
            ((uint32_t)test_scRGBToPQ(this, g2020) << 10) |
            ((uint32_t)test_scRGBToPQ(this, b2020) << 20) |
            (3U << 30);
          break;
        }

        case FRAME_TYPE_RGBA16F:
        {
          uint16_t * rgba = (uint16_t *)row + x * 4;
          rgba[0] = test_floatToHalf((float)r * 4.0f / 255.0f);
          rgba[1] = test_floatToHalf((float)g * 4.0f / 255.0f);
          rgba[2] = test_floatToHalf((float)b * 4.0f / 255.0f);
          rgba[3] = test_floatToHalf(1.0f);
          break;
        }

        default:
          DEBUG_UNREACHABLE();
      }
    }
  }

  framebuffer_set_write_ptr(fb, (size_t)this->format.pitch * this->height);
}

static LG_TransportStatus test_nextFrame(LG_Transport * this, bool useDMA,
    LG_TransportFrame * frame)
{
  if (!this->connected)
    return LG_TRANSPORT_DISCONNECTED;
  if (this->framePending)
    return LG_TRANSPORT_ERROR;
  if (this->frameCount && this->serial >= this->frameCount)
  {
    if (!this->holdLastFrame)
      return LG_TRANSPORT_END;
    usleep(1000);
    return LG_TRANSPORT_TIMEOUT;
  }

  if (this->realtime)
  {
    const uint64_t now = nanotime();
    if (now < this->nextFrameTime)
    {
      const uint64_t remaining = this->nextFrameTime - now;
      usleep((remaining > 1000000 ? 1000000 : remaining) / 1000);
      return LG_TRANSPORT_TIMEOUT;
    }
  }

  const bool formatChanged = this->cycleFormats && test_setFormat(this,
      this->serial % ARRAY_LENGTH(testFormats));
  ++this->serial;
  struct TestBuffer * buffer = &this->buffers[this->bufferIndex];
  this->bufferIndex = (this->bufferIndex + 1) % TEST_BUFFER_COUNT;
  framebuffer_prepare(buffer->framebuffer);
  test_generateFrame(this, buffer->framebuffer);

  memset(frame, 0, sizeof(*frame));
  frame->serial    = this->serial;
  frame->timestamp = this->realtime ? nanotime() :
    this->serial * (1000000000ULL / this->frameRate);
  frame->format      = &this->format;
  frame->framebuffer = buffer->framebuffer;
  frame->dmaFD       = -1;

  if (this->damageMode == TEST_DAMAGE_FULL ||
      this->serial == 1 || formatChanged)
    frame->damageRectsCount = 0;
  else
  {
    unsigned boxSize = this->width < this->height ?
      this->width : this->height;
    if (boxSize > 64)
      boxSize = 64;
    const unsigned rangeX = this->width > boxSize ?
      this->width - boxSize : 1;
    const unsigned rangeY = this->height > boxSize ?
      this->height - boxSize : 1;
    this->damage[0] = (FrameDamageRect) {
      .x      = ((this->serial - 1) * 7) % rangeX,
      .y      = ((this->serial - 1) * 5) % rangeY,
      .width  = boxSize,
      .height = boxSize,
    };
    this->damage[1] = (FrameDamageRect) {
      .x      = (this->serial * 7) % rangeX,
      .y      = (this->serial * 5) % rangeY,
      .width  = boxSize,
      .height = boxSize,
    };
    frame->damageRects = this->damage;
    switch (this->damageMode)
    {
      case TEST_DAMAGE_MOVING:
        frame->damageRectsCount = 2;
        break;

      case TEST_DAMAGE_OVERLAP:
        this->damage[2] = this->damage[0];
        this->damage[3] = this->damage[1];
        frame->damageRectsCount = 4;
        break;

      case TEST_DAMAGE_MAX:
        for (unsigned i = 2; i < ARRAY_LENGTH(this->damage); ++i)
          this->damage[i] = (FrameDamageRect) {0};
        frame->damageRectsCount = ARRAY_LENGTH(this->damage);
        break;

      case TEST_DAMAGE_INVALID:
        this->damage[0] = (FrameDamageRect) {
          .x = this->width, .y = 0, .width = 1, .height = 1
        };
        frame->damageRectsCount = 1;
        break;

      case TEST_DAMAGE_NULL:
        frame->damageRects      = NULL;
        frame->damageRectsCount = 1;
        break;

      case TEST_DAMAGE_ZERO:
        this->damage[2] = (FrameDamageRect) {0};
        frame->damageRectsCount = 3;
        break;

      case TEST_DAMAGE_FULL:
        DEBUG_UNREACHABLE();
        break;
    }
  }

  this->framePending = true;
  this->nextFrameTime += 1000000000ULL / this->frameRate;
  return LG_TRANSPORT_OK;
}

static void test_releaseFrame(LG_Transport * this, LG_TransportFrame * frame)
{
  this->framePending = false;
  memset(frame, 0, sizeof(*frame));
}

static LG_TransportStatus test_nextPointer(LG_Transport * this,
    LG_TransportPointer * pointer)
{
  if (!this->connected)
    return LG_TRANSPORT_DISCONNECTED;
  usleep(1000);
  return LG_TRANSPORT_TIMEOUT;
}

static void test_releasePointer(LG_Transport * this,
    LG_TransportPointer * pointer)
{
}

static LG_TransportStatus test_sendControl(LG_Transport * this,
    const LG_TransportControl * control, LG_TransportControlToken * token)
{
  if (!this->connected)
    return LG_TRANSPORT_DISCONNECTED;
  return LG_TRANSPORT_UNAVAILABLE;
}

static LG_TransportStatus test_controlStatus(LG_Transport * this,
    LG_TransportControlToken token)
{
  return this->connected ? LG_TRANSPORT_UNAVAILABLE :
    LG_TRANSPORT_DISCONNECTED;
}

const LG_TransportOps LGT_Test =
{
  .name           = "test",
  .setup          = test_setup,
  .create         = test_create,
  .destroy        = test_destroy,
  .connect        = test_connect,
  .disconnect     = test_disconnect,
  .sessionValid   = test_sessionValid,
  .supportsDMA    = test_supportsDMA,
  .attachRenderer = test_attachRenderer,
  .detachRenderer = test_detachRenderer,
  .nextFrame      = test_nextFrame,
  .releaseFrame   = test_releaseFrame,
  .nextPointer    = test_nextPointer,
  .releasePointer = test_releasePointer,
  .sendControl    = test_sendControl,
  .controlStatus  = test_controlStatus,
};
