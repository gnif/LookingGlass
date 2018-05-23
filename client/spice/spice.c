/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
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

#include "spice.h"
#include "utils.h"
#include "debug.h"

#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <spice/protocol.h>
#include <spice/error_codes.h>

#include "messages.h"
#include "rsa.h"

#ifdef DEBUG_SPICE_MOUSE
  #define DEBUG_MOUSE(fmt, args...) DEBUG_PRINT("[M]", fmt, ##args)
#else
  #define DEBUG_MOUSE(fmt, args...) do {} while(0)
#endif

#ifdef DEBUG_SPICE_KEYBOARD
  #define DEBUG_KEYBOARD(fmt, args...) DEBUG_PRINT("[K]", fmt, ##args)
#else
  #define DEBUG_KEYBOARD(fmt, args...) do {} while(0)
#endif


// ============================================================================

// internal structures
struct SpiceChannel
{
  bool     connected;
  bool     initDone;
  uint8_t  channelType;
  int      socket;
  uint32_t ackFrequency;
  uint32_t ackCount;
  uint32_t serial;
  LG_Lock  lock;
};

struct SpiceKeyboard
{
  uint32_t modifiers;
};

#define SPICE_MOUSE_QUEUE_SIZE 64

struct SpiceMouse
{
  uint32_t buttonState;

  int                  sentCount;
  SpiceMsgcMouseMotion queue[SPICE_MOUSE_QUEUE_SIZE];
  int                  rpos, wpos;
  int                  queueLen;
  LG_Lock              lock;
};

union SpiceAddr
{
  struct sockaddr     addr;
  struct sockaddr_in  in;
  struct sockaddr_in6 in6;
  struct sockaddr_un  un;
};

struct Spice
{
  char            password[32];
  short           family;
  union SpiceAddr addr;

  uint32_t sessionID;
  uint32_t channelID;
  struct   SpiceChannel scMain;
  struct   SpiceChannel scInputs;

  struct SpiceKeyboard kb;
  struct SpiceMouse    mouse;
};

// globals
struct Spice spice =
{
  .sessionID            = 0,
  .scMain  .connected   = false,
  .scMain  .channelType = SPICE_CHANNEL_MAIN,
  .scInputs.connected   = false,
  .scInputs.channelType = SPICE_CHANNEL_INPUTS,
};

// internal forward decls
bool spice_connect_channel   (struct SpiceChannel * channel);
void spice_disconnect_channel(struct SpiceChannel * channel);

bool spice_process_ack(struct SpiceChannel * channel);

bool spice_on_common_read        (struct SpiceChannel * channel, SpiceDataHeader * header, bool * handled);
bool spice_on_main_channel_read  ();
bool spice_on_inputs_channel_read();

bool    spice_read     (const struct SpiceChannel * channel,       void * buffer, const ssize_t size);
ssize_t spice_write    (const struct SpiceChannel * channel, const void * buffer, const ssize_t size);
bool    spice_write_msg(struct SpiceChannel * channel, uint32_t type, const void * buffer, const ssize_t size);
bool    spice_discard  (const struct SpiceChannel * channel, ssize_t size);


// ============================================================================

bool spice_connect(const char * host, const short port, const char * password)
{
  strncpy(spice.password, password, sizeof(spice.password) - 1);
  memset(&spice.addr, 0, sizeof(spice.addr));

  if (port == 0)
  {
    spice.family = AF_UNIX;
    spice.addr.un.sun_family = spice.family;
    strncpy(spice.addr.un.sun_path, host, sizeof(spice.addr.un.sun_path) - 1);
    DEBUG_INFO("Remote: %s", host);
  }
  else
  {
    spice.family = AF_INET;
    inet_pton(spice.family, host, &spice.addr.in.sin_addr);
    spice.addr.in.sin_family = spice.family;
    spice.addr.in.sin_port   = htons(port);
    DEBUG_INFO("Remote: %s:%d", host, port);
  }

  LG_LOCK_INIT(spice.mouse.lock);

  spice.channelID = 0;
  if (!spice_connect_channel(&spice.scMain))
  {
    DEBUG_ERROR("connect main channel failed");
    return false;
  }

  return true;
}

