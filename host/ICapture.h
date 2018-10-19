/*
Looking Glass - KVM FrameRelay (KVMFR) Client
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

#pragma once

#include "common/KVMFR.h"
#include <vector>
#include <windows.h>

struct CursorBuffer
{
  unsigned int bufferSize;
  char       * buffer;
  unsigned int pointerSize;
};

struct CursorInfo
{
  bool            visible;
  bool            hasPos;
  bool            hasShape;
  int             x, y;

  enum CursorType type;
  unsigned int    w, h;
  unsigned int    pitch;
  CursorBuffer    shape;
};

struct FrameInfo
{
  unsigned int width;
  unsigned int height;
  unsigned int stride;
  unsigned int pitch;
  void * buffer;
  size_t bufferSize;
};

enum GrabStatus
{
  GRAB_STATUS_OK      = 1,
  GRAB_STATUS_TIMEOUT = 2,
  GRAB_STATUS_REINIT  = 4,
  GRAB_STATUS_CURSOR  = 8,
  GRAB_STATUS_FRAME   = 16,
  GRAB_STATUS_ERROR   = 32
};

typedef std::vector<const char *> CaptureOptions;

class ICapture
{
public:
  virtual const char * GetName() = 0;

  virtual bool CanInitialize() = 0;
  virtual bool Initialize(CaptureOptions * options) = 0;
  virtual void DeInitialize() = 0;
  virtual bool ReInitialize() = 0;
  virtual enum FrameType GetFrameType() = 0;
  virtual size_t GetMaxFrameSize() = 0;
  virtual unsigned int Capture() = 0;
  virtual enum GrabStatus GetFrame(struct FrameInfo & frame) = 0;
  virtual bool GetCursor(CursorInfo & cursor) = 0;
  virtual void FreeCursor() = 0;
  virtual enum GrabStatus DiscardFrame() = 0;
};
