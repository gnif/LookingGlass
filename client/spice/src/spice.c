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

#include "spice.h"
#include "utils.h"
#include "common/debug.h"

#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <spice/protocol.h>
#include <spice/vd_agent.h>

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

#ifdef DEBUG_SPICE_CLIPBOARD
  #define DEBUG_CLIPBOARD(fmt, args...) DEBUG_PRINT("[C]", fmt, ##args)
#else
  #define DEBUG_CLIPBOARD(fmt, args...) do {} while(0)
#endif

// we don't really need flow control because we are all local
// instead do what the spice-gtk library does and provide the largest
// possible number
#define SPICE_AGENT_TOKENS_MAX ~0

// ============================================================================

// internal structures
struct SpiceChannel
{
  bool     connected;
  bool     ready;
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

struct SpiceMouse
{
  uint32_t buttonState;

  int                  sentCount;
  int                  rpos, wpos;
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

  bool     hasAgent;
  uint32_t serverTokens;
  uint32_t clientTokens;
  uint32_t sessionID;
  uint32_t channelID;
  struct   SpiceChannel scMain;
  struct   SpiceChannel scInputs;

  struct SpiceKeyboard kb;
  struct SpiceMouse    mouse;

  bool cbSupported;
  bool cbSelection;

  // clipboard variables
  bool                  cbAgentGrabbed;
  bool                  cbClientGrabbed;
  SpiceDataType         cbType;
  uint8_t *             cbBuffer;
  uint32_t              cbRemain;
  uint32_t              cbSize;
  SpiceClipboardNotice  cbNoticeFn;
  SpiceClipboardData    cbDataFn;
  SpiceClipboardRelease cbReleaseFn;
  SpiceClipboardRequest cbRequestFn;
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

bool spice_on_common_read        (struct SpiceChannel * channel, SpiceMiniDataHeader * header, bool * handled);
bool spice_on_main_channel_read  ();
bool spice_on_inputs_channel_read();

bool spice_agent_process  (uint32_t dataSize);
bool spice_agent_connect  ();
bool spice_agent_send_caps(bool request);
void spice_agent_on_clipboard();

// utility functions
static uint32_t spice_type_to_agent_type(SpiceDataType type);
static SpiceDataType agent_type_to_spice_type(uint32_t type);

// thread safe read/write methods
bool spice_write_msg       (struct SpiceChannel * channel, uint32_t type, const void * buffer, const ssize_t size);
bool spice_agent_write_msg (uint32_t type, const void * buffer, ssize_t size);

// non thread safe read/write methods (nl = non-locking)
bool    spice_read_nl     (const struct SpiceChannel * channel, void * buffer, const ssize_t size);
ssize_t spice_write_nl    (const struct SpiceChannel * channel, const void * buffer, const ssize_t size);
bool    spice_discard_nl  (const struct SpiceChannel * channel, ssize_t size);
bool    spice_write_msg_nl(      struct SpiceChannel * channel, uint32_t type, const void * buffer, const ssize_t size, const ssize_t extra);

// ============================================================================

bool spice_connect(const char * host, const unsigned short port, const char * password)
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
    DEBUG_INFO("Remote: %s:%u", host, port);
  }

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

  spice.sessionID = 0;

  if (spice.cbBuffer)
    free(spice.cbBuffer);
  spice.cbBuffer = NULL;
  spice.cbRemain = 0;
  spice.cbSize   = 0;

  spice.cbAgentGrabbed  = false;
  spice.cbClientGrabbed = false;
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

bool spice_on_common_read(struct SpiceChannel * channel, SpiceMiniDataHeader * header, bool * handled)
{
  *handled = false;
  if (!spice_read_nl(channel, header, sizeof(SpiceMiniDataHeader)))
  {
    DEBUG_ERROR("read failure");
    return false;
  }

//#if 0
  DEBUG_PROTO("socket: %d, type: %2u, size %6u",
      channel->socket, header->type, header->size);
//#endif

  if (!channel->initDone)
    return true;

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
      if (!spice_read_nl(channel, &in, sizeof(in)))
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
      if (!spice_read_nl(channel, &in, sizeof(in)))
        return false;

      const int discard = header->size - sizeof(in);
      if (!spice_discard_nl(channel, discard))
      {
        DEBUG_ERROR("failed discarding enough bytes (%d) from the ping packet", discard);
        return false;
      }
      else
        DEBUG_PROTO("discard %d", discard);

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
      if (!spice_read_nl(channel, &in, sizeof(in)))
        return false;

      char msg[in.message_len+1];
      if (!spice_read_nl(channel, msg, in.message_len+1))
        return false;

      DEBUG_INFO("notify message: %s", msg);
      *handled = true;
      return true;
    }
  }

  return true;
}