// ============================================================================

void spice_disconnect()
{
  spice_disconnect_channel(&spice.scMain  );
  spice_disconnect_channel(&spice.scInputs);

  LG_LOCK_FREE(spice.mouse.lock);

  spice.sessionID = 0;
}

// ============================================================================

bool spice_ready()
{
  return spice.scMain.connected &&
         spice.scInputs.connected;
}

// ============================================================================

bool spice_process()
{
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(spice.scMain.socket  , &readSet);
  FD_SET(spice.scInputs.socket, &readSet);

  struct timeval timeout;
  timeout.tv_sec  = 1;
  timeout.tv_usec = 0;

  int rc = select(FD_SETSIZE, &readSet, NULL, NULL, &timeout);
  if (rc < 0)
  {
    DEBUG_ERROR("select failure");
    return false;
  }

  for(int i = 0; i < FD_SETSIZE; ++i)
    if (FD_ISSET(i, &readSet))
    {
      if (i == spice.scMain.socket)
      {
        if (spice_on_main_channel_read())
        {
          if (spice.scMain.connected && !spice_process_ack(&spice.scMain))
          {
            DEBUG_ERROR("failed to process ack on main channel");
            return false;
          }
          continue;
        }
        else
        {
          DEBUG_ERROR("failed to perform read on main channel");
          return false;
        }
      }

      if (spice.scInputs.connected && i == spice.scInputs.socket)
      {
        if (!spice_process_ack(&spice.scInputs))
        {
          DEBUG_ERROR("failed to process ack on inputs channel");
          return false;
        }

        if (spice_on_inputs_channel_read())
          continue;
        else
        {
          DEBUG_ERROR("failed to perform read on inputs channel");
          return false;
        }
      }
    }

  return true;
}

// ============================================================================

bool spice_process_ack(struct SpiceChannel * channel)
{
  if (channel->ackFrequency == 0)
    return true;

  if (channel->ackCount++ != channel->ackFrequency)
    return true;

  channel->ackCount = 0;
  return spice_write_msg(channel, SPICE_MSGC_ACK, "\0", 1);
}

// ============================================================================

bool spice_on_common_read(struct SpiceChannel * channel, SpiceDataHeader * header, bool * handled)
{
  if (!spice_read(channel, header, sizeof(SpiceDataHeader)))
  {
    DEBUG_ERROR("read failure");
    *handled = false;
    return false;
  }

#if 0
  printf("socket: %d, serial: %6u, type: %2u, size %6u, sub_list %4u\n",
      channel->socket,
      header->serial, header->type, header->size, header->sub_list);
#endif

  if (!channel->initDone)
  {
    *handled = false;

    return true;
  }
  switch(header->type)
  {
    case SPICE_MSG_MIGRATE:
    case SPICE_MSG_MIGRATE_DATA:
    {
      DEBUG_PROTO("SPICE_MSG_MIGRATE_DATA");

      *handled = true;
      DEBUG_WARN("migration is not supported");
      return false;
    }

    case SPICE_MSG_SET_ACK:
    {
      DEBUG_INFO("SPICE_MSG_SET_ACK");
      *handled = true;

      SpiceMsgSetAck in;
      if (!spice_read(channel, &in, sizeof(in)))
        return false;

      channel->ackFrequency = in.window;

      SpiceMsgcAckSync out;
      out.generation = in.generation;
      if (!spice_write_msg(channel, SPICE_MSGC_ACK_SYNC, &out, sizeof(out)))
        return false;

      return true;
    }

    case SPICE_MSG_PING:
    {
      DEBUG_PROTO("SPICE_MSG_PING");
      *handled = true;

      SpiceMsgPing in;
      if (!spice_read(channel, &in, sizeof(in)))
        return false;

      if (!spice_discard(channel, header->size - sizeof(in)))
      {
        DEBUG_ERROR("failed discarding enough bytes from the ping packet");
        return false;
      }

      SpiceMsgcPong out;
      out.id        = in.id;
      out.timestamp = in.timestamp;
      if (!spice_write_msg(channel, SPICE_MSGC_PONG, &out, sizeof(out)))
        return false;

      return true;
    }

    case SPICE_MSG_WAIT_FOR_CHANNELS:
    case SPICE_MSG_DISCONNECTING    :
    {
      *handled = true;
      DEBUG_FIXME("ignored wait-for-channels or disconnect message");
      return false;
    }

    case SPICE_MSG_NOTIFY:
    {
      DEBUG_PROTO("SPICE_MSG_NOTIFY");
      SpiceMsgNotify in;
      if (!spice_read(channel, &in, sizeof(in)))
        return false;

      char msg[in.message_len+1];
      if (!spice_read(channel, msg, in.message_len+1))
        return false;

      DEBUG_INFO("notify message: %s", msg);
      *handled = true;
      return true;
    }
  }

  *handled = false;
  return true;
}

