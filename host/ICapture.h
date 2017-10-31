#pragma once

#include <common/KVMGFXHeader.h>

__interface ICapture
{
public:
  bool Initialize();
  bool DeInitialize();
  enum FrameType GetFrameType();
  enum FrameComp GetFrameCompression();
  size_t GetMaxFrameSize();
  bool GrabFrame(void * buffer, size_t bufferSize, size_t * outLen);
};