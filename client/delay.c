/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>

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

#include "delay.h"

#include <SDL2/SDL.h>

#include "debug.h"

static unsigned int delay_loop_count = 0;

void delay()
{
  unsigned char j;
  for(unsigned int i = 0; i < delay_loop_count; ++i)
  {
    j = 0;
    while(--j)
      asm("");
  }
}

void delay_calibrate()
{
  // initialize the poll limit to a sane value
  for(int start = SDL_GetTicks(); SDL_GetTicks() - start < 10; ++delay_loop_count) {}
  DEBUG_INFO("Init : %u", delay_loop_count);

  // the following loop must produce 20 concurrent valid results to ensure a valid calculation
  int okCount = 0;
  while(okCount < 20)
  {
    // time the final loop and calculate the remaining time
    unsigned int ticks = SDL_GetTicks();
    delay();
    int remain = 10 - (SDL_GetTicks() - ticks);

    // if the remaining time is within 2ms, accept it (Linux is not a RTOS)
    if (remain > -2 && remain < 2)
    {
      ++okCount;
      continue;
    }

    // the delay is out of spec, adjust the limit
    okCount = 0;
    DEBUG_INFO("Diff : %c%d ms", remain > 0 ? '-' : '+', abs(remain));
    delay_loop_count += ceil(((double)delay_loop_count / 10.0) * remain);
  }

  DEBUG_INFO("Final: %u", delay_loop_count);

  // scale the limit for a 1ms delay
  delay_loop_count = ceil((double)delay_loop_count / 10.0);
}