// ============================================================================

bool spice_on_main_channel_read()
{
  struct SpiceChannel *channel = &spice.scMain;

  SpiceDataHeader header;
  bool handled;

  if (!spice_on_common_read(channel, &header, &handled))
  {
    DEBUG_ERROR("read failure");
    return false;
  }

  if (handled)
    return true;

  if (!channel->initDone)
  {
    if (header.type != SPICE_MSG_MAIN_INIT)
    {
      spice_disconnect();
      DEBUG_ERROR("expected main init message but got type %u", header.type);
      return false;
    }

    DEBUG_PROTO("SPICE_MSG_MAIN_INIT");

    channel->initDone = true;
    SpiceMsgMainInit msg;
    if (!spice_read(channel, &msg, sizeof(msg)))
    {
      spice_disconnect();
      return false;
    }

    spice.sessionID = msg.session_id;
    if (!spice_connect_channel(&spice.scInputs))
    {
      DEBUG_ERROR("failed to connect inputs channel");
      return false;
    }

    if (msg.current_mouse_mode != SPICE_MOUSE_MODE_CLIENT && !spice_mouse_mode(false))
    {
      DEBUG_ERROR("failed to set mouse mode");
      return false;
    }

    return true;
  }

  DEBUG_WARN("main channel unhandled message type %u", header.type);
  spice_discard(channel, header.size);
  return true;
}

// ============================================================================

bool spice_on_inputs_channel_read()
{
  struct SpiceChannel *channel = &spice.scInputs;

  SpiceDataHeader header;
  bool handled;

  if (!spice_on_common_read(channel, &header, &handled))
  {
    DEBUG_ERROR("read failure");
    return false;
  }

  if (handled)
    return true;

  switch(header.type)
  {
    case SPICE_MSG_INPUTS_INIT:
    {
      DEBUG_PROTO("SPICE_MSG_INPUTS_INIT");

      if (channel->initDone)
      {
        DEBUG_ERROR("input init message already done");
        return false;
      }

      channel->initDone = true;

      SpiceMsgInputsInit in;
      if (!spice_read(channel, &in, sizeof(in)))
        return false;

      return true;
    }

    case SPICE_MSG_INPUTS_KEY_MODIFIERS:
    {
      DEBUG_PROTO("SPICE_MSG_INPUTS_KEY_MODIFIERS");
      SpiceMsgInputsInit in;
      if (!spice_read(channel, &in, sizeof(in)))
        return false;

      spice.kb.modifiers = in.modifiers;
      return true;
    }

    case SPICE_MSG_INPUTS_MOUSE_MOTION_ACK:
    {
      DEBUG_PROTO("SPICE_MSG_INPUTS_MOUSE_MOTION_ACK");
      int sent = 0;
      LG_LOCK(spice.mouse.lock);
      while(spice.mouse.queueLen && sent < 4)
      {
        SpiceMsgcMouseMotion *msg = &spice.mouse.queue[spice.mouse.rpos];
        if (!spice_write_msg(channel, SPICE_MSGC_INPUTS_MOUSE_MOTION, msg, sizeof(SpiceMsgcMouseMotion)))
        {
          DEBUG_ERROR("failed to send post ack");
          spice.mouse.sentCount = sent;
          LG_UNLOCK(spice.mouse.lock);
          return false;
        }

        if (++spice.mouse.rpos == SPICE_MOUSE_QUEUE_SIZE)
          spice.mouse.rpos = 0;

        ++sent;
        --spice.mouse.queueLen;
      }

      spice.mouse.sentCount = sent;
      LG_UNLOCK(spice.mouse.lock);
      return true;
    }
  }

  DEBUG_WARN("inputs channel unhandled message type %u", header.type);
  spice_discard(channel, header.size);
  return true;
}

