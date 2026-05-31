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

#include "downsample_parser.h"

#include "common/debug.h"
#include "common/windebug.h"

#include <math.h>

typedef struct Downsample
{
  ComScope * comScope;

  ID3D11Device        ** device;
  ID3D11DeviceContext ** context;
  bool shareable;

  bool                   disabled;
  int                    width , height;
  ID3D11SamplerState  ** sampler;
  ID3D11PixelShader   ** pshader;
}
Downsample;
static Downsample this = {0};

#define comRef_toGlobal(dst, src) \
  _comRef_toGlobal(this.comScope, dst, src)

typedef struct
{
  ID3D11Texture2D        ** tex;
  ID3D11RenderTargetView ** target;
}
DownsampleInst;

static Vector downsampleRules = {0};

static void downsample_earlyInit(void)
{
  struct Option options[] =
  {
    DOWNSAMPLE_PARSER("dxgi", &downsampleRules),
    {0}
  };

  option_register(options);
}

static bool downsample_setup(
  ID3D11Device        ** device,
  ID3D11DeviceContext ** context,
  IDXGIOutput         ** output,
  bool                   shareable
)
{
  this.device    = device;
  this.context   = context;
  this.shareable = shareable;

  comRef_initGlobalScope(10, this.comScope);
  return true;
}

static void downsample_finish(void)
{
  comRef_freeScope(&this.comScope);
  memset(&this, 0, sizeof(this));
}

static bool downsample_configure(void * opaque,
  int * width, int * height,
  int * cols , int * rows  ,
  CaptureFormat * format)
{
  bool result = false;
  DownsampleInst * inst = (DownsampleInst *)opaque;

  if (*format == CAPTURE_FMT_BGR_32)
    this.disabled = true;

  if (this.disabled)
    return true;

  HRESULT status;
  comRef_scopePush(10);

  if (!this.pshader)
  {
    DownsampleRule * rule = downsampleRule_match(&downsampleRules,
      *width, *height);

    if (!rule || (rule->targetX == *width && rule->targetY == *height))
    {
      this.disabled = true;
      result = true;
      goto exit;
    }

    this.width  = rule->targetX;
    this.height = rule->targetY;

    DEBUG_INFO("Downsampling to: %u x %u", this.width, this.height);

    static const char * pshaderSrc =
      "Texture2D    gInputTexture : register(t0);\n"
      "SamplerState gSamplerState : register(s0);\n"
      "\n"
      "float4 main(\n"
      "  float4 position : SV_POSITION,\n"
      "  float2 texCoord : TEXCOORD0) : SV_TARGET"
      "{\n"
      "  return gInputTexture.Sample(gSamplerState, texCoord);\n"
      "}\n";

    comRef_defineLocal(ID3DBlob, byteCode);
    if (!compileShader(byteCode, "main", "ps_5_0", pshaderSrc, NULL))
      goto exit;

    comRef_defineLocal(ID3D11PixelShader, pshader);
    HRESULT status = ID3D11Device_CreatePixelShader(
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

    comRef_toGlobal(this.pshader, pshader);
    comRef_toGlobal(this.sampler, sampler);
  }

  D3D11_TEXTURE2D_DESC texDesc =
  {
    .Width              = this.width,
    .Height             = this.height,
    .MipLevels          = 1,
    .ArraySize          = 1,
    .SampleDesc.Count   = 1,
    .SampleDesc.Quality = 0,
    .Usage              = D3D11_USAGE_DEFAULT,
    .Format             = getDXGIFormat(*format),
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
  status = ID3D11Device_CreateTexture2D(
    *this.device, &texDesc, NULL, tex);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create the output texture", status);
    goto exit;
  }

  comRef_defineLocal(ID3D11RenderTargetView, target);
  status = ID3D11Device_CreateRenderTargetView(
    *this.device, *(ID3D11Resource **)tex, NULL, target);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create the render target view", status);
    goto exit;
  }

  *width  = *cols = this.width;
  *height = *rows = this.height;

  comRef_toGlobal(inst->tex    , tex    );
  comRef_toGlobal(inst->target , target );
  result = true;

exit:
  comRef_scopePop();
  return result;
}

static bool downsample_init(void ** opaque)
{
  DownsampleInst * inst = (DownsampleInst *)calloc(1, sizeof(*inst));
  if (!inst)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return false;
  }

  *opaque = inst;
  return true;
}

static void downsample_free(void * opaque)
{
  DownsampleInst * inst = (DownsampleInst *)opaque;
  comRef_release(inst->target);
  comRef_release(inst->tex   );
  free(inst);
}

static ID3D11Texture2D * downsample_run(void * opaque,
  ID3D11ShaderResourceView * srv)
{
  if (this.disabled)
    return NULL;

  DownsampleInst * inst = (DownsampleInst *)opaque;

  // set the pixel shader & resources
  ID3D11DeviceContext_PSSetShader(*this.context, *this.pshader, NULL, 0);
  ID3D11DeviceContext_PSSetSamplers       (*this.context, 0, 1, this.sampler);
  ID3D11DeviceContext_PSSetShaderResources(*this.context, 0, 1, &srv);

  // set the render target
  ID3D11DeviceContext_OMSetRenderTargets(*this.context, 1, inst->target, NULL);

  return *inst->tex;
}

DXGIPostProcess DXGIPP_Downsample =
{
  .name      = "Downsample",
  .earlyInit = downsample_earlyInit,
  .setup     = downsample_setup,
  .init      = downsample_init,
  .free      = downsample_free,
  .configure = downsample_configure,
  .run       = downsample_run,
  .finish    = downsample_finish
};
