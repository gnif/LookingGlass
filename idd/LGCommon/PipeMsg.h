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

#pragma once

#include <stdint.h>

static constexpr wchar_t LG_PIPE_NAME[] = L"\\\\.\\pipe\\LookingGlassIDD";

#pragma pack(push, 4)
struct LGPipeMsg
{
  unsigned size;
  enum Type : uint32_t
  {
    SETCURSORPOS,
    SETDISPLAYMODE,
    GPUSTATUS,
    RELOADSETTINGS,
    RESOLUTIONREJECTED,
    SET_RECOVERY,
    RECOVERY_OFF,
    RECOVERY_ON,
    RECOVERY_FAILED,
    RECOVERY_NO_DISPLAY,
    CLIPBOARD_SETUP,
    CLIPBOARD_READY,
    CLIPBOARD_KICK,
    CLIPBOARD_RESET,
    HELLO
  }
  type;

  enum : uint32_t
  {
    RECOVERY_ACTIVE  = 0x1U,
    PROTOCOL_VERSION = 2U
  };

  union
  {
    struct
    {
      int32_t x;
      int32_t y;
    }
    curorPos;

    struct
    {
      uint32_t width;
      uint32_t height;
      uint32_t refreshMilliHz;
    }
    displayMode;

    struct
    {
      bool software;
    }
    gpuStatus;

    struct
    {
      uint32_t width;
      uint32_t height;
      uint32_t requiredSizeMiB;
    }
    resolutionRejected;

    struct
    {
      uint64_t session;
      uint32_t request;
    }
    recovery;

    struct
    {
      // Legacy setup carries a handle already duplicated into the receiver.
      uint64_t handle;
      uint32_t bytes;
    }
    clipboardSetup;

    struct
    {
      uint64_t epoch;
      uint32_t status;
    }
    clipboardReady;

    struct
    {
      uint64_t epoch;
      uint32_t rings;
    }
    clipboardKick;

    struct
    {
      uint64_t epoch;
      uint32_t reason;
    }
    clipboardReset;

    struct
    {
      uint32_t version;
      uint64_t authorityId[2];
    }
    hello;
  };
};
#pragma pack(pop)

static_assert(sizeof(LGPipeMsg) == 28, "LGPipeMsg wire layout changed");