// ============================================================================

bool spice_connect_channel(struct SpiceChannel * channel)
{
  channel->initDone     = false;
  channel->ackFrequency = 0;
  channel->ackCount     = 0;
  channel->serial       = 0;

  LG_LOCK_INIT(channel->lock);

  size_t addrSize;
  switch(spice.family)
  {
    case AF_UNIX:
      addrSize = sizeof(spice.addr.un);
      break;

    case AF_INET:
      addrSize = sizeof(spice.addr.in);
      break;

    case AF_INET6:
      addrSize = sizeof(spice.addr.in6);
      break;

    default:
      DEBUG_ERROR("Unsupported socket family");
      return false;
  }

  channel->socket = socket(spice.family, SOCK_STREAM, 0);
  if (channel->socket == -1)
    return false;

  if (spice.family != AF_UNIX)
  {
    int flag = 1;
    setsockopt(channel->socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int));
  }

  if (connect(channel->socket, &spice.addr.addr, addrSize) == -1)
  {
    DEBUG_ERROR("socket connect failure");
    close(channel->socket);
    return false;
  }
  channel->connected = true;

  SpiceLinkHeader header =
  {
    .magic         = SPICE_MAGIC        ,
    .major_version = SPICE_VERSION_MAJOR,
    .minor_version = SPICE_VERSION_MINOR,
    .size          = sizeof(SpiceLinkMess)
  };

  SpiceLinkMess message =
  {
    .connection_id    = spice.sessionID,
    .channel_type     = channel->channelType,
    .channel_id       = spice.channelID,
    .num_common_caps  = 0,
    .num_channel_caps = 0,
    .caps_offset      = sizeof(SpiceLinkMess)
  };

  spice_write(channel, &header , sizeof(header ));
  spice_write(channel, &message, sizeof(message));

  if (!spice_read(channel, &header, sizeof(header)))
  {
    DEBUG_ERROR("failed to read SpiceLinkHeader");
    spice_disconnect_channel(channel);
    return false;
  }

  if (header.magic != SPICE_MAGIC || header.major_version != SPICE_VERSION_MAJOR)
  {
    DEBUG_ERROR("invalid or unsupported protocol version");
    spice_disconnect_channel(channel);
    return false;
  }

  if (header.size < sizeof(SpiceLinkReply))
  {
    DEBUG_ERROR("reported data size too small");
    spice_disconnect_channel(channel);
    return false;
  }

  SpiceLinkReply reply;
  if (!spice_read(channel, &reply, sizeof(reply)))
  {
    DEBUG_ERROR("failed to read SpiceLinkReply");
    spice_disconnect_channel(channel);
    return false;
  }

  if (reply.error != SPICEC_ERROR_CODE_SUCCESS)
  {
    DEBUG_ERROR("server replied with error %u", reply.error);
    spice_disconnect_channel(channel);
    return false;
  }

  uint32_t capsCommon [reply.num_common_caps ];
  uint32_t capsChannel[reply.num_channel_caps];
  spice_read(channel, &capsCommon , sizeof(capsCommon ));
  spice_read(channel, &capsChannel, sizeof(capsChannel));

  struct spice_password pass;
  if (!spice_rsa_encrypt_password(reply.pub_key, spice.password, &pass))
  {
    spice_disconnect_channel(channel);
    return false;
  }

  if (!spice_write(channel, pass.data, pass.size))
  {
    spice_rsa_free_password(&pass);
    DEBUG_ERROR("failed to write encrypted data");
    spice_disconnect_channel(channel);
    return false;
  }

  spice_rsa_free_password(&pass);

  uint32_t linkResult;
  if (!spice_read(channel, &linkResult, sizeof(linkResult)))
  {
    DEBUG_ERROR("failed to read SpiceLinkResult");
    spice_disconnect_channel(channel);
    return false;
  }

  if (linkResult != SPICE_LINK_ERR_OK)
  {
    DEBUG_ERROR("connect code error %u", linkResult);
    spice_disconnect_channel(channel);
    return false;
  }

  return true;
}

