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
#include "MultiMemcpy.h"

#define W32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <mftransform.h>
#include <stdio.h>
#include <comdef.h>

_COM_SMARTPTR_TYPEDEF(IDXGIFactory1         , __uuidof(IDXGIFactory1         ));
_COM_SMARTPTR_TYPEDEF(ID3D11Device          , __uuidof(ID3D11Device          ));
_COM_SMARTPTR_TYPEDEF(ID3D11DeviceContext   , __uuidof(ID3D11DeviceContext   ));
_COM_SMARTPTR_TYPEDEF(ID3D10Multithread     , __uuidof(ID3D10Multithread     ));
_COM_SMARTPTR_TYPEDEF(IDXGIDevice           , __uuidof(IDXGIDevice           ));
_COM_SMARTPTR_TYPEDEF(IDXGIOutput1          , __uuidof(IDXGIOutput1          ));
_COM_SMARTPTR_TYPEDEF(IDXGIOutput           , __uuidof(IDXGIOutput           ));
_COM_SMARTPTR_TYPEDEF(IDXGIAdapter1         , __uuidof(IDXGIAdapter1         ));
_COM_SMARTPTR_TYPEDEF(IDXGIOutputDuplication, __uuidof(IDXGIOutputDuplication));
_COM_SMARTPTR_TYPEDEF(ID3D11Texture2D       , __uuidof(ID3D11Texture2D       ));
_COM_SMARTPTR_TYPEDEF(IDXGIResource         , __uuidof(IDXGIResource         ));

_COM_SMARTPTR_TYPEDEF(IMFActivate           , __uuidof(IMFActivate           ));
_COM_SMARTPTR_TYPEDEF(IMFAttributes         , __uuidof(IMFAttributes         ));
_COM_SMARTPTR_TYPEDEF(IMFDXGIDeviceManager  , __uuidof(IMFDXGIDeviceManager  ));
_COM_SMARTPTR_TYPEDEF(IMFTransform          , __uuidof(IMFTransform          ));
_COM_SMARTPTR_TYPEDEF(IMFMediaEventGenerator, __uuidof(IMFMediaEventGenerator));
_COM_SMARTPTR_TYPEDEF(IMFMediaType          , __uuidof(IMFMediaType          ));
_COM_SMARTPTR_TYPEDEF(IMFSample             , __uuidof(IMFSample             ));
_COM_SMARTPTR_TYPEDEF(IMFMediaBuffer        , __uuidof(IMFMediaBuffer        ));
_COM_SMARTPTR_TYPEDEF(IMF2DBuffer           , __uuidof(IMF2DBuffer           ));

namespace Capture
{
  class DXGI : public ICapture, public IMFAsyncCallback
  {
  public:
    DXGI();
    virtual ~DXGI();

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
      Sleep(400);
      return Initialize(m_options);
    }

    enum FrameType GetFrameType();
    size_t GetMaxFrameSize();
    enum GrabStatus GrabFrame(struct FrameInfo & frame, struct CursorInfo & cursor);

    /*
    Junk needed for the horrid IMFAsyncCallback interface
    */
    STDMETHODIMP QueryInterface(REFIID riid, void ** ppv)
    {
      if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
        *ppv = static_cast<IMFAsyncCallback*>(this);
        AddRef();
        return S_OK;
      } else {
        *ppv = NULL;
        return E_NOINTERFACE;
      }
    }

    STDMETHODIMP_(ULONG) AddRef()
    {
      return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release()
    {
      long cRef = InterlockedDecrement(&m_cRef);
      if (!cRef)
        delete this;
      return cRef;
    }

    STDMETHODIMP GetParameters(DWORD *pdwFlags, DWORD *pdwQueue) { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult *pAsyncResult);

  private:
    bool InitRawCapture();
    bool InitH264Capture();

    GrabStatus GrabFrameTexture(struct FrameInfo & frame, struct CursorInfo & cursor, ID3D11Texture2DPtr & texture, bool & timeout);
    GrabStatus GrabFrameRaw    (struct FrameInfo & frame, struct CursorInfo & cursor);
    GrabStatus GrabFrameH264   (struct FrameInfo & frame, struct CursorInfo & cursor);

    void WaitForDesktop();


    long             m_cRef;
    CaptureOptions * m_options;

    bool           m_initialized;
    unsigned int   m_width;
    unsigned int   m_height;
    enum FrameType m_frameType;

    MultiMemcpy                     m_memcpy;
    IDXGIFactory1Ptr                m_dxgiFactory;
    ID3D11DevicePtr                 m_device;
    D3D_FEATURE_LEVEL               m_featureLevel;
    ID3D11DeviceContextPtr          m_deviceContext;
    IDXGIOutput1Ptr                 m_output;
    IDXGIOutputDuplicationPtr       m_dup;
    bool                            m_releaseFrame;
    ID3D11Texture2DPtr              m_texture;
    D3D11_MAPPED_SUBRESOURCE        m_mapping;
    bool                            m_surfaceMapped;

    HANDLE                          m_encodeEvent;
    HANDLE                          m_shutdownEvent;
    bool                            m_encodeNeedsData;
    bool                            m_encodeHasData;
    CRITICAL_SECTION                m_encodeCS;

    UINT                            m_resetToken;
    IMFDXGIDeviceManagerPtr         m_mfDeviceManager;
    IMFActivatePtr                  m_mfActivation;
    IMFTransformPtr                 m_mfTransform;
    IMFMediaEventGeneratorPtr       m_mediaEventGen;

    BYTE *                          m_pointer;
    UINT                            m_pointerBufSize;
    UINT                            m_pointerSize;
    BOOL                            m_lastMouseVis;
    POINT                           m_lastMousePos;
  };
};