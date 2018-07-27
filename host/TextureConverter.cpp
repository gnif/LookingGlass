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

#include "TextureConverter.h"
#include "common\debug.h"

#include "Shaders\Vertex.h"
#include "Shaders\BGRAtoNV12.h"

TextureConverter::TextureConverter()
{
}

TextureConverter::~TextureConverter()
{
  DeInitialize();
}

bool TextureConverter::Initialize(
  ID3D11DeviceContextPtr deviceContext,
  ID3D11DevicePtr        device,
  const unsigned int     width,
  const unsigned int     height,
  DXGI_FORMAT            format
)
{
  HRESULT                         result;
  D3D11_TEXTURE2D_DESC            texDesc;
  D3D11_RENDER_TARGET_VIEW_DESC   viewDesc;
  D3D11_SHADER_RESOURCE_VIEW_DESC shaderDesc;
  D3D11_SAMPLER_DESC              samplerDesc;

  m_deviceContext = deviceContext;
  m_device        = device;
  m_width         = width;
  m_height        = height;
  m_format        = format;

  switch (format)
  {
    case DXGI_FORMAT_NV12:
      result = device->CreatePixelShader(g_BGRAtoNV12, sizeof(g_BGRAtoNV12), NULL, &m_pixelShader);
      break;

    default:
      DEBUG_ERROR("Unsupported format");
      return false;
  }

  if (FAILED(result))
  {
    DeInitialize();
    DEBUG_ERROR("Failed to create the pixel shader");
    return false;
  }

  const D3D11_INPUT_ELEMENT_DESC inputDesc[] =
  {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT   , 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 }
  };
  
  result = device->CreateInputLayout(inputDesc, _countof(inputDesc), g_Vertex, sizeof(g_Vertex), &m_layout);
  if (FAILED(result))
  {
    DeInitialize();
    DEBUG_ERROR("Failed to create the input layout");
    return false;
  }

  result = device->CreateVertexShader(g_Vertex, sizeof(g_Vertex), NULL, &m_vertexShader);
  if (FAILED(result))
  {
    DeInitialize();
    DEBUG_ERROR("Failed to create the vertex shader");
    return false;
  }

  ZeroMemory(&texDesc    , sizeof(texDesc    ));
  ZeroMemory(&viewDesc   , sizeof(viewDesc   ));
  ZeroMemory(&shaderDesc , sizeof(shaderDesc ));
  ZeroMemory(&samplerDesc, sizeof(samplerDesc));

  texDesc.Width            = width;
  texDesc.Height           = height;
  texDesc.MipLevels        = 1;
  texDesc.ArraySize        = 1;
  texDesc.SampleDesc.Count = 1;
  texDesc.Usage            = D3D11_USAGE_DEFAULT;
  texDesc.CPUAccessFlags   = 0;
  texDesc.MiscFlags        = 0;
  texDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
  texDesc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  result = device->CreateTexture2D(&texDesc, NULL, &m_targetTexture);
  if (FAILED(result))
  {
    DeInitialize();
    DEBUG_ERROR("Failed to create the render texture");
    return false;
  }

  viewDesc.Format             = texDesc.Format;
  viewDesc.ViewDimension      = D3D11_RTV_DIMENSION_TEXTURE2D;
  viewDesc.Texture2D.MipSlice = 0;
  result = device->CreateRenderTargetView(m_targetTexture, &viewDesc, &m_renderView);
  if (FAILED(result))
  {
    DeInitialize();
    DEBUG_ERROR("Failed to create the render view");
    return false;
  }

  shaderDesc.Format                    = texDesc.Format;
  shaderDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
  shaderDesc.Texture2D.MostDetailedMip = 0;
  shaderDesc.Texture2D.MipLevels       = 1;

  result = device->CreateShaderResourceView(m_targetTexture, &shaderDesc, &m_shaderView);
  if (FAILED(result))
  {
    DeInitialize();
    DEBUG_ERROR("Failed to create shader resource view");
    return false;
  }

  samplerDesc.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  samplerDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
  samplerDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
  samplerDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
  samplerDesc.MipLODBias     = 0.0f;
  samplerDesc.MaxAnisotropy  = 1;
  samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  samplerDesc.BorderColor[0] = 1.0f;
  samplerDesc.BorderColor[1] = 1.0f;
  samplerDesc.BorderColor[2] = 1.0f;
  samplerDesc.MinLOD         = -FLT_MAX;
  samplerDesc.MaxLOD         = FLT_MAX;

  result = device->CreateSamplerState(&samplerDesc, &m_samplerState);
  if (FAILED(result))
  {
    DeInitialize();
    DEBUG_ERROR("Failed to create sampler state");
    return false;
  }

  return IntializeBuffers();
}

