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

#pragma pack(push,1)

typedef struct SpicePoint16
{
  int16_t x, y;
}
SpicePoint16;

typedef struct SpiceMsgMainInit
{
  uint32_t session_id;
  uint32_t display_channels_hint;
  uint32_t supported_mouse_modes;
  uint32_t current_mouse_mode;
  uint32_t agent_connected;
  uint32_t agent_tokens;
  uint32_t multi_media_time;
  uint32_t ram_hint;
}
SpiceMsgMainInit;

typedef struct SpiceChannelID
{
  uint8_t type;
  uint8_t channel_id;
}
SpiceChannelID;

typedef struct SpiceMsgMainChannelsList
{
  uint32_t num_of_channels;
  //SpiceChannelID channels[num_of_channels]
}
SpiceMainChannelsList;

typedef struct SpiceMsgcMainMouseModeRequest
{
  uint16_t mouse_mode;
}
SpiceMsgcMainMouseModeRequest;

typedef struct SpiceMsgPing
{
  uint32_t id;
  uint64_t timestamp;
}
SpiceMsgPing,
SpiceMsgcPong;

typedef struct SpiceMsgSetAck
{
  uint32_t generation;
  uint32_t window;
}
SpiceMsgSetAck;

typedef struct SpiceMsgcAckSync
{
  uint32_t generation;
}
SpiceMsgcAckSync;

typedef struct SpiceMsgNotify
{
  uint64_t time_stamp;
  uint32_t severity;
  uint32_t visibility;
  uint32_t what;
  uint32_t message_len;
  //char message[message_len+1]
}
SpiceMsgNotify;

typedef struct SpiceMsgInputsInit
{
  uint16_t modifiers;
}
SpiceMsgInputsInit,
SpiceMsgInputsKeyModifiers,
SpiceMsgcInputsKeyModifiers;

typedef struct SpiceMsgcKeyDown
{
  uint32_t code;
}
SpiceMsgcKeyDown,
SpiceMsgcKeyUp;

typedef struct SpiceMsgcMousePosition
{
  uint32_t x;
  uint32_t y;
  uint16_t button_state;
  uint8_t  display_id;
}
SpiceMsgcMousePosition;

typedef struct SpiceMsgcMouseMotion
{
  int32_t  x;
  int32_t  y;
  uint16_t button_state;
}
SpiceMsgcMouseMotion;

typedef struct SpiceMsgcMousePress
{
  uint8_t  button;
  uint16_t button_state;
}
SpiceMsgcMousePress,
SpiceMsgcMouseRelease;


// spice is missing these defines, the offical reference library incorrectly uses the VD defines
#define COMMON_CAPS_BYTES (((SPICE_COMMON_CAP_MINI_HEADER + 32) / 8) & ~3)
#define COMMON_SET_CAPABILITY(caps, index) \
    { (caps)[(index) / 32] |= (1 << ((index) % 32)); }

#define MAIN_CAPS_BYTES (((SPICE_MAIN_CAP_SEAMLESS_MIGRATE + 32) / 8) & ~3)
#define MAIN_SET_CAPABILITY(caps, index) \
    { (caps)[(index) / 32] |= (1 << ((index) % 32)); }


#pragma pack(pop)