// ============================================================================

bool spice_on_main_channel_read()
{
  struct SpiceChannel *channel = &spice.scMain;

  SpiceMiniDataHeader header;
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
    if (!spice_read_nl(channel, &msg, sizeof(msg)))
    {
      spice_disconnect();
      return false;
    }

    spice.sessionID = msg.session_id;

    spice.serverTokens = msg.agent_tokens;
    spice.hasAgent    = msg.agent_connected;
    if (spice.hasAgent && !spice_agent_connect())
    {
      spice_disconnect();
      DEBUG_ERROR("failed to connect to spice agent");
      return false;
    }

    if (msg.current_mouse_mode != SPICE_MOUSE_MODE_CLIENT && !spice_mouse_mode(false))
    {
      DEBUG_ERROR("failed to set mouse mode");
      return false;
    }

    if (!spice_write_msg(channel, SPICE_MSGC_MAIN_ATTACH_CHANNELS, NULL, 0))
    {
      spice_disconnect();
      DEBUG_ERROR("failed to ask for channel list");
      return false;
    }

    return true;
  }

  if (header.type == SPICE_MSG_MAIN_CHANNELS_LIST)
  {
    DEBUG_PROTO("SPICE_MSG_MAIN_CHANNELS_LIST");

    SpiceMainChannelsList msg;
    if (!spice_read_nl(channel, &msg, sizeof(msg)))
    {
      DEBUG_ERROR("Failed to read channel list msg");
      spice_disconnect();
      return false;
    }

    // documentation doesn't state that the array is null terminated but it seems that it is
    SpiceChannelID channels[msg.num_of_channels];
    if (!spice_read_nl(channel, &channels, msg.num_of_channels * sizeof(SpiceChannelID)))
    {
      DEBUG_ERROR("Failed to read channel list vector");
      spice_disconnect();
      return false;
    }

    for(int i = 0; i < msg.num_of_channels; ++i)
    {
      DEBUG_PROTO("channel %d = %u", i, channels[i].type);
      if (channels[i].type == SPICE_CHANNEL_INPUTS)
      {
        if (spice.scInputs.connected)
        {
          DEBUG_ERROR("inputs channel already connected");
          spice_disconnect();
          return false;
        }

        if (!spice_connect_channel(&spice.scInputs))
        {
          DEBUG_ERROR("failed to connect inputs channel");
          spice_disconnect();
          return false;
        }
      }
    }

    return true;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_CONNECTED)
  {
    DEBUG_PROTO("SPICE_MSG_MAIN_AGENT_CONNECTED");

    spice.hasAgent = true;
    if (!spice_agent_connect())
    {
      DEBUG_ERROR("failed to connect to spice agent");
      spice_disconnect();
      return false;
    }
    return true;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS)
  {
    DEBUG_PROTO("SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS");

    uint32_t num_tokens;
    if (!spice_read_nl(channel, &num_tokens, sizeof(num_tokens)))
    {
      DEBUG_ERROR("failed to read agent tokens");
      spice_disconnect();
      return false;
    }

    spice.hasAgent    = true;
    spice.serverTokens = num_tokens;
    if (!spice_agent_connect())
    {
      DEBUG_ERROR("failed to connect to spice agent");
      spice_disconnect();
      return false;
    }
    return true;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_DISCONNECTED)
  {
    DEBUG_PROTO("SPICE_MSG_MAIN_AGENT_DISCONNECTED");

    uint32_t error;
    if (!spice_read_nl(channel, &error, sizeof(error)))
    {
      DEBUG_ERROR("failed to read agent disconnect error code");
      spice_disconnect();
      return false;
    }

    DEBUG_INFO("Spice agent disconnected, error: %u", error);
    spice.hasAgent = false;

    if (spice.cbBuffer)
    {
      free(spice.cbBuffer);
      spice.cbBuffer = NULL;
      spice.cbSize   = 0;
      spice.cbRemain = 0;
    }

    return true;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_DATA)
  {
    DEBUG_PROTO("SPICE_MSG_MAIN_AGENT_DATA");

    if (!spice.hasAgent)
    {
      DEBUG_WARN("recieved agent data when the agent is yet to be started");
      spice_discard_nl(channel, header.size);
      return true;
    }

    if (!spice_agent_process(header.size))
    {
      DEBUG_ERROR("failed to process spice agent message");
      spice_disconnect();
      return false;
    }
    return true;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_TOKEN)
  {
    DEBUG_PROTO("SPICE_MSG_MAIN_AGENT_TOKEN");

    uint32_t num_tokens;
    if (!spice_read_nl(channel, &num_tokens, sizeof(num_tokens)))
    {
      DEBUG_ERROR("failed to read agent tokens");
      spice_disconnect();
      return false;
    }

    spice.serverTokens = num_tokens;
    return true;
  }

  DEBUG_WARN("main channel unhandled message type %u", header.type);
  spice_discard_nl(channel, header.size);
  return true;
}

