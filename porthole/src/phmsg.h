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
  uint32_t id;    // the ID of the FD
} __attribute__ ((packed)) PHMsgFd;

typedef struct {
  uint32_t fd_id; // the ID of the FD for this segment
  uint32_t size;  // the size of this segment
  uint64_t addr;  // the base address of this segment
} __attribute__ ((packed)) PHMsgSegment;

typedef struct {
  uint32_t type; // the application defined type
  uint32_t id;   // the ID of the new mapping
} __attribute__ ((packed)) PHMsgFinish;

typedef struct {
  uint32_t id;   // the mapping ID
} __attribute__ ((packed)) PHMsgUnmap;

typedef struct {
  uint32_t msg;
  union
  {
    PHMsgFd      fd;
    PHMsgSegment segment;
    PHMsgFinish  finish;
    PHMsgUnmap   unmap;
  } u;
} __attribute__ ((packed)) PHMsg;

#define PH_MSG_MAP     0x1 // start of a map sequence
#define PH_MSG_FD      0x2 // file descriptor
#define PH_MSG_SEGMENT 0x3 // map segment
#define PH_MSG_FINISH  0x4 // finish of map sequence
#define PH_MSG_UNMAP   0x5 // unmap a previous map

#define PH_MSG_MAP_SIZE     (sizeof(uint32_t))
#define PH_MSG_FD_SIZE      (sizeof(uint32_t) + sizeof(PHMsgFd))
#define PH_MSG_SEGMENT_SIZE (sizeof(uint32_t) + sizeof(PHMsgSegment))
#define PH_MSG_FINISH_SIZE  (sizeof(uint32_t) + sizeof(PHMsgFinish))
#define PH_MSG_UNMAP_SIZE   (sizeof(uint32_t) + sizeof(PHMsgUnmap))