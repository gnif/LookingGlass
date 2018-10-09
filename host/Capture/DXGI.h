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

#include "ICapture.h"
#include "Com.h"
#include "TextureConverter.h"
#include "MFT/H264.h"

#include <list>

#define W32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>

namespace Capture
{
  class DXGI : public ICapture
  {
  public:
    DXGI();
    virtual ~DXGI();

    const char * GetName() { return "DXGI"; }

    bool CanInitialize();
    bool Initialize(CaptureOptions * options);

    void DeInitialize();
    bool ReInitialize()
    {
      DeInitialize();
      /*
      DXGI needs some time when mode switches occur, failing to do so causes
      failure to start and exceptions internal to DXGI
      */
      Sleep(400);
      return Initialize(m_options);
    }

    enum FrameType GetFrameType();
    size_t GetMaxFrameSize();
    unsigned int Capture();
    GrabStatus GetFrame (struct FrameInfo  & frame );
    bool GetCursor(CursorInfo & cursor);
    void FreeCursor(CursorInfo & cursor);
    GrabStatus DiscardFrame();

  private:
    typedef std::list<CursorInfo    > CursorList;
    typedef std::list<CursorBuffer *> CursorBufferPool;

    bool InitRawCapture();
    bool InitYUV420Capture();
    bool InitH264Capture();

    CursorList         m_cursorUpdates;
    CursorBufferPool   m_cursorData;
    CRITICAL_SECTION   m_cursorCS;
    CRITICAL_SECTION   m_cursorDataCS;

    ID3D11Texture2DPtr m_ftexture;

    GrabStatus ReleaseFrame();
    GrabStatus GrabFrameRaw    (struct FrameInfo & frame);
    GrabStatus GrabFrameYUV420 (struct FrameInfo & frame);
    GrabStatus GrabFrameH264   (struct FrameInfo & frame);

    CaptureOptions * m_options;

    bool           m_initialized;
    unsigned int   m_width;
    unsigned int   m_height;
    enum FrameType m_frameType;

    IDXGIFactory1Ptr                m_dxgiFactory;
    ID3D11DevicePtr                 m_device;
    D3D_FEATURE_LEVEL               m_featureLevel;
    ID3D11DeviceContextPtr          m_deviceContext;
    IDXGIOutput1Ptr                 m_output;
    IDXGIOutputDuplicationPtr       m_dup;
    bool                            m_releaseFrame;
    ID3D11Texture2DPtr              m_texture[3];
    TextureConverter              * m_textureConverter;
    MFT::H264                     * m_h264;

    int                             m_lastCursorX, m_lastCursorY;
    BOOL                            m_lastMouseVis;
    POINT                           m_hotSpot;
  };
};