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

#include <stdint.h>

typedef struct {
  uint64_t id;
} __attribute__ ((packed)) MsgFd;

typedef struct {
  uint64_t fd_id;
  uint64_t addr;
  uint32_t size;
} __attribute__ ((packed)) MsgSegment;

typedef struct {
  uint32_t type;
} __attribute__ ((packed)) MsgFinish;

typedef struct {
  uint32_t msg;
  union
  {
    MsgFd      fd;
    MsgSegment segment;
    MsgFinish  finish;
  } u;
} __attribute__ ((packed)) Msg;

#define INTRO_MSG_RESET   0x1
#define INTRO_MSG_FD      0x2
#define INTRO_MSG_SEGMENT 0x3
#define INTRO_MSG_FINISH  0x4

#define INTRO_MSG_RESET_SIZE   (sizeof(uint32_t))
#define INTRO_MSG_FD_SIZE      (sizeof(uint32_t) + sizeof(MsgFd))
#define INTRO_MSG_SEGMENT_SIZE (sizeof(uint32_t) + sizeof(MsgSegment))
#define INTRO_MSG_FINISH_SIZE  (sizeof(uint32_t) + sizeof(MsgFinish))