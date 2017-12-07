#include <stdbool.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>

int main(int argc, char * argv[])
{
  SDL_Window    * window;
  SDL_Renderer  * renderer;

  TTF_Init();
  SDL_Init(SDL_INIT_VIDEO);
  window   = SDL_CreateWindow("sync-test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1000, 1040, SDL_WINDOW_RESIZABLE);
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

  SDL_Event       event;
  bool            running     = true;
  unsigned int    frameCount  = 0;
  Uint32          fpsFrame    = 0;
  Uint32          fpsStart    = SDL_GetTicks();  
  float           fps         = 0.0f;
  TTF_Font      * fpsFont     = NULL;
  SDL_Rect        fpsTextRect = {5, 5};
  SDL_Texture   * fpsText     = NULL;

  int width = 1000;
  int boxX  = 100;
  int boxY  = 100;

  fpsFont = TTF_OpenFont("C:\\Windows\\Fonts\\cour.ttf", 24);

  while (running)
  {
    while (SDL_PollEvent(&event))
      switch(event.type)
      {
        case SDL_QUIT:
          running = false;
          break;

        case SDL_KEYUP:
          switch(event.key.keysym.scancode)
          {
            case SDL_SCANCODE_ESCAPE:
              running = false;
              break;

            case SDL_SCANCODE_F11:
              SDL_SetWindowFullscreen(window, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
              break;
          }

        case SDL_WINDOWEVENT:
          switch (event.window.event)
          {
            case SDL_WINDOWEVENT_RESIZED:
              width = event.window.data1;
              boxX  = event.window.data1 / 10;
              boxY  = (event.window.data2 - 40) / 10;
              break;
          }
      }

    SDL_SetRenderDrawColor(renderer, 0x00, 0x00, 0x00, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);    

    SDL_SetRenderDrawColor(renderer, 0xff, 0xff, 0xff, SDL_ALPHA_OPAQUE);
    for (int y = 0; y < 10; ++y)
      for (int x = 0; x < 10; ++x)
      {
        const SDL_Rect rect = { x * boxX, y * boxY + 40, boxX, boxY};
        if (y * 10 + x == frameCount % 100)
          SDL_RenderFillRect(renderer, &rect);
         else
          SDL_RenderDrawRect(renderer, &rect);
      }

    if (SDL_GetTicks() - fpsStart > 1000)
    {
      const float delta         = (float)(SDL_GetTicks() - fpsStart) / 1000.0f;
      const unsigned int frames = frameCount - fpsFrame;
      fps = (float)frames / delta;

      fpsStart = SDL_GetTicks();
      fpsFrame = frameCount;
    }

    const SDL_Color c = {0x00, 0xff, 0x00, 0xff};
    char text[128];
    snprintf(text, sizeof(text), "FPS: %7.4f, Frame: %05u    \"F11\" to toggle Full Screen", fps, frameCount);
    SDL_Surface * fpsSurf = TTF_RenderText_Solid(fpsFont, text, c);
    if (fpsText)
      SDL_DestroyTexture(fpsText);
    fpsText = SDL_CreateTextureFromSurface(renderer, fpsSurf);
    fpsTextRect.x = width / 2 - fpsSurf->w / 2;
    fpsTextRect.y = 20 - fpsSurf->h / 2;
    fpsTextRect.w = fpsSurf->w;
    fpsTextRect.h = fpsSurf->h;
    SDL_FreeSurface(fpsSurf);
    SDL_RenderCopy(renderer, fpsText, NULL, &fpsTextRect);

    SDL_RenderPresent(renderer);
    ++frameCount;
  }

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  return 0;
}