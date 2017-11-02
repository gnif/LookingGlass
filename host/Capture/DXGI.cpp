/*
KVMGFX Client - A KVM Client for VGA Passthrough
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

#include "DXGI.h"
using namespace Capture;

DXGI::DXGI() :
  m_initialized(false)
{

}

DXGI::~DXGI()
{

}

bool DXGI::Initialize()
{
  if (m_initialized)
    DeInitialize();

  m_initialized = true;
  return true;
}

void DXGI::DeInitialize()
{
  m_initialized = false;
}

FrameType DXGI::GetFrameType()
{
  if (!m_initialized)
    return FRAME_TYPE_INVALID;

  return FrameType();
}

FrameComp DXGI::GetFrameCompression()
{
  if (!m_initialized)
    return FRAME_COMP_NONE;

  return FrameComp();
}

size_t DXGI::GetMaxFrameSize()
{
  return size_t();
}

bool DXGI::GrabFrame(FrameInfo & frame)
{
  return false;
}