// ============================================================================

bool spice_on_inputs_channel_read()
{
  struct SpiceChannel *channel = &spice.scInputs;

  SpiceMiniDataHeader header;
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
      if (!spice_read_nl(channel, &in, sizeof(in)))
        return false;

      return true;
    }

    case SPICE_MSG_INPUTS_KEY_MODIFIERS:
    {
      DEBUG_PROTO("SPICE_MSG_INPUTS_KEY_MODIFIERS");
      SpiceMsgInputsInit in;
      if (!spice_read_nl(channel, &in, sizeof(in)))
        return false;

      spice.kb.modifiers = in.modifiers;
      return true;
    }

    case SPICE_MSG_INPUTS_MOUSE_MOTION_ACK:
    {
      DEBUG_PROTO("SPICE_MSG_INPUTS_MOUSE_MOTION_ACK");
      const int count = __sync_add_and_fetch(&spice.mouse.sentCount, SPICE_INPUT_MOTION_ACK_BUNCH);
      if (count < 0)
      {
        DEBUG_ERROR("comms failure, too many mouse motion ACKs recieved");
        return false;
      }
      return true;
    }
  }

  DEBUG_WARN("inputs channel unhandled message type %u", header.type);
  spice_discard_nl(channel, header.size);
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

  uint32_t supportCaps[COMMON_CAPS_BYTES / sizeof(uint32_t)];
  uint32_t channelCaps[MAIN_CAPS_BYTES   / sizeof(uint32_t)];
  memset(supportCaps, 0, sizeof(supportCaps));
  memset(channelCaps, 0, sizeof(channelCaps));

  COMMON_SET_CAPABILITY(supportCaps, SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
  COMMON_SET_CAPABILITY(supportCaps, SPICE_COMMON_CAP_AUTH_SPICE             );
  COMMON_SET_CAPABILITY(supportCaps, SPICE_COMMON_CAP_MINI_HEADER            );

  if (channel == &spice.scMain)
    MAIN_SET_CAPABILITY(channelCaps, SPICE_MAIN_CAP_AGENT_CONNECTED_TOKENS);

  SpiceLinkHeader header =
  {
    .magic         = SPICE_MAGIC        ,
    .major_version = SPICE_VERSION_MAJOR,
    .minor_version = SPICE_VERSION_MINOR,
    .size          =
      sizeof(SpiceLinkMess) +
      sizeof(supportCaps  ) +
      sizeof(channelCaps  )
  };

  SpiceLinkMess message =
  {
    .connection_id    = spice.sessionID,
    .channel_type     = channel->channelType,
    .channel_id       = spice.channelID,
    .num_common_caps  = sizeof(supportCaps) / sizeof(uint32_t),
    .num_channel_caps = sizeof(channelCaps) / sizeof(uint32_t),
    .caps_offset      = sizeof(SpiceLinkMess)
  };

  if (
      !spice_write_nl(channel, &header     , sizeof(header     )) ||
      !spice_write_nl(channel, &message    , sizeof(message    )) ||
      !spice_write_nl(channel, &supportCaps, sizeof(supportCaps)) ||
      !spice_write_nl(channel, &channelCaps, sizeof(channelCaps))
     )
  {
    DEBUG_ERROR("failed to write the initial payload");
    spice_disconnect_channel(channel);
    return false;
  }

  if (!spice_read_nl(channel, &header, sizeof(header)))
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
  if (!spice_read_nl(channel, &reply, sizeof(reply)))
  {
    DEBUG_ERROR("failed to read SpiceLinkReply");
    spice_disconnect_channel(channel);
    return false;
  }

  if (reply.error != SPICE_LINK_ERR_OK)
  {
    DEBUG_ERROR("server replied with error %u", reply.error);
    spice_disconnect_channel(channel);
    return false;
  }

  uint32_t capsCommon [reply.num_common_caps ];
  uint32_t capsChannel[reply.num_channel_caps];
  if (
      !spice_read_nl(channel, &capsCommon , sizeof(capsCommon)) ||
      !spice_read_nl(channel, &capsChannel, sizeof(capsChannel))
     )
  {
    DEBUG_ERROR("failed to read the capabilities");
    spice_disconnect_channel(channel);
    return false;
  }

  SpiceLinkAuthMechanism auth;
  auth.auth_mechanism = SPICE_COMMON_CAP_AUTH_SPICE;
  if (!spice_write_nl(channel, &auth, sizeof(auth)))
  {
    DEBUG_ERROR("failed to write the auth mechanism");
    spice_disconnect_channel(channel);
    return false;
  }

  struct spice_password pass;
  if (!spice_rsa_encrypt_password(reply.pub_key, spice.password, &pass))
  {
    DEBUG_ERROR("failed to encrypt the password");
    spice_disconnect_channel(channel);
    return false;
  }

  if (!spice_write_nl(channel, pass.data, pass.size))
  {
    spice_rsa_free_password(&pass);
    DEBUG_ERROR("failed to write encrypted data");
    spice_disconnect_channel(channel);
    return false;
  }

  spice_rsa_free_password(&pass);

  uint32_t linkResult;
  if (!spice_read_nl(channel, &linkResult, sizeof(linkResult)))
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

  channel->ready = true;
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

bool spice_agent_connect()
{
  DEBUG_INFO("Spice agent available, sending start");

  spice.clientTokens = SPICE_AGENT_TOKENS_MAX;
  if (!spice_write_msg(&spice.scMain, SPICE_MSGC_MAIN_AGENT_START, &spice.clientTokens, sizeof(spice.clientTokens)))
  {
    DEBUG_ERROR("failed to send agent start message");
    return false;
  }

  if (!spice_agent_send_caps(true))
    return false;

  return true;
}

// ============================================================================

bool spice_agent_process(uint32_t dataSize)
{
  if (spice.cbRemain)
  {
    const uint32_t r = spice.cbRemain > dataSize ? dataSize : spice.cbRemain;
    if (!spice_read_nl(&spice.scMain, spice.cbBuffer + spice.cbSize, r))
    {
      DEBUG_ERROR("failed to read the clipboard data");
      free(spice.cbBuffer);
      spice.cbBuffer = NULL;
      spice.cbRemain = 0;
      spice.cbSize   = 0;
      return false;
    }

    spice.cbRemain -= r;
    spice.cbSize   += r;

    if (spice.cbRemain == 0)
      spice_agent_on_clipboard();

    return true;
  }

  VDAgentMessage msg;

  #pragma pack(push,1)
  struct Selection
  {
    uint8_t selection;
    uint8_t reserved[3];
  };
  #pragma pack(pop)

  if (!spice_read_nl(&spice.scMain, &msg, sizeof(msg)))
  {
    DEBUG_ERROR("failed to read spice agent message");
    return false;
  }
  dataSize -= sizeof(msg);

  if (msg.protocol != VD_AGENT_PROTOCOL)
  {
    DEBUG_ERROR("invalid or unknown spice agent protocol");
    return false;
  }

  switch(msg.type)
  {
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
    {
      VDAgentAnnounceCapabilities *caps = (VDAgentAnnounceCapabilities *)malloc(msg.size);
      memset(caps, 0, msg.size);

      if (!spice_read_nl(&spice.scMain, caps, msg.size))
      {
        DEBUG_ERROR("failed to read agent message payload");
        free(caps);
        return false;
      }

      const int capsSize = VD_AGENT_CAPS_SIZE_FROM_MSG_SIZE(msg.size);
      spice.cbSupported  = VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND) ||
                           VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize, VD_AGENT_CAP_CLIPBOARD_SELECTION);
      spice.cbSelection  = VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize, VD_AGENT_CAP_CLIPBOARD_SELECTION);

      if (spice.cbSupported)
        DEBUG_INFO("clipboard capability detected");

      if (caps->request && !spice_agent_send_caps(false))
      {
        free(caps);
        return false;
      }

      free(caps);
      return true;
    }

    case VD_AGENT_CLIPBOARD:
    case VD_AGENT_CLIPBOARD_REQUEST:
    case VD_AGENT_CLIPBOARD_GRAB:
    case VD_AGENT_CLIPBOARD_RELEASE:
    {
      uint32_t remaining = msg.size;
      if (spice.cbSelection)
      {
        struct Selection selection;
        if (!spice_read_nl(&spice.scMain, &selection, sizeof(selection)))
        {
          DEBUG_ERROR("failed to read the clipboard selection");
          return false;
        }
        remaining -= sizeof(selection);
        dataSize  -= sizeof(selection);
      }

      if (msg.type == VD_AGENT_CLIPBOARD_RELEASE)
      {
        DEBUG_CLIPBOARD("VD_AGENT_CLIPBOARD_RELEASE");
        spice.cbAgentGrabbed = false;
        if (spice.cbReleaseFn)
          spice.cbReleaseFn();
        return true;
      }

      if (msg.type == VD_AGENT_CLIPBOARD || msg.type == VD_AGENT_CLIPBOARD_REQUEST)
      {
        uint32_t type;
        if (!spice_read_nl(&spice.scMain, &type, sizeof(type)))
        {
          DEBUG_ERROR("failed to read the clipboard data type");
          return false;
        }
        remaining -= sizeof(type);
        dataSize  -= sizeof(type);

        if (msg.type == VD_AGENT_CLIPBOARD)
        {
          DEBUG_CLIPBOARD("VD_AGENT_CLIPBOARD");
          if (spice.cbBuffer)
          {
            DEBUG_ERROR("cbBuffer was never freed");
            return false;
          }

          spice.cbSize     = 0;
          spice.cbRemain   = remaining;
          spice.cbBuffer   = (uint8_t *)malloc(remaining);
          const uint32_t r = remaining > dataSize ? dataSize : remaining;

          if (!spice_read_nl(&spice.scMain, spice.cbBuffer, r))
          {
            DEBUG_ERROR("failed to read the clipboard data");
            free(spice.cbBuffer);
            spice.cbBuffer = NULL;
            spice.cbRemain = 0;
            spice.cbSize   = 0;
            return false;
          }

          spice.cbRemain -= r;
          spice.cbSize   += r;

          if (spice.cbRemain == 0)
            spice_agent_on_clipboard();

          return true;
        }
        else
        {
          DEBUG_CLIPBOARD("VD_AGENT_CLIPBOARD_REQUEST");
          if (spice.cbRequestFn)
            spice.cbRequestFn(agent_type_to_spice_type(type));
          return true;
        }
      }
      else
      {
        DEBUG_CLIPBOARD("VD_AGENT_CLIPBOARD_GRAB");
        if (remaining == 0)
          return true;

        uint32_t *types = malloc(remaining);
        if (!spice_read_nl(&spice.scMain, types, remaining))
        {
          DEBUG_ERROR("failed to read the clipboard grab types");
          return false;
        }

        // there is zero documentation on the types field, it might be a bitfield
        // but for now we are going to assume it's not.

        spice.cbType          = agent_type_to_spice_type(types[0]);
        spice.cbAgentGrabbed  = true;
        spice.cbClientGrabbed = false;
        if (spice.cbSelection)
        {
          // Windows doesnt support this, so until it's needed there is no point messing with it
          DEBUG_ERROR("Fixme!");
          return false;
        }

        if (spice.cbNoticeFn)
            spice.cbNoticeFn(spice.cbType);

        free(types);
        return true;
      }
    }
  }

  DEBUG_WARN("unknown agent message type %d", msg.type);
  spice_discard_nl(&spice.scMain, msg.size);
  return true;
}