bool TextureConverter::IntializeBuffers()
{
  struct VS_INPUT      * verticies;
  unsigned long        * indicies;
  HRESULT                result;
  D3D11_BUFFER_DESC      vertexDesc, indexDesc;
  D3D11_SUBRESOURCE_DATA vertexData, indexData;

  m_vertexCount = 4;
  m_indexCount  = 4;

  verticies = new struct VS_INPUT[m_vertexCount];
  if (!verticies)
  {
    DeInitialize();
    DEBUG_ERROR("new failure");
    return false;
  }

  indicies = new unsigned long[m_indexCount];
  if (!indicies)
  {
    DeInitialize();
    DEBUG_ERROR("new failure");
    return false;
  }

  verticies[0].pos = DirectX::XMFLOAT3(-1.0f, -1.0f, 0.5f); //BL
  verticies[1].pos = DirectX::XMFLOAT3(-1.0f,  1.0f, 0.5f); //TL
  verticies[2].pos = DirectX::XMFLOAT3( 1.0f, -1.0f, 0.5f); //BR
  verticies[3].pos = DirectX::XMFLOAT3( 1.0f,  1.0f, 0.5f); //TR
  verticies[0].tex = DirectX::XMFLOAT2(0.0f, 1.0f);
  verticies[1].tex = DirectX::XMFLOAT2(0.0f, 0.0f);
  verticies[2].tex = DirectX::XMFLOAT2(1.0f, 1.0f);
  verticies[3].tex = DirectX::XMFLOAT2(1.0f, 0.0f);
  indicies[0] = 0;
  indicies[1] = 1;
  indicies[2] = 2;
  indicies[3] = 3;

  vertexDesc.Usage               = D3D11_USAGE_DEFAULT;
  vertexDesc.ByteWidth           = sizeof(struct VS_INPUT) * m_vertexCount;
  vertexDesc.BindFlags           = D3D11_BIND_VERTEX_BUFFER;
  vertexDesc.CPUAccessFlags      = 0;
  vertexDesc.MiscFlags           = 0;
  vertexDesc.StructureByteStride = 0;

  vertexData.pSysMem          = verticies;
  vertexData.SysMemPitch      = 0;
  vertexData.SysMemSlicePitch = 0;

  result = m_device->CreateBuffer(&vertexDesc, &vertexData, &m_vertexBuffer);
  if (FAILED(result))
  {
    delete[] indicies;
    delete[] verticies;
    DeInitialize();
    DEBUG_ERROR("Failed to create vertex buffer");
    return false;
  }

  indexDesc.Usage               = D3D11_USAGE_DEFAULT;
  indexDesc.ByteWidth           = sizeof(unsigned long) * m_indexCount;
  indexDesc.BindFlags           = D3D11_BIND_INDEX_BUFFER;
  indexDesc.CPUAccessFlags      = 0;
  indexDesc.MiscFlags           = 0;
  indexDesc.StructureByteStride = 0;

  indexData.pSysMem          = indicies;
  indexData.SysMemPitch      = 0;
  indexData.SysMemSlicePitch = 0;

  result = m_device->CreateBuffer(&indexDesc, &indexData, &m_indexBuffer);
  if (FAILED(result))
  {
    delete[] indicies;
    delete[] verticies;
    DeInitialize();
    DEBUG_ERROR("Failed to create index buffer");
    return false;
  }

  delete[] indicies;
  delete[] verticies;
  return true;
}

void TextureConverter::DeInitialize()
{
  SafeRelease(&m_samplerState );
  SafeRelease(&m_indexBuffer  );
  SafeRelease(&m_vertexBuffer );
  SafeRelease(&m_shaderView   );
  SafeRelease(&m_renderView   );
  SafeRelease(&m_targetTexture);
  SafeRelease(&m_vertexShader );
  SafeRelease(&m_pixelShader  );
  SafeRelease(&m_layout       );
}

bool TextureConverter::Convert(ID3D11Texture2DPtr &texture)
{
  unsigned int stride;
  unsigned int offset;
  float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

  HRESULT                         result;
  D3D11_TEXTURE2D_DESC            texDesc;
  D3D11_SHADER_RESOURCE_VIEW_DESC shaderDesc;
  ID3D11ShaderResourceViewPtr     textureView;
  texture->GetDesc(&texDesc);
  shaderDesc.Format                    = texDesc.Format;
  shaderDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
  shaderDesc.Texture2D.MostDetailedMip = 0;
  shaderDesc.Texture2D.MipLevels       = 1;

  result = m_device->CreateShaderResourceView(texture, &shaderDesc, &textureView);
  if (FAILED(result))
  {
    DeInitialize();
    DEBUG_ERROR("Failed to create shader resource view");
    return false;
  }

  
  ID3D11Buffer             *buffers      [] = { m_vertexBuffer.GetInterfacePtr() };
  ID3D11RenderTargetView   *renderViews  [] = { m_renderView  .GetInterfacePtr() };
  ID3D11SamplerState       *samplerStates[] = { m_samplerState.GetInterfacePtr() };
  ID3D11ShaderResourceView *shaderViews  [] = { textureView   .GetInterfacePtr() };
  D3D11_VIEWPORT            viewPorts    [] = { {
    0.0f, 0.0f,
    (float)m_width, (float)m_height,
    0.0f, 1.0f
  } };

  m_deviceContext->OMSetRenderTargets(1, renderViews, NULL);
  m_deviceContext->ClearRenderTargetView(m_renderView, color);

  stride = sizeof(VS_INPUT);
  offset = 0;

  m_deviceContext->RSSetViewports        (_countof(viewPorts), viewPorts);
  m_deviceContext->IASetInputLayout      (m_layout);
  m_deviceContext->IASetVertexBuffers    (0, _countof(buffers), buffers, &stride, &offset);
  m_deviceContext->IASetIndexBuffer      (m_indexBuffer, DXGI_FORMAT_R32_UINT, 0);
  m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  m_deviceContext->VSSetShader           (m_vertexShader, NULL, 0);
  m_deviceContext->PSSetSamplers         (0, _countof(samplerStates), samplerStates);
  m_deviceContext->PSSetShaderResources  (0, _countof(shaderViews  ), shaderViews  );
  m_deviceContext->PSSetShader           (m_pixelShader , NULL, 0);

  m_deviceContext->DrawIndexed(m_indexCount, 0, 0);

  SafeRelease(&textureView);
  SafeRelease(&texture);
  texture = m_targetTexture;
  return true;
}
