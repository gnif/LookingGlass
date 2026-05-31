/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "pp.h"
#include "com_ref.h"
#include "util.h"

#include "common/debug.h"
#include "common/windebug.h"
#include "common/display.h"

#include <dxgi1_6.h>

typedef struct SDRWhiteLevel
{
  ComScope * comScope;

  ID3D11Device        ** device;
  ID3D11DeviceContext ** context;

  bool shareable;
  ID3D11PixelShader   ** pshader;
  ID3D11SamplerState  ** sampler;
  ID3D11Buffer        ** buffer;

  DISPLAYCONFIG_PATH_INFO displayPathInfo;
  float sdrWhiteLevel;
}
SDRWhiteLevel;
static SDRWhiteLevel this = {0};

#define comRef_toGlobal(dst, src) \
  _comRef_toGlobal(this.comScope, dst, src)

typedef struct
{
  ID3D11Texture2D        ** tex;
  ID3D11RenderTargetView ** target;
}
SDRWhiteLevelInst;

struct ShaderConsts
{
  float sdrWhiteLevel;
}
__attribute__((aligned(16)));

static void updateConsts(void);

static bool sdrWhiteLevel_setup(
  ID3D11Device        ** device,
  ID3D11DeviceContext ** context,
  IDXGIOutput         ** output,
  bool                   shareable
)
{
  bool result = false;
  HRESULT status;

  this.device    = device;
  this.context   = context;
  this.shareable = shareable;

  comRef_initGlobalScope(11, this.comScope);
  comRef_scopePush(11);

  comRef_defineLocal(IDXGIOutput6, output6);
  status = IDXGIOutput_QueryInterface(
    *output, &IID_IDXGIOutput6, (void **)output6);

  if (!SUCCEEDED(status))
  {
    DEBUG_ERROR("Failed to get the IDXGIOutput6 interface");
    goto exit;
  }

  DXGI_OUTPUT_DESC1 desc1;
  IDXGIOutput6_GetDesc1(*output6, &desc1);
  if (!display_getPathInfo(desc1.Monitor, &this.displayPathInfo))
  {
    DEBUG_ERROR("Failed to get the display path info");
    goto exit;
  }

  static const char * pshaderSrc =
    "Texture2D    gInputTexture : register(t0);\n"
    "SamplerState gSamplerState : register(s0);\n"
    "cbuffer      gConsts       : register(b0)\n"
    "{\n"
    "  float SDRWhiteLevel;"
    "};\n"
    "\n"
    "float4 main(\n"
    "  float4 position : SV_POSITION,\n"
    "  float2 texCoord : TEXCOORD0) : SV_TARGET"
    "{\n"
    "  float4 color = gInputTexture.Sample(gSamplerState, texCoord);\n"
    "  color.rgb   *= SDRWhiteLevel;\n"
    "  return color;\n"
    "}\n";

  comRef_defineLocal(ID3DBlob, byteCode);
  if (!compileShader(byteCode, "main", "ps_5_0", pshaderSrc, NULL))
    goto exit;

  comRef_defineLocal(ID3D11PixelShader, pshader);
  status = ID3D11Device_CreatePixelShader(
    *this.device,
    ID3D10Blob_GetBufferPointer(*byteCode),
    ID3D10Blob_GetBufferSize   (*byteCode),
    NULL,
    pshader);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create the pixel shader", status);
    goto exit;
  }

  comRef_toGlobal(this.pshader, pshader);

  const D3D11_SAMPLER_DESC samplerDesc =
  {
    .Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
    .AddressU       = D3D11_TEXTURE_ADDRESS_WRAP,
    .AddressV       = D3D11_TEXTURE_ADDRESS_WRAP,
    .AddressW       = D3D11_TEXTURE_ADDRESS_WRAP,
    .ComparisonFunc = D3D11_COMPARISON_NEVER,
    .MinLOD         = 0,
    .MaxLOD         = D3D11_FLOAT32_MAX
  };

  comRef_defineLocal(ID3D11SamplerState, sampler);
  status = ID3D11Device_CreateSamplerState(*this.device, &samplerDesc, sampler);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create the sampler state", status);
    goto exit;
  }

  D3D11_BUFFER_DESC bufferDesc =
  {
    .ByteWidth      = sizeof(struct ShaderConsts),
    .Usage          = D3D11_USAGE_DEFAULT,
    .BindFlags      = D3D11_BIND_CONSTANT_BUFFER,
  };

  comRef_defineLocal(ID3D11Buffer, buffer);
  status = ID3D11Device_CreateBuffer(
    *this.device, &bufferDesc, NULL,
    buffer);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create the constant buffer", status);
    goto exit;
  }

  comRef_toGlobal(this.sampler, sampler);
  comRef_toGlobal(this.buffer , buffer );

  updateConsts();
  DEBUG_INFO("SDR White Level   : %f"   , this.sdrWhiteLevel);

  result = true;