// ============================================================================

void spice_agent_on_clipboard()
{
  if (spice.cbDataFn)
    spice.cbDataFn(spice.cbType, spice.cbBuffer, spice.cbSize);

  free(spice.cbBuffer);
  spice.cbBuffer = NULL;
  spice.cbSize   = 0;
  spice.cbRemain = 0;
}

// ============================================================================

bool spice_agent_send_caps(bool request)
{
  const ssize_t capsSize = sizeof(VDAgentAnnounceCapabilities) + VD_AGENT_CAPS_BYTES;
  VDAgentAnnounceCapabilities *caps = (VDAgentAnnounceCapabilities *)malloc(capsSize);
  memset(caps, 0, capsSize);

  caps->request = request ? 1 : 0;
  VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
  VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_SELECTION);

  if (!spice_agent_write_msg(VD_AGENT_ANNOUNCE_CAPABILITIES, caps, capsSize))
  {
    DEBUG_ERROR("failed to send agent capabilities");
    free(caps);
    return false;
  }
  free(caps);

  return true;
}

// ============================================================================

bool spice_agent_write_msg(uint32_t type, const void * buffer, ssize_t size)
{
  VDAgentMessage msg;
  msg.protocol = VD_AGENT_PROTOCOL;
  msg.type     = type;
  msg.opaque   = 0;
  msg.size     = size;

  LG_LOCK(spice.scMain.lock);

  uint8_t * buf   = (uint8_t *)buffer;
  ssize_t toWrite = size > VD_AGENT_MAX_DATA_SIZE - sizeof(msg) ? VD_AGENT_MAX_DATA_SIZE - sizeof(msg) : size;
  if (!spice_write_msg_nl(&spice.scMain, SPICE_MSGC_MAIN_AGENT_DATA, &msg, sizeof(msg), toWrite))
  {
    LG_UNLOCK(spice.scMain.lock);
    DEBUG_ERROR("failed to write agent data header");
    return false;
  }

  bool first = true;
  while(toWrite)
  {
    bool ok = false;
    if (first)
    {
      ok    = spice_write_nl(&spice.scMain, buf, toWrite) == toWrite;
      first = false;
    }
    else
    {
      ok = spice_write_msg_nl(&spice.scMain, SPICE_MSGC_MAIN_AGENT_DATA, buf, toWrite, 0);
    }

    if (!ok)
    {
      LG_UNLOCK(spice.scMain.lock);
      DEBUG_ERROR("failed to write agent data payload");
      return false;
    }

    size   -= toWrite;
    buf    += toWrite;
    toWrite = size > VD_AGENT_MAX_DATA_SIZE ? VD_AGENT_MAX_DATA_SIZE : size;
  }

  LG_UNLOCK(spice.scMain.lock);
  return true;
}

