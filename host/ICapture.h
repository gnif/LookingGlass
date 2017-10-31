#pragma once

#include "common/KVMGFXHeader.h"

struct FrameInfo
{
  unsigned int width;
  unsigned int height;
  unsigned int stride;
  void * buffer;
  size_t bufferSize;
  size_t outSize;
};

__interface ICapture
{
public:
  bool Initialize();
  void DeInitialize();
  enum FrameType GetFrameType();
  enum FrameComp GetFrameCompression();
  size_t GetMaxFrameSize();
  bool GrabFrame(struct FrameInfo & frame);
};