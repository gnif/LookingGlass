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

#include "ICapture.h"

#define W32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <atlbase.h>

namespace Capture
{
  class DXGI : public ICapture
  {
  public:
    DXGI();
    ~DXGI();

    const char * GetName() { return "DXGI"; }

    bool Initialize(CaptureOptions * options);
    void DeInitialize();
    bool ReInitialize()
    {
      DeInitialize();
      /*
      DXGI needs some time when mode switches occur, failing to do so causes
      failure to start and exceptions internal to DXGI
      */
      Sleep(200);
      return Initialize(m_options);
    }

    enum FrameType GetFrameType();
    enum FrameComp GetFrameCompression();
    size_t GetMaxFrameSize();
    enum GrabStatus GrabFrame(struct FrameInfo & frame);

  private:
    CaptureOptions * m_options;

    bool          m_initialized;
    unsigned int  m_width;
    unsigned int  m_height;

    CComPtr<IDXGIFactory1>          m_dxgiFactory;
    CComPtr<ID3D11Device>           m_device;
    D3D_FEATURE_LEVEL               m_featureLevel;
    CComPtr<ID3D11DeviceContext>    m_deviceContext;
    CComQIPtr<IDXGIOutput1>         m_output;
    CComPtr<IDXGIOutputDuplication> m_dup;
    CComPtr<ID3D11Texture2D>        m_texture;
    BYTE *                          m_pointer;
    UINT                            m_pointerBufSize;
    UINT                            m_pointerSize;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO m_shapeInfo;
    BOOL                            m_pointerVisible;
    POINT                           m_pointerPos;
  };
};