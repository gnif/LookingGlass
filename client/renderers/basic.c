#include "lg-renderer.h"

#include <stdbool.h>
#include <stdint.h>
#include <SDL2/SDL.h>

#include "debug.h"
#include "memcpySSE.h"

struct LGR_Basic
{
  bool               initialized;
  LG_RendererFormat  format;
  size_t             texSize;
  size_t             dataWidth;
  SDL_Renderer     * renderer;
  SDL_Texture      * texture;
};

const char * lgr_basic_get_name()
{
  return "Basic";
}

bool lgr_basic_initialize(void ** opaque, const LG_RendererParams params, const LG_RendererFormat format)
{
  // create our local storage
  *opaque = malloc(sizeof(struct LGR_Basic));
  if (!*opaque)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(struct LGR_Basic));
    return false;
  }
  memset(*opaque, 0, sizeof(struct LGR_Basic));
  struct LGR_Basic * this = (struct LGR_Basic *)*opaque;

  this->renderer = SDL_CreateRenderer(params.window, -1,
    SDL_RENDERER_ACCELERATED |
    (params.vsync ? SDL_RENDERER_PRESENTVSYNC : 0)
  );

  if (!this->renderer)
  {
    DEBUG_ERROR("Failed to create renderer");
    return false;
  }

  Uint32 sdlFormat;
  switch(format.bpp)
  {
    case 24:
      sdlFormat = SDL_PIXELFORMAT_RGB24;
      break;

    case 32:
      sdlFormat = SDL_PIXELFORMAT_ARGB8888;
      break;

    default:
      DEBUG_ERROR("Unsupported bpp");
      return false;
  }

  // calculate the texture size in bytes
  this->texSize = format.height * format.pitch;

  // create the target texture
  this->texture = SDL_CreateTexture(
    this->renderer,
    sdlFormat,
    SDL_TEXTUREACCESS_STREAMING,
    format.width,
    format.height
  );

  if (!this->texture)
  {
    DEBUG_ERROR("SDL_CreateTexture failed");
    return false;
  }


  memcpy(&this->format, &format, sizeof(LG_RendererFormat));
  this->dataWidth   = this->format.width * (this->format.bpp / 8);
  this->initialized = true;
  return true;
}

void lgr_basic_deinitialize(void * opaque)
{
  struct LGR_Basic * this = (struct LGR_Basic *)opaque;
  if (!this)
    return;

  if (this->texture)
    SDL_DestroyTexture(this->texture);

  if (this->renderer)
    SDL_DestroyRenderer(this->renderer);

  free(this);
}

bool lgr_basic_is_compatible(void * opaque, const LG_RendererFormat format)
{
  const struct LGR_Basic * this = (struct LGR_Basic *)opaque;
  if (!this || !this->initialized)
    return false;

  return (memcmp(&this->format, &format, sizeof(LG_RendererFormat)) == 0);
}

void lgr_basic_on_resize(void * opaque, const int width, const int height)
{
  const struct LGR_Basic * this = (struct LGR_Basic *)opaque;
  if (!this || !this->initialized)
    return;
}

bool lgr_basic_render(void * opaque, const LG_RendererRect destRect, const uint8_t * data, bool resample)
{
  struct LGR_Basic * this = (struct LGR_Basic *)opaque;
  if (!this || !this->initialized)
    return false;

  int       pitch;
  uint8_t * dest;

  if (SDL_LockTexture(this->texture, NULL, (void**)&dest, &pitch) != 0)
  {
    DEBUG_ERROR("Failed to lock the texture for update");
    return false;
  }

  if (pitch == this->format.pitch)
    memcpySSE(dest, data, this->texSize);
  else
  {
    for(unsigned int y = 0; y < this->format.height; ++y)
    {
      memcpySSE(dest, data, this->dataWidth);
      dest += pitch;
      data += this->format.pitch;
    }
  }

  SDL_Rect rect;
  rect.x = destRect.x;
  rect.y = destRect.y;
  rect.w = destRect.w;
  rect.h = destRect.h;

  SDL_UnlockTexture(this->texture);
  SDL_RenderCopy(this->renderer, this->texture, NULL, &rect);
  SDL_RenderPresent(this->renderer);

  return true;
}

const LG_Renderer LGR_Basic =
{
  .get_name      = lgr_basic_get_name,
  .initialize    = lgr_basic_initialize,
  .deinitialize  = lgr_basic_deinitialize,
  .is_compatible = lgr_basic_is_compatible,
  .on_resize     = lgr_basic_on_resize,
  .render        = lgr_basic_render
};