exit:
  comRef_scopePop();
  return result;
}

static void sdrWhiteLevel_finish(void)
{
  comRef_freeScope(&this.comScope);
  memset(&this, 0, sizeof(this));
}

static bool sdrWhiteLevel_init(void ** opaque)
{
  SDRWhiteLevelInst * inst = (SDRWhiteLevelInst *)calloc(1, sizeof(*inst));
  if (!inst)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return false;
  }

 *opaque = inst;
 return true;
}

static void sdrWhiteLevel_free(void * opaque)
{
  SDRWhiteLevelInst * inst = (SDRWhiteLevelInst *)opaque;
  comRef_release(inst->target);
  comRef_release(inst->tex   );
  free(inst);
}

static void updateConsts(void)
{
  float nits = display_getSDRWhiteLevel(&this.displayPathInfo);
  if (nits == this.sdrWhiteLevel)
    return;

  this.sdrWhiteLevel = nits;

  struct ShaderConsts consts = { .sdrWhiteLevel = 80.0f / nits };
  ID3D11DeviceContext_UpdateSubresource(
    *this.context, *(ID3D11Resource**)this.buffer,
    0, NULL, &consts, 0, 0);
}

static bool sdrWhiteLevel_configure(void * opaque,
  int * width, int * height,
  int * cols , int * rows,
  CaptureFormat * format)
{
  SDRWhiteLevelInst * inst = (SDRWhiteLevelInst *)opaque;
  if (inst->tex)
    return true;

  comRef_scopePush(10);

  // create the output texture
  D3D11_TEXTURE2D_DESC texDesc =
  {
    .Width              = *width,
    .Height             = *height,
    .MipLevels          = 1,
    .ArraySize          = 1,
    .SampleDesc.Count   = 1,
    .SampleDesc.Quality = 0,
    .Usage              = D3D11_USAGE_DEFAULT,
    .Format             = DXGI_FORMAT_R10G10B10A2_UNORM,
    .BindFlags          = D3D11_BIND_RENDER_TARGET |
                          D3D11_BIND_SHADER_RESOURCE,
    .CPUAccessFlags     = 0,
    .MiscFlags          = 0
  };

  // allow texture sharing with other backends
  if (this.shareable)
    texDesc.MiscFlags |=
      D3D11_RESOURCE_MISC_SHARED |
      D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

  comRef_defineLocal(ID3D11Texture2D, tex);
  HRESULT status = ID3D11Device_CreateTexture2D(
    *this.device, &texDesc, NULL, tex);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create the output texture", status);
    goto fail;
  }

  comRef_defineLocal(ID3D11RenderTargetView, target);
  status = ID3D11Device_CreateRenderTargetView(
    *this.device, *(ID3D11Resource **)tex, NULL, target);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create the render target view", status);
    goto fail;
  }

  comRef_toGlobal(inst->tex   , tex   );
  comRef_toGlobal(inst->target, target);

  comRef_scopePop();

  *format = CAPTURE_FMT_RGBA10;
  return true;

fail:
  comRef_scopePop();
  return false;
}

static ID3D11Texture2D * sdrWhiteLevel_run(void * opaque,
  ID3D11ShaderResourceView * srv)
{
  SDRWhiteLevelInst * inst = (SDRWhiteLevelInst *)opaque;

  updateConsts();

  // set the pixel shader & resource
  ID3D11DeviceContext_PSSetShader(*this.context, *this.pshader, NULL, 0);

  // set the pixel shader resources
  ID3D11DeviceContext_PSSetShaderResources(*this.context, 0, 1, &srv        );
  ID3D11DeviceContext_PSSetSamplers       (*this.context, 0, 1, this.sampler);
  ID3D11DeviceContext_PSSetConstantBuffers(*this.context, 0, 1, this.buffer );

  // set the render target
  ID3D11DeviceContext_OMSetRenderTargets(*this.context, 1, inst->target, NULL);

  return *inst->tex;
}

DXGIPostProcess DXGIPP_SDRWhiteLevel =
{
  .name      = "SDRWhiteLevel",
  .earlyInit = NULL,
  .setup     = sdrWhiteLevel_setup,
  .init      = sdrWhiteLevel_init,
  .free      = sdrWhiteLevel_free,
  .configure = sdrWhiteLevel_configure,
  .run       = sdrWhiteLevel_run,
  .finish    = sdrWhiteLevel_finish
};
