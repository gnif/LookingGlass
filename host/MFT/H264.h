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

#define W32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>

#include "Com.h"

namespace MFT
{
  enum H264_Event
  {
    H264_EVENT_ENCODE     = 0x01,
    H264_EVENT_NEEDS_DATA = 0x04,
    H264_EVENT_HAS_DATA   = 0x08,
    H264_EVENT_ERROR      = 0x10
  };

  class H264: public IMFAsyncCallback
  {
  public:
    H264();
    ~H264();
    bool Initialize(ID3D11DevicePtr device, unsigned int width, unsigned int height);
    void DeInitialize();
    unsigned int Process();
    bool ProvideFrame(ID3D11Texture2DPtr texture);
    bool GetFrame(void * buffer, const size_t bufferSize, unsigned int & dataLen);

    ID3D11DevicePtr                 m_device;
    unsigned int                    m_width;
    unsigned int                    m_height;

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

    /*
    Junk needed for the horrid IMFAsyncCallback interface
    */
    STDMETHODIMP QueryInterface(REFIID riid, void ** ppv)
    {
      if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
        *ppv = static_cast<IMFAsyncCallback*>(this);
        AddRef();
        return S_OK;
      }
      else {
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
    long m_cRef;
  };
};