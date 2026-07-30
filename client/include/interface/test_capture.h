/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#pragma once

#include <stdint.h>

#define LG_TEST_CAPTURE_MAGIC UINT64_C(0x4c47434150545552)
#define LG_TEST_CAPTURE_VERSION 1

typedef enum LG_TestCaptureFormat
{
  LG_TEST_CAPTURE_RGBA8,
  LG_TEST_CAPTURE_RGB10_A2,
  LG_TEST_CAPTURE_RGBA32F,
}
LG_TestCaptureFormat;

enum
{
  LG_TEST_CAPTURE_HDR        = 0x1,
  LG_TEST_CAPTURE_HDR_PQ     = 0x2,
  LG_TEST_CAPTURE_NATIVE_HDR = 0x4,
  LG_TEST_CAPTURE_BOTTOM_UP  = 0x8,
};

typedef struct LG_TestCaptureHeader
{
  uint64_t magic;
  uint32_t version;
  uint32_t headerSize;
  uint64_t frameSerial;
  uint32_t sourceType;
  uint32_t captureFormat;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t flags;
  uint64_t dataSize;
}
LG_TestCaptureHeader;
