/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>

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

struct FrameInfo
{
  unsigned int width;
  unsigned int height;
  unsigned int stride;
  void * buffer;
  size_t bufferSize;
  size_t outSize;

  bool hasMousePos;
  int mouseX, mouseY;
};

enum GrabStatus
{
  GRAB_STATUS_OK,
  GRAB_STATUS_REINIT,
  GRAB_STATUS_ERROR
};

typedef std::vector<const char *> CaptureOptions;

__interface ICapture
{
public:
  const char * GetName();
  
  bool Initialize(CaptureOptions * options);
  void DeInitialize();
  bool ReInitialize();
  enum FrameType GetFrameType();
  size_t GetMaxFrameSize();
  enum GrabStatus GrabFrame(struct FrameInfo & frame);
};