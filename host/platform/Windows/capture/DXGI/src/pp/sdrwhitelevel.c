/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

typedef struct HDR16to10
{
  ComScope * comScope;

  ID3D11Device        ** device;
  ID3D11DeviceContext ** context;

  bool shareable;
  ID3D11PixelShader   ** pshader;
  ID3D11SamplerState  ** sampler;
}
HDR16to10;
static HDR16to10 this = {0};

#define comRef_toGlobal(dst, src) \
  _comRef_toGlobal(this.comScope, dst, src)

typedef struct
{
  ID3D11Texture2D        ** tex;
  ID3D11RenderTargetView ** target;
}
HDR16to10Inst;

static bool hdr16to10_setup(
  ID3D11Device        ** device,
  ID3D11DeviceContext ** context,
  IDXGIOutput         ** output,
  bool                   shareable
)
{
  (void)output;
  bool result = false;
  HRESULT status;

  this.device    = device;
  this.context   = context;
  this.shareable = shareable;

  comRef_initGlobalScope(10, this.comScope);
  comRef_scopePush(10);

  static const char * pshaderSrc =
    "Texture2D    gInputTexture : register(t0);\n"
    "SamplerState gSamplerState : register(s0);\n"
    "static const float PQ_m1 = 0.1593017578125;\n"
    "static const float PQ_m2 = 78.84375;\n"
    "static const float PQ_c1 = 0.8359375;\n"
    "static const float PQ_c2 = 18.8515625;\n"
    "static const float PQ_c3 = 18.6875;\n"
    "float4 main(\n"
    "  float4 position : SV_POSITION,\n"
    "  float2 texCoord : TEXCOORD0) : SV_TARGET"
    "{\n"
    "  float4 color = gInputTexture.Sample(gSamplerState, texCoord);\n"
    "  float3 linear709 = color.rgb * (80.0 / 10000.0);\n"
    "  float3 linear2020 = float3(\n"
    "    dot(linear709, float3(0.6274039, 0.3292830, 0.0433131)),\n"
    "    dot(linear709, float3(0.0690973, 0.9195404, 0.0113623)),\n"
    "    dot(linear709, float3(0.0163914, 0.0880133, 0.8955953)));\n"
    "  float3 p = pow(max(linear2020, 0.0), PQ_m1);\n"
    "  color.rgb = pow((PQ_c1 + PQ_c2 * p) /\n"
    "    (1.0 + PQ_c3 * p), PQ_m2);\n"
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

  comRef_toGlobal(this.sampler, sampler);

  result = true;

exit:
  comRef_scopePop();
  return result;
}

static void hdr16to10_finish(void)
{
  comRef_freeScope(&this.comScope);
  memset(&this, 0, sizeof(this));
}

static bool hdr16to10_init(void ** opaque)
{
  HDR16to10Inst * inst = (HDR16to10Inst *)calloc(1, sizeof(*inst));
  if (!inst)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return false;
  }

 *opaque = inst;
 return true;
}

static void hdr16to10_free(void * opaque)
{
  HDR16to10Inst * inst = (HDR16to10Inst *)opaque;
  comRef_release(inst->target);
  comRef_release(inst->tex   );
  free(inst);
}

static bool hdr16to10_configure(void * opaque,
  int * width, int * height,
  int * cols , int * rows,
  CaptureFormat * format)
{
  HDR16to10Inst * inst = (HDR16to10Inst *)opaque;
  if (*format != CAPTURE_FMT_RGBA16F)
  {
    DEBUG_ERROR("HDR16to10 requires an FP16 scRGB source");
    return false;
  }

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

static ID3D11Texture2D * hdr16to10_run(void * opaque,
  ID3D11ShaderResourceView * srv)
{
  HDR16to10Inst * inst = (HDR16to10Inst *)opaque;

  // set the pixel shader & resource
  ID3D11DeviceContext_PSSetShader(*this.context, *this.pshader, NULL, 0);

  // set the pixel shader resources
  ID3D11DeviceContext_PSSetShaderResources(*this.context, 0, 1, &srv        );
  ID3D11DeviceContext_PSSetSamplers       (*this.context, 0, 1, this.sampler);

  // set the render target
  ID3D11DeviceContext_OMSetRenderTargets(*this.context, 1, inst->target, NULL);

  return *inst->tex;
}

DXGIPostProcess DXGIPP_HDR16to10 =
{
  .name      = "HDR16to10",
  .earlyInit = NULL,
  .setup     = hdr16to10_setup,
  .init      = hdr16to10_init,
  .free      = hdr16to10_free,
  .configure = hdr16to10_configure,
  .run       = hdr16to10_run,
  .finish    = hdr16to10_finish
};