// ============================================================================

void spice_disconnect_channel(struct SpiceChannel * channel)
{
  if (channel->connected)
  {
    shutdown(channel->socket, SHUT_WR);

    char buffer[1024];
    ssize_t len = 0;
    do
      len = read(channel->socket, buffer, sizeof(buffer));
    while(len > 0);

    close(channel->socket);
  }
  channel->connected = false;
  LG_LOCK_FREE(channel->lock);
}

// ============================================================================

ssize_t spice_write(const struct SpiceChannel * channel, const void * buffer, const ssize_t size)
{
  if (!channel->connected)
  {
    DEBUG_ERROR("not connected");
    return -1;
  }

  if (!buffer)
  {
    DEBUG_ERROR("invalid buffer argument supplied");
    return -1;
  }

  ssize_t len = send(channel->socket, buffer, size, 0);
  if (len != size)
    DEBUG_WARN("incomplete write");

  return len;
}

// ============================================================================

bool spice_write_msg(struct SpiceChannel * channel, uint32_t type, const void * buffer, const ssize_t size)
{
  LG_LOCK(channel->lock);

  SpiceDataHeader header;
  header.serial   = channel->serial++;
  header.type     = type;
  header.size     = size;
  header.sub_list = 0;

  if (spice_write(channel, &header, sizeof(header)) != sizeof(header))
  {
    DEBUG_ERROR("failed to write message header");
    LG_UNLOCK(channel->lock);
    return false;
  }

  if (spice_write(channel, buffer, size) != size)
  {
    DEBUG_ERROR("failed to write message body");
    LG_UNLOCK(channel->lock);
    return false;
  }

  LG_UNLOCK(channel->lock);
  return true;
}

// ============================================================================

bool spice_read(const struct SpiceChannel * channel, void * buffer, const ssize_t size)
{
  if (!channel->connected)
  {
    DEBUG_ERROR("not connected");
    return false;
  }

  if (!buffer)
  {
    DEBUG_ERROR("invalid buffer argument supplied");
    return false;
  }

  ssize_t len = read(channel->socket, buffer, size);
  if (len != size)
  {
    DEBUG_ERROR("incomplete write");
    return false;
  }

  return true;
}

// ============================================================================

bool spice_discard(const struct SpiceChannel * channel, ssize_t size)
{
  while(size)
  {
    char c[8192];
    size_t len = read(channel->socket, c, size > sizeof(c) ? sizeof(c) : size);
    if (len <= 0)
      return false;

    size -= len;
  }
  return true;
}

// ============================================================================

bool spice_key_down(uint32_t code)
{
  DEBUG_KEYBOARD("%u", code);
  if (!spice.scInputs.connected)
  {
    DEBUG_ERROR("not connected");
    return false;
  }

  if (code > 0x100)
    code = 0xe0 | ((code - 0x100) << 8);

  SpiceMsgcKeyDown msg;
  msg.code = code;

  return spice_write_msg(&spice.scInputs, SPICE_MSGC_INPUTS_KEY_DOWN, &msg, sizeof(msg));
}

// ============================================================================

bool spice_key_up(uint32_t code)
{
  DEBUG_KEYBOARD("%u", code);
  if (!spice.scInputs.connected)
  {
    DEBUG_ERROR("not connected");
    return false;
  }

  if (code < 0x100)
    code |= 0x80;
  else
    code = 0x80e0 | ((code - 0x100) << 8);

  SpiceMsgcKeyDown msg;
  msg.code = code;

  return spice_write_msg(&spice.scInputs, SPICE_MSGC_INPUTS_KEY_UP, &msg, sizeof(msg));
}

// ============================================================================

