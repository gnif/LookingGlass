/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#include <stdbool.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

typedef enum LG_ClipboardData
{
  LG_CLIPBOARD_DATA_TEXT = 0,
  LG_CLIPBOARD_DATA_PNG,
  LG_CLIPBOARD_DATA_BMP,
  LG_CLIPBOARD_DATA_TIFF,
  LG_CLIPBOARD_DATA_JPEG,

  LG_CLIPBOARD_DATA_NONE // enum max, not a data type
}
LG_ClipboardData;

typedef void (* LG_ClipboardReplyFn  )(void * opaque, const LG_ClipboardData type, uint8_t * data, uint32_t size);
typedef void (* LG_ClipboardRequestFn)(LG_ClipboardReplyFn replyFn, void * opaque);
typedef void (* LG_ClipboardReleaseFn)();
typedef void (* LG_ClipboardNotifyFn)(LG_ClipboardData type);
typedef void (* LG_ClipboardDataFn  )(const LG_ClipboardData type, uint8_t * data, size_t size);

typedef const char * (* LG_ClipboardGetName)();
typedef bool         (* LG_ClipboardInit)(SDL_SysWMinfo * wminfo, LG_ClipboardReleaseFn releaseFn, LG_ClipboardNotifyFn notifyFn, LG_ClipboardDataFn dataFn);
typedef void         (* LG_ClipboardFree)();
typedef void         (* LG_ClipboardWMEvent)(SDL_SysWMmsg * msg);
typedef void         (* LG_ClipboardNotice)(LG_ClipboardRequestFn requestFn, LG_ClipboardData type);
typedef void         (* LG_ClipboardRelease)();
typedef void         (* LG_ClipboardRequest)(LG_ClipboardData type);

typedef struct LG_Clipboard
{
  LG_ClipboardGetName getName;
  LG_ClipboardInit    init;
  LG_ClipboardFree    free;
  LG_ClipboardWMEvent wmevent;
  LG_ClipboardNotice  notice;
  LG_ClipboardRelease release;
  LG_ClipboardRequest request;
}
LG_Clipboard;