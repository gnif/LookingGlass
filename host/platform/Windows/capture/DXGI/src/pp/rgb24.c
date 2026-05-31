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

#include <math.h>

typedef struct RGB24
{
  ComScope * comScope;

  ID3D11Device        ** device;
  ID3D11DeviceContext ** context;
  bool shareable;

  int                    width;
  int                    height;
  ID3D11PixelShader   ** pshader;
}
RGB24;
static RGB24 this = {0};

#define comRef_toGlobal(dst, src) \
  _comRef_toGlobal(this.comScope, dst, src)

typedef struct
{
  ID3D11Texture2D        ** tex;
  ID3D11RenderTargetView ** target;
}
RGB24Inst;


static bool rgb24_setup(
  ID3D11Device        ** device,
  ID3D11DeviceContext ** context,
  IDXGIOutput         ** output,
  bool                   shareable
)
{
  comRef_initGlobalScope(10, this.comScope);
  this.device    = device;
  this.context   = context;
  this.shareable = shareable;
  return true;
}

static void rgb24_finish(void)
{
  comRef_freeScope(&this.comScope);
  memset(&this, 0, sizeof(this));
}

static bool rgb24_configure(void * opaque,
  int * width, int * height,
  int * cols , int * rows  ,
  CaptureFormat * format)
{
  RGB24Inst * inst = (RGB24Inst *)opaque;

  HRESULT status;

  comRef_scopePush(10);

  if (!this.pshader)
  {
    const unsigned packedPitch = ALIGN_TO(*cols * 3, 4);

    /* we must align to 64 byte boundaries to avoid breaking dmabuf import */
    this.width  = ALIGN_TO(packedPitch / 4, 64);

    /* adjust for the aligned width */
    this.height = (*cols * *rows) / (packedPitch / 3);

    char sOutputWidth[6], sOutputHeight[6];
    snprintf(sOutputWidth , sizeof(sOutputWidth) , "%d", this.width );
    snprintf(sOutputHeight, sizeof(sOutputHeight), "%d", this.height);

    const D3D_SHADER_MACRO defines[] =
    {
      {"OUTPUT_WIDTH" , sOutputWidth },
      {"OUTPUT_HEIGHT", sOutputHeight},
      {NULL, NULL}
    };

    static const char * pshaderSrc =
      "Texture2D<float4> gInputTexture : register(t0);\n"
      "\n"
      "float4 main(\n"
      "  float4 position : SV_POSITION,\n"
      "  float2 texCoord : TEXCOORD0) : SV_TARGET\n"
      "{\n"
      "  uint inputAndOutputY = uint(texCoord.y * OUTPUT_HEIGHT);\n"
      "  uint outputX = uint(texCoord.x * OUTPUT_WIDTH);\n"
      "\n"
      "  uint fstInputX = (outputX * 4) / 3;\n"
      "  float4 color0 = gInputTexture.Load(\n"
      "    uint3(fstInputX, inputAndOutputY, 0));\n"
      "\n"
      "  uint sndInputX = fstInputX + 1;\n"
      "  float4 color3 = gInputTexture.Load(\n"
      "    uint3(sndInputX, inputAndOutputY, 0));\n"
      "\n"
      "  uint outputXMod3 = outputX % 3;\n"
      "\n"
      "  float4 color1 = outputXMod3 <= 1 ? color0 : color3;\n"
      "  float4 color2 = outputXMod3 == 0 ? color0 : color3;\n"
      "\n"
      "  float b = color0.bgr[outputXMod3];\n"
      "  float g = color1.grb[outputXMod3];\n"
      "  float r = color2.rbg[outputXMod3];\n"
      "  float a = color3.bgr[outputXMod3];\n"
      "  return float4(r, g, b, a);\n"
      "}\n";

    comRef_defineLocal(ID3DBlob, byteCode);
    if (!compileShader(byteCode, "main", "ps_5_0", pshaderSrc, defines))
      goto fail;

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
      goto fail;
    }

    comRef_toGlobal(this.pshader, pshader);

    DEBUG_INFO("RGBA to RGB packing enabled, %.2f MiB per frame saved",
      (float)((*width * *height - this.width * this.height) * 4) / 1048576
    );
    DEBUG_INFO("Packed size: %ux%u", this.width, this.height);
  }

  // This texture is actually going to contain the packed BGR24 output
  D3D11_TEXTURE2D_DESC texDesc =
  {
    .Width              = this.width,
    .Height             = this.height,
    .MipLevels          = 1,
    .ArraySize          = 1,
    .SampleDesc.Count   = 1,
    .SampleDesc.Quality = 0,
    .Usage              = D3D11_USAGE_DEFAULT,
    .Format             = DXGI_FORMAT_B8G8R8A8_UNORM,
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

  *cols   = this.width;
  *rows   = this.height;
  *format = CAPTURE_FMT_BGR_32;

  comRef_toGlobal(inst->tex   , tex    );
  comRef_toGlobal(inst->target, target );

  comRef_scopePop();
  return true;

fail:
  comRef_scopePop();
  return false;
}

static bool rgb24_init(void ** opaque)
{
  RGB24Inst * inst = (RGB24Inst *)calloc(1, sizeof(*inst));
  if (!inst)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return false;
  }

  *opaque = inst;
  return true;
}

static void rgb24_free(void * opaque)
{
  RGB24Inst * inst = (RGB24Inst *)opaque;
  comRef_release(inst->target);
  comRef_release(inst->tex   );
  free(inst);
}

static ID3D11Texture2D * rgb24_run(void * opaque,
  ID3D11ShaderResourceView * srv)
{
  RGB24Inst * inst = (RGB24Inst *)opaque;

  // set the pixel shader & resources
  ID3D11DeviceContext_PSSetShader(*this.context, *this.pshader, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(*this.context, 0, 1, &srv);

  // set the render target
  ID3D11DeviceContext_OMSetRenderTargets(*this.context, 1, inst->target, NULL);

  return *inst->tex;
}

DXGIPostProcess DXGIPP_RGB24 =
{
  .name      = "RGB24",
  .earlyInit = NULL,
  .setup     = rgb24_setup,
  .init      = rgb24_init,
  .free      = rgb24_free,
  .configure = rgb24_configure,
  .run       = rgb24_run,
  .finish    = rgb24_finish
};