bool spice_mouse_mode(bool server)
{
  DEBUG_MOUSE("%s", server ? "server" : "client");
  if (!spice.scInputs.connected)
  {
    DEBUG_ERROR("not connected");
    return false;
  }

  SpiceMsgcMainMouseModeRequest msg;
  msg.mouse_mode = server ? SPICE_MOUSE_MODE_SERVER : SPICE_MOUSE_MODE_CLIENT;

  return spice_write_msg(&spice.scMain, SPICE_MSGC_MAIN_MOUSE_MODE_REQUEST, &msg, sizeof(msg));
}

// ============================================================================

bool spice_mouse_position(uint32_t x, uint32_t y)
{
  DEBUG_MOUSE("x=%u, y=%u", x, y);
  if (!spice.scInputs.connected)
  {
    DEBUG_ERROR("not connected");
    return false;
  }

  SpiceMsgcMousePosition msg;
  msg.x            = x;
  msg.y            = y;
  msg.button_state = spice.mouse.buttonState;
  msg.display_id   = 0;

  return spice_write_msg(&spice.scInputs, SPICE_MSGC_INPUTS_MOUSE_POSITION, &msg, sizeof(msg));
}

// ============================================================================

bool spice_mouse_motion(int32_t x, int32_t y)
{
  DEBUG_MOUSE("x=%d, y=%d", x, y);
  if (!spice.scInputs.connected)
  {
    DEBUG_ERROR("not connected");
    return false;
  }

  LG_LOCK(spice.mouse.lock);
  if (spice.mouse.sentCount == 4)
  {
    if (spice.mouse.queueLen == SPICE_MOUSE_QUEUE_SIZE)
    {
      DEBUG_ERROR("mouse motion ringbuffer full!");
      LG_UNLOCK(spice.mouse.lock);
      return false;
    }

    SpiceMsgcMouseMotion *msg =
      &spice.mouse.queue[spice.mouse.wpos++];
    msg->x            = x;
    msg->y            = y;
    msg->button_state = spice.mouse.buttonState;

    if (spice.mouse.wpos == SPICE_MOUSE_QUEUE_SIZE)
      spice.mouse.wpos = 0;

    ++spice.mouse.queueLen;
    LG_UNLOCK(spice.mouse.lock);
    return true;
  }

  SpiceMsgcMouseMotion msg;
  msg.x            = x;
  msg.y            = y;
  msg.button_state = spice.mouse.buttonState;

  ++spice.mouse.sentCount;
  LG_UNLOCK(spice.mouse.lock);
  return spice_write_msg(&spice.scInputs, SPICE_MSGC_INPUTS_MOUSE_MOTION, &msg, sizeof(msg));
}

// ============================================================================

bool spice_mouse_press(uint32_t button)
{
  DEBUG_MOUSE("%u", button);
  if (!spice.scInputs.connected)
  {
    DEBUG_ERROR("not connected");
    return false;
  }

  switch(button)
  {
    case SPICE_MOUSE_BUTTON_LEFT  : spice.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_LEFT  ; break;
    case SPICE_MOUSE_BUTTON_MIDDLE: spice.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_MIDDLE; break;
    case SPICE_MOUSE_BUTTON_RIGHT : spice.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_RIGHT ; break;
  }

  SpiceMsgcMousePress msg;
  msg.button       = button;
  msg.button_state = spice.mouse.buttonState;

  return spice_write_msg(&spice.scInputs, SPICE_MSGC_INPUTS_MOUSE_PRESS, &msg, sizeof(msg));
}

// ============================================================================

bool spice_mouse_release(uint32_t button)
{
  DEBUG_MOUSE("%u", button);
  if (!spice.scInputs.connected)
  {
    DEBUG_ERROR("not connected");
    return false;
  }

  switch(button)
  {
    case SPICE_MOUSE_BUTTON_LEFT  : spice.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_LEFT  ; break;
    case SPICE_MOUSE_BUTTON_MIDDLE: spice.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_MIDDLE; break;
    case SPICE_MOUSE_BUTTON_RIGHT : spice.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_RIGHT ; break;
  }

  SpiceMsgcMouseRelease msg;
  msg.button       = button;
  msg.button_state = spice.mouse.buttonState;

  return spice_write_msg(&spice.scInputs, SPICE_MSGC_INPUTS_MOUSE_RELEASE, &msg, sizeof(msg));
}