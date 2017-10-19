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

#pragma pack(pop)