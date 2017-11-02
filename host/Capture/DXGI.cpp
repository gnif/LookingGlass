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

#include "common\debug.h"

DXGI::DXGI() :
  m_initialized(false),
  m_manager(NULL)
{
}

DXGI::~DXGI()
{

}

bool DXGI::Initialize()
{
  if (m_initialized)
    DeInitialize();

  if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
  {
    DEBUG_ERROR("CoInitializeEx failed");
    return false;
  }

  m_manager = new DXGIManager();
  m_manager->SetCaptureSource(CSDesktop);

  RECT rect;
  m_manager->GetOutputRect(rect);
  m_width  = rect.right  - rect.left;
  m_height = rect.bottom - rect.top;

  m_initialized = true;
  return true;
}

void DXGI::DeInitialize()
{
  if (m_manager)
  {
    delete m_manager;
    m_manager = NULL;
  }

  m_initialized = false;
}

FrameType DXGI::GetFrameType()
{
  if (!m_initialized)
    return FRAME_TYPE_INVALID;

  return FRAME_TYPE_ARGB;
}

FrameComp DXGI::GetFrameCompression()
{
  if (!m_initialized)
    return FRAME_COMP_NONE;

  return FRAME_COMP_NONE;
}

size_t DXGI::GetMaxFrameSize()
{
  if (!m_initialized);
    return 0;

  return m_width * m_height * 4;
}

bool DXGI::GrabFrame(FrameInfo & frame)
{
  RECT rect;
  m_manager->GetOutputRect(rect);
  m_width  = rect.right  - rect.left;
  m_height = rect.bottom - rect.top;

  HRESULT result;
  for(int i = 0; i < 2; ++i)
  {
    result = m_manager->GetOutputBits((BYTE*)frame.buffer, rect);
    if (SUCCEEDED(result))
      break;
  }

  if (FAILED(result))
    return false;


  frame.width   = m_width;
  frame.height  = m_height;
  frame.stride  = m_width;
  frame.outSize = m_width * m_height * 4;
  return true;
}