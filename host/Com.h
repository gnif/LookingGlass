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
#include <comdef.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3d11.h>
#include <mftransform.h>

_COM_SMARTPTR_TYPEDEF(IDXGIFactory1           , __uuidof(IDXGIFactory1           ));
_COM_SMARTPTR_TYPEDEF(ID3D11Device            , __uuidof(ID3D11Device            ));
_COM_SMARTPTR_TYPEDEF(ID3D11DeviceContext     , __uuidof(ID3D11DeviceContext     ));
_COM_SMARTPTR_TYPEDEF(IDXGIDevice             , __uuidof(IDXGIDevice             ));
_COM_SMARTPTR_TYPEDEF(IDXGIOutput1            , __uuidof(IDXGIOutput1            ));
_COM_SMARTPTR_TYPEDEF(IDXGIOutput5            , __uuidof(IDXGIOutput5            ));
_COM_SMARTPTR_TYPEDEF(IDXGIOutput             , __uuidof(IDXGIOutput             ));
_COM_SMARTPTR_TYPEDEF(IDXGIAdapter1           , __uuidof(IDXGIAdapter1           ));
_COM_SMARTPTR_TYPEDEF(IDXGIOutputDuplication  , __uuidof(IDXGIOutputDuplication  ));
_COM_SMARTPTR_TYPEDEF(ID3D11Texture2D         , __uuidof(ID3D11Texture2D         ));
_COM_SMARTPTR_TYPEDEF(IDXGIResource           , __uuidof(IDXGIResource           ));

_COM_SMARTPTR_TYPEDEF(ID3D10Multithread       , __uuidof(ID3D10Multithread       ));
_COM_SMARTPTR_TYPEDEF(IMFActivate             , __uuidof(IMFActivate             ));
_COM_SMARTPTR_TYPEDEF(IMFAttributes           , __uuidof(IMFAttributes           ));
_COM_SMARTPTR_TYPEDEF(IMFDXGIDeviceManager    , __uuidof(IMFDXGIDeviceManager    ));
_COM_SMARTPTR_TYPEDEF(IMFTransform            , __uuidof(IMFTransform            ));
_COM_SMARTPTR_TYPEDEF(IMFMediaEvent           , __uuidof(IMFMediaEvent           ));
_COM_SMARTPTR_TYPEDEF(IMFMediaEventGenerator  , __uuidof(IMFMediaEventGenerator  ));
_COM_SMARTPTR_TYPEDEF(IMFMediaType            , __uuidof(IMFMediaType            ));
_COM_SMARTPTR_TYPEDEF(IMFSample               , __uuidof(IMFSample               ));
_COM_SMARTPTR_TYPEDEF(IMFMediaBuffer          , __uuidof(IMFMediaBuffer          ));
_COM_SMARTPTR_TYPEDEF(IMF2DBuffer             , __uuidof(IMF2DBuffer             ));

_COM_SMARTPTR_TYPEDEF(ID3D11RenderTargetView  , __uuidof(ID3D11RenderTargetView  ));
_COM_SMARTPTR_TYPEDEF(ID3D11ShaderResourceView, __uuidof(ID3D11ShaderResourceView));
_COM_SMARTPTR_TYPEDEF(ID3D11DepthStencilView  , __uuidof(ID3D11DepthStencilView  ));
_COM_SMARTPTR_TYPEDEF(ID3D11InputLayout       , __uuidof(ID3D11InputLayout       ));
_COM_SMARTPTR_TYPEDEF(ID3D11VertexShader      , __uuidof(ID3D11VertexShader      ));
_COM_SMARTPTR_TYPEDEF(ID3D11PixelShader       , __uuidof(ID3D11PixelShader       ));
_COM_SMARTPTR_TYPEDEF(ID3D11SamplerState      , __uuidof(ID3D11SamplerState      ));
_COM_SMARTPTR_TYPEDEF(ID3D11Buffer            , __uuidof(ID3D11Buffer            ));