// ============================================================================

ssize_t spice_write_nl(const struct SpiceChannel * channel, const void * buffer, const ssize_t size)
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

inline bool spice_write_msg(struct SpiceChannel * channel, uint32_t type, const void * buffer, const ssize_t size)
{
  bool result;
  LG_LOCK(channel->lock);

  result = spice_write_msg_nl(channel, type, buffer, size, 0);

  LG_UNLOCK(channel->lock);
  return result;
}

// ============================================================================

bool spice_write_msg_nl(struct SpiceChannel * channel, uint32_t type, const void * buffer, const ssize_t size, const ssize_t extra)
{
  if (!channel->ready)
  {
    DEBUG_ERROR("channel not ready");
    return false;
  }

  SpiceMiniDataHeader header;
  ++channel->serial;
  header.type     = type;
  header.size     = size + extra;

  if (spice_write_nl(channel, &header, sizeof(header)) != sizeof(header))
  {
    DEBUG_ERROR("failed to write message header");
    return false;
  }

  if (buffer && size)
  {
    if (spice_write_nl(channel, buffer, size) != size)
    {
      DEBUG_ERROR("failed to write message body");
      return false;
    }
  }

  return true;
}

// ============================================================================

bool spice_read_nl(const struct SpiceChannel * channel, void * buffer, const ssize_t size)
{
  if (!channel->connected)
  {
    DEBUG_ERROR("not connected");
    return -1;
  }

  if (!buffer)
  {
    DEBUG_ERROR("invalid buffer argument supplied");
    return false;
  }

  size_t    left = size;
  uint8_t * buf  = (uint8_t *)buffer;
  while(left)
  {
    ssize_t len = read(channel->socket, buf, left);
    if (len <= 0)
    {
      if (len == 0)
        DEBUG_ERROR("remote end closd connection after %ld byte(s)", size - left);
      return false;
    }
    left -= len;
    buf  += len;
  }

  return true;
}

// ============================================================================

bool spice_discard_nl(const struct SpiceChannel * channel, ssize_t size)
{
  void *c = malloc(8192);
  ssize_t left = size;
  while(left)
  {
    size_t len = read(channel->socket, c, left > 8192 ? 8192 : left);
    if (len <= 0)
    {
      if (len == 0)
        DEBUG_ERROR("remote end closed connection after %ld byte(s)", size - left);
      free(c);
      return false;
    }
    left -= len;
  }

  free(c);
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
  if (!spice.scMain.connected)
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

  __sync_fetch_and_add(&spice.mouse.sentCount, 1);
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

  SpiceMsgcMouseMotion msg;
  msg.x            = x;
  msg.y            = y;
  msg.button_state = spice.mouse.buttonState;

  __sync_fetch_and_add(&spice.mouse.sentCount, 1);
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

// ============================================================================

static uint32_t spice_type_to_agent_type(SpiceDataType type)
{
  switch(type)
  {
    case SPICE_DATA_TEXT: return VD_AGENT_CLIPBOARD_UTF8_TEXT ; break;
    case SPICE_DATA_PNG : return VD_AGENT_CLIPBOARD_IMAGE_PNG ; break;
    case SPICE_DATA_BMP : return VD_AGENT_CLIPBOARD_IMAGE_BMP ; break;
    case SPICE_DATA_TIFF: return VD_AGENT_CLIPBOARD_IMAGE_TIFF; break;
    case SPICE_DATA_JPEG: return VD_AGENT_CLIPBOARD_IMAGE_JPG ; break;
    default:
      DEBUG_ERROR("unsupported spice data type specified");
      return VD_AGENT_CLIPBOARD_NONE;
  }
}

static SpiceDataType agent_type_to_spice_type(uint32_t type)
{
  switch(type)
  {
    case VD_AGENT_CLIPBOARD_UTF8_TEXT : return SPICE_DATA_TEXT; break;
    case VD_AGENT_CLIPBOARD_IMAGE_PNG : return SPICE_DATA_PNG ; break;
    case VD_AGENT_CLIPBOARD_IMAGE_BMP : return SPICE_DATA_BMP ; break;
    case VD_AGENT_CLIPBOARD_IMAGE_TIFF: return SPICE_DATA_TIFF; break;
    case VD_AGENT_CLIPBOARD_IMAGE_JPG : return SPICE_DATA_JPEG; break;
    default:
      DEBUG_ERROR("unsupported agent data type specified");
      return SPICE_DATA_NONE;
  }
}

// ============================================================================

bool spice_clipboard_request(SpiceDataType type)
{
  VDAgentClipboardRequest req;

  if (!spice.cbAgentGrabbed)
  {
    DEBUG_ERROR("the agent has not grabbed any data yet");
    return false;
  }

  if (type != spice.cbType)
  {
    DEBUG_ERROR("data type requested doesn't match reported data type");
    return false;
  }

  req.type = spice_type_to_agent_type(type);
  if (!spice_agent_write_msg(VD_AGENT_CLIPBOARD_REQUEST, &req, sizeof(req)))
  {
    DEBUG_ERROR("failed to request clipboard data");
    return false;
  }

  return true;
}

// ============================================================================

bool spice_set_clipboard_cb(SpiceClipboardNotice cbNoticeFn, SpiceClipboardData cbDataFn, SpiceClipboardRelease cbReleaseFn, SpiceClipboardRequest cbRequestFn)
{
  if ((cbNoticeFn && !cbDataFn) || (cbDataFn && !cbNoticeFn))
  {
    DEBUG_ERROR("clipboard callback notice and data callbacks must be specified");
    return false;
  }

  spice.cbNoticeFn  = cbNoticeFn;
  spice.cbDataFn    = cbDataFn;
  spice.cbReleaseFn = cbReleaseFn;
  spice.cbRequestFn = cbRequestFn;

  return true;
}

// ============================================================================

bool spice_clipboard_grab(SpiceDataType type)
{
  if (type == SPICE_DATA_NONE)
  {
    DEBUG_ERROR("grab type is invalid");
    return false;
  }

  if (spice.cbSelection)
  {
    uint8_t req[8] = { VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD };
    ((uint32_t*)req)[1] = spice_type_to_agent_type(type);

    if (!spice_agent_write_msg(VD_AGENT_CLIPBOARD_GRAB, req, sizeof(req)))
    {
      DEBUG_ERROR("failed to grab the clipboard");
      return false;
    }

    spice.cbClientGrabbed = true;
    return true;
  }

  uint32_t req = spice_type_to_agent_type(type);
  if (!spice_agent_write_msg(VD_AGENT_CLIPBOARD_GRAB, &req, sizeof(req)))
  {
    DEBUG_ERROR("failed to grab the clipboard");
    return false;
  }

  spice.cbClientGrabbed = true;
  return true;
}

// ============================================================================

bool spice_clipboard_release()
{
  // check if if there is anything to release first
  if (!spice.cbClientGrabbed)
    return true;

  if (spice.cbSelection)
  {
    uint8_t req[4] = { VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD };
    if (!spice_agent_write_msg(VD_AGENT_CLIPBOARD_RELEASE, req, sizeof(req)))
    {
      DEBUG_ERROR("failed to release the clipboard");
      return false;
    }

    spice.cbClientGrabbed = false;
    return true;
  }

   if (!spice_agent_write_msg(VD_AGENT_CLIPBOARD_RELEASE, NULL, 0))
   {
     DEBUG_ERROR("failed to release the clipboard");
     return false;
   }

   spice.cbClientGrabbed = false;
   return true;
}

// ============================================================================

bool spice_clipboard_data(SpiceDataType type, uint8_t * data, size_t size)
{
  uint8_t * buffer;
  size_t    bufSize;

  if (spice.cbSelection)
  {
    bufSize                = 8 + size;
    buffer                 = (uint8_t *)malloc(bufSize);
    buffer[0]              = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    buffer[1]              = buffer[2] = buffer[3] = 0;
    ((uint32_t*)buffer)[1] = spice_type_to_agent_type(type);
    memcpy(buffer + 8, data, size);
  }
  else
  {
    bufSize                = 4 + size;
    buffer                 = (uint8_t *)malloc(bufSize);
    ((uint32_t*)buffer)[0] = spice_type_to_agent_type(type);
    memcpy(buffer + 4, data, size);
  }

  if (!spice_agent_write_msg(VD_AGENT_CLIPBOARD, buffer, bufSize))
  {
    DEBUG_ERROR("failed to write the clipboard data");
    free(buffer);
    return false;
  }

  free(buffer);
  return true;
}