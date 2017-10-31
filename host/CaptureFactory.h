#pragma once

#define W32_LEAN_AND_MEAN
#include <Windows.h>

#include "ICapture.h"

static class CaptureFactory
{
public:
  static ICapture * GetCaptureDevice()
  {
    return NULL;
  }
};