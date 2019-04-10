/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#include <stdbool.h>
#include <NvFBC/nvFBC.h>

#ifndef __cplusplus
typedef void * NvFBCToSys;
#else
#include <NvFBC/nvFBCToSys.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/capture.h"

enum BufferFormat
{
  BUFFER_FMT_ARGB,
  BUFFER_FMT_RGB,
  BUFFER_FMT_YYYYUV420p,
  BUFFER_FMT_RGB_PLANAR,
  BUFFER_FMT_XOR,
  BUFFER_FMT_YUV444p,
  BUFFER_FMT_ARGB10
};

enum DiffMapBlockSize
{
  DIFFMAP_BLOCKSIZE_128X128 = 0,
  DIFFMAP_BLOCKSIZE_16X16,
  DIFFMAP_BLOCKSIZE_32X32,
  DIFFMAP_BLOCKSIZE_64X64
};

bool NvFBCInit();
void NvFBCFree();

bool NvFBCToSysCreate(void * privData, unsigned int privDataSize, NvFBCToSys ** nvfbc);
void NvFBCToSysRelease(NvFBCToSys ** nvfbc);

bool NvFBCToSysSetup(
  NvFBCToSys          * nvfbc,
  enum                  BufferFormat format,
  bool                  hwCursor,
  bool                  useDiffMap,
  enum DiffMapBlockSize diffMapBlockSize,
  void **               frameBuffer,
  void **               diffMap
);

CaptureResult NvFBCToSysCapture(
  NvFBCToSys * nvfbc,
  const unsigned int waitTime,
  const unsigned int x,
  const unsigned int y,
  const unsigned int width,
  const unsigned int height,
  NvFBCFrameGrabInfo * grabInfo
);

#ifdef __cplusplus
}
#endif