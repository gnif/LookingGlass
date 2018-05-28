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

struct CursorInfo
{
  bool            updated;

  bool            visible;
  bool            hasShape;
  bool            hasPos;
  int             x, y;

  enum CursorType type;
  unsigned int    w, h;
  unsigned int    pitch;
  void          * shape;
  unsigned int    dataSize;
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
  GRAB_STATUS_OK,
  GRAB_STATUS_REINIT,
  GRAB_STATUS_CURSOR,
  GRAB_STATUS_ERROR
};

typedef std::vector<const char *> CaptureOptions;

class ICapture
{
public:
  virtual const char * GetName() = 0;

  virtual bool Initialize(CaptureOptions * options) = 0;
  virtual void DeInitialize() = 0;
  virtual bool ReInitialize() = 0;
  virtual enum FrameType GetFrameType() = 0;
  virtual size_t GetMaxFrameSize() = 0;
  virtual enum GrabStatus GrabFrame(struct FrameInfo & frame, struct CursorInfo & cursor) = 0;
};
