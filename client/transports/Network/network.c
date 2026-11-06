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

#include "network.h"
#include "audio.h"
#include "clipboard.h"
#include "connection.h"
#include "control.h"
#include "cursor.h"
#include "input.h"
#include "known_hosts.h"
#include "recovery.h"
#include "video.h"

#include "app.h"
#include "interface/renderer.h"

#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"
#include <LGProtocol/NetworkProtocol.h>
#include <LGProtocol/NetworkServices.h>
#include "common/option.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_ADDRESS_MAX           255U
#define NETWORK_ENDPOINT_TEXT_SIZE    (NETWORK_ADDRESS_MAX + 9U)
#define NETWORK_MAX_PAYLOAD           \
  (LG_NET_CURSOR_SHAPE_HEADER_WIRE_SIZE + LG_NET_CURSOR_MAX_SHAPE_LENGTH)
#define NETWORK_MAX_DATAGRAM          1136U
#define NETWORK_AUDIO_MAX_PAYLOAD     \
  (LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE + 65536U)
#define NETWORK_TRANSFER_CHUNK        (256U * 1024U)
#define NETWORK_CLIPBOARD_MAX_PAYLOAD \
  (LG_NET_CLIPBOARD_CHUNK_HEADER_WIRE_SIZE + NETWORK_TRANSFER_CHUNK)
#define NETWORK_FILE_MAX_PAYLOAD      \
  (LG_NET_FILE_CHUNK_HEADER_WIRE_SIZE + NETWORK_TRANSFER_CHUNK)
#define NETWORK_VIDEO_MAX_PAYLOAD     LG_NET_MAX_PAYLOAD_LENGTH
#define NETWORK_INPUT_MAX_PAYLOAD     LG_NET_INPUT_ABSOLUTE_WIRE_SIZE

static const LGNetCapability NETWORK_CAPABILITIES[] =
{
  {
    .service                  = LG_NET_SERVICE_RECOVERY,
    .versionMin               = LG_NET_RECOVERY_VERSION_MIN,
    .versionMax               = LG_NET_RECOVERY_VERSION_CURRENT,
    .flags                    = LG_NET_CAPABILITY_RELIABLE,
    .features                 = 0,
    .maxPayloadLength         = NETWORK_MAX_PAYLOAD,
    .maxDatagramPayload       = 0,
    .maxBidirectionalStreams  = 0,
    .maxUnidirectionalStreams = 0,
  },
  {
    .service                  = LG_NET_SERVICE_CURSOR,
    .versionMin               = LG_NET_CURSOR_VERSION_MIN,
    .versionMax               = LG_NET_CURSOR_VERSION_CURRENT,
    .flags                    = LG_NET_CAPABILITY_RELIABLE |
      LG_NET_CAPABILITY_DATAGRAM,
    .features                 = 0,
    .maxPayloadLength         = NETWORK_MAX_PAYLOAD,
    .maxDatagramPayload       = NETWORK_MAX_DATAGRAM,
    .maxBidirectionalStreams  = 0,
    .maxUnidirectionalStreams = 0,
  },
  {
    .service                  = LG_NET_SERVICE_INPUT,
    .versionMin               = LG_NET_INPUT_VERSION_MIN,
    .versionMax               = LG_NET_INPUT_VERSION_CURRENT,
    .flags                    = LG_NET_CAPABILITY_RELIABLE |
      LG_NET_CAPABILITY_DATAGRAM,
    .features                 = 0,
    .maxPayloadLength         = NETWORK_INPUT_MAX_PAYLOAD,
    .maxDatagramPayload       = NETWORK_INPUT_MAX_PAYLOAD,
    .maxBidirectionalStreams  = 1,
    .maxUnidirectionalStreams = 0,
  },
  {
    .service                  = LG_NET_SERVICE_VIDEO,
    .versionMin               =
      LG_NET_VIDEO_BLOCK_FRAGMENTATION_INTRODUCED_SERVICE_VERSION,
    .versionMax               = LG_NET_VIDEO_VERSION_CURRENT,
    .flags                    = LG_NET_CAPABILITY_RELIABLE |
      LG_NET_CAPABILITY_DATAGRAM | LG_NET_CAPABILITY_BULK,
    .features                 = 0,
    .maxPayloadLength         = NETWORK_VIDEO_MAX_PAYLOAD,
    .maxDatagramPayload       = NETWORK_MAX_DATAGRAM,
    .maxBidirectionalStreams  = 0,
    .maxUnidirectionalStreams = 1,
  },
  {
    .service                  = LG_NET_SERVICE_AUDIO,
    .versionMin               =
      LG_NET_AUDIO_OWNERSHIP_INTRODUCED_SERVICE_VERSION,
    .versionMax               = LG_NET_AUDIO_VERSION_CURRENT,
    .flags                    = LG_NET_CAPABILITY_RELIABLE |
      LG_NET_CAPABILITY_DATAGRAM | LG_NET_CAPABILITY_MULTI_CLIENT,
    .features                 = 0,
    .maxPayloadLength         = NETWORK_AUDIO_MAX_PAYLOAD,
    .maxDatagramPayload       = NETWORK_MAX_DATAGRAM,
    .maxBidirectionalStreams  = 0,
    .maxUnidirectionalStreams = 0,
  },
  {
    .service                  = LG_NET_SERVICE_CLIPBOARD,
    .versionMin               = LG_NET_CLIPBOARD_VERSION_MIN,
    .versionMax               = LG_NET_CLIPBOARD_VERSION_CURRENT,
    .flags                    = LG_NET_CAPABILITY_RELIABLE |
      LG_NET_CAPABILITY_BULK,
    .features                 = 0,
    .maxPayloadLength         = NETWORK_CLIPBOARD_MAX_PAYLOAD,
    .maxDatagramPayload       = 0,
    .maxBidirectionalStreams  = 0,
    .maxUnidirectionalStreams = 1,
  },
  {
    .service                  = LG_NET_SERVICE_FILE,
    .versionMin               = LG_NET_FILE_VERSION_MIN,
    .versionMax               = LG_NET_FILE_VERSION_CURRENT,
    .flags                    = LG_NET_CAPABILITY_RELIABLE |
      LG_NET_CAPABILITY_BULK,
    .features                 = 0,
    .maxPayloadLength         = NETWORK_FILE_MAX_PAYLOAD,
    .maxDatagramPayload       = 0,
    .maxBidirectionalStreams  = 0,
    .maxUnidirectionalStreams = 1,
  },
  {
    .service                  = LG_NET_SERVICE_CONTROL,
    .versionMin               = LG_NET_CONTROL_VERSION_MIN,
    .versionMax               = LG_NET_CONTROL_VERSION_CURRENT,
    .flags                    = LG_NET_CAPABILITY_RELIABLE,
    .features                 = 0,
    .maxPayloadLength         = NETWORK_MAX_PAYLOAD,
    .maxDatagramPayload       = 0,
    .maxBidirectionalStreams  = 0,
    .maxUnidirectionalStreams = 0,
  },
};

struct NetworkTrustPrompt
{
  MsgBoxHandle            message;
  NetworkTrustDecisionFn  decision;
  void                  * decisionOpaque;
};

struct LG_Transport
{
  NetworkConnection * connection;
  NetworkAudio      * audio;
  NetworkClipboard  * clipboard;
  NetworkControl    * control;
  NetworkCursor     * cursor;
  NetworkInput      * input;
  NetworkRecovery   * recovery;
  NetworkVideo      * video;
  NetworkKnownHosts   knownHosts;
  LGEvent           * promptIdle;

  LG_Lock callbackLock;
  LG_Lock stateLock;
  LG_Lock promptLock;

  LG_VideoStatus   videoStatus;
  LG_VideoStatusFn videoStatusFn;
  void            * videoStatusOpaque;

  bool                      promptCallbackActive;
  struct NetworkTrustPrompt prompt;
};

static bool networkAddressValidate(struct Option * option,
    const char ** error)
{
  const char * address = option->value.x_string;
  if (address && *address && strlen(address) <= NETWORK_ADDRESS_MAX)
  {
    for (const unsigned char * c = (const unsigned char *)address; *c; ++c)
      if (*c < 0x20U || *c == 0x7fU)
        goto invalid;
    return true;
  }

invalid:
  *error = "The network address must be 1-255 characters without controls";
  return false;
}

static bool networkPortValidate(struct Option * option, const char ** error)
{
  if (option->value.x_int > 0 && option->value.x_int <= UINT16_MAX)
    return true;

  *error = "The network port must be between 1 and 65535";
  return false;
}

static void networkSetup(void)
{
  static struct Option options[] =
  {
    {
      .module         = "network",
      .name           = "address",
      .description    = "Network server address",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "127.0.0.1",
      .validator      = networkAddressValidate,
    },
    {
      .module      = "network",
      .name        = "port",
      .description = "Network server UDP port",
      .type        = OPTION_TYPE_INT,
      .value.x_int = LG_NET_DEFAULT_PORT,
      .validator   = networkPortValidate,
    },
    {
      .module         = "network",
      .name           = "password",
      .description    = "Network password (empty uses no authentication)",
      .type           = OPTION_TYPE_STRING,
      .sensitive      = true,
      .value.x_string = NULL,
    },
    {
      .module       = "network",
      .name         = "allowUnencrypted",
      .description  = "Allow unencrypted 1-RTT network payloads",
      .type         = OPTION_TYPE_BOOL,
      .value.x_bool = false,
    },
    {
      .module       = "network",
      .name         = "forgetCertificate",
      .description  = "Forget the saved certificate for this server",
      .type         = OPTION_TYPE_BOOL,
      .value.x_bool = false,
    },
    {0}
  };

  option_register(options);
}

static void networkTrustDecision(bool trusted, void * opaque)
{
  LG_Transport * this = opaque;
  NetworkTrustDecisionFn decision;
  void                 * decisionOpaque;

  LG_LOCK(this->promptLock);
  decision                    = this->prompt.decision;
  decisionOpaque              = this->prompt.decisionOpaque;
  this->prompt.message        = NULL;
  this->prompt.decision       = NULL;
  this->prompt.decisionOpaque = NULL;
  if (decision)
  {
    this->promptCallbackActive = true;
    lgResetEvent(this->promptIdle);
  }
  LG_UNLOCK(this->promptLock);

  if (decision)
    decision(decisionOpaque, trusted);

  LG_LOCK(this->promptLock);
  this->promptCallbackActive = false;
  lgSignalEvent(this->promptIdle);
  LG_UNLOCK(this->promptLock);
}

static bool networkTrustPrompt(void * opaque, const char * endpoint,
    const char * fingerprint, NetworkTrustDecisionFn decision,
    void * decisionOpaque)
{
  LG_Transport * this = opaque;

  LG_LOCK(this->promptLock);
  if (this->prompt.message || this->prompt.decision ||
      this->promptCallbackActive)
  {
    LG_UNLOCK(this->promptLock);
    return false;
  }

  this->prompt.decision       = decision;
  this->prompt.decisionOpaque = decisionOpaque;
  this->prompt.message        = app_confirmMsgBox(
      "Trust network server?", networkTrustDecision, this,
      "The identity of %s has not been seen before.\n\n"
      "SHA-256 fingerprint:\n%s\n\n"
      "Trust and remember this server?", endpoint, fingerprint);
  const bool queued = this->prompt.message != NULL;
  if (!queued)
  {
    this->prompt.decision       = NULL;
    this->prompt.decisionOpaque = NULL;
  }
  LG_UNLOCK(this->promptLock);
  return queued;
}

static void networkCancelTrustPrompt(void * opaque)
{
  LG_Transport * this = opaque;
  MsgBoxHandle  message;

  LG_LOCK(this->promptLock);
  message                     = this->prompt.message;
  this->prompt.message        = NULL;
  this->prompt.decision       = NULL;
  this->prompt.decisionOpaque = NULL;
  LG_UNLOCK(this->promptLock);

  if (message)
    app_msgBoxClose(message);
  lgWaitEvent(this->promptIdle, TIMEOUT_INFINITE);
  LG_LOCK(this->promptLock);
  LG_UNLOCK(this->promptLock);
}

static void networkNotifyVideoStatus(LG_Transport * this)
{
  LG_VideoStatusFn    callback;
  void              * opaque;
  LG_VideoStatus      status;

  LG_LOCK(this->callbackLock);
  LG_LOCK(this->stateLock);
  callback = this->videoStatusFn;
  opaque   = this->videoStatusOpaque;
  status   = this->videoStatus;
  LG_UNLOCK(this->stateLock);

  if (callback)
    callback(opaque, &status);
  LG_UNLOCK(this->callbackLock);
}

static void networkVideoStatus(void * opaque,
    const LG_VideoComponentStatus * status)
{
  LG_Transport * this = opaque;

  LG_LOCK(this->stateLock);
  this->videoStatus.frame = *status;
  LG_UNLOCK(this->stateLock);
  networkNotifyVideoStatus(this);
}

static uint64_t networkNextEpoch(uint64_t epoch)
{
  ++epoch;
  return epoch ? epoch : 1U;
}

static void networkSetUnavailable(LG_VideoComponentStatus * status,
    LG_TransportStatus reason)
{
  if (status->available)
    status->epoch = networkNextEpoch(status->epoch);
  status->available = false;
  status->reason    = reason;
}

static void networkSetAvailable(LG_VideoComponentStatus * status)
{
  if (!status->available)
    status->epoch = networkNextEpoch(status->epoch);
  status->available = true;
  status->reason    = LG_TRANSPORT_OK;
}

static void networkConnectionState(void * opaque, bool connected,
    LG_TransportStatus reason)
{
  LG_Transport             * this   = opaque;
  const LG_TransportStatus   status = connected ?
    LG_TRANSPORT_UNAVAILABLE : reason;

  networkInputConnectionState(this->input, connected);
  networkAudioConnectionState(this->audio, connected);
  networkClipboardConnectionState(this->clipboard, connected);
  networkControlConnectionState(this->control, connected);
  networkRecoveryConnectionState(this->recovery, connected);
  networkVideoConnectionState(this->video, connected);
  const bool cursorAvailable =
    networkCursorConnectionState(this->cursor, connected);

  LG_LOCK(this->stateLock);
  if (cursorAvailable)
    networkSetAvailable(&this->videoStatus.pointer);
  else
    networkSetUnavailable(&this->videoStatus.pointer, status);
  LG_UNLOCK(this->stateLock);

  networkNotifyVideoStatus(this);
}

static void networkPacket(void * opaque, const LGNetPacketView * packet,
    NetworkPacketDelivery delivery)
{
  LG_Transport * this     = opaque;
  const bool     datagram =
    delivery == NETWORK_PACKET_DELIVERY_DATAGRAM;
  bool           valid;
  switch (packet->envelope.service)
  {
    case LG_NET_SERVICE_RECOVERY:
      valid = delivery != NETWORK_PACKET_DELIVERY_BULK &&
        networkRecoveryPacket(this->recovery, packet, datagram);
      break;
    case LG_NET_SERVICE_CURSOR:
      valid = delivery != NETWORK_PACKET_DELIVERY_BULK &&
        networkCursorPacket(this->cursor, packet, datagram);
      break;
    case LG_NET_SERVICE_INPUT:
      valid = delivery != NETWORK_PACKET_DELIVERY_BULK &&
        networkInputPacket(this->input, packet, datagram);
      break;
    case LG_NET_SERVICE_AUDIO:
      valid = delivery != NETWORK_PACKET_DELIVERY_BULK &&
        (datagram ? networkAudioDatagram(this->audio, packet) :
          networkAudioPacket(this->audio, packet));
      break;
    case LG_NET_SERVICE_VIDEO:
      valid = datagram ? networkVideoDatagram(this->video, packet) :
        networkVideoPacket(this->video, packet);
      break;
    case LG_NET_SERVICE_CLIPBOARD:
    case LG_NET_SERVICE_FILE:
      valid = delivery != NETWORK_PACKET_DELIVERY_DATAGRAM &&
        networkClipboardPacket(this->clipboard, packet, delivery);
      break;
    case LG_NET_SERVICE_CONTROL:
      valid = delivery != NETWORK_PACKET_DELIVERY_BULK &&
        networkControlPacket(this->control, packet, datagram);
      break;
    default:
      valid = false;
      break;
  }

  if (!valid)
  {
    DEBUG_ERROR("Invalid network service %u message %u",
        (unsigned)packet->envelope.service,
        (unsigned)packet->envelope.messageType);
    networkConnectionCancel(this->connection);
  }
}

static void networkSendReady(void * opaque, LGNetService service,
    NetworkPacketDelivery delivery)
{
  LG_Transport * this     = opaque;
  const bool     datagram =
    delivery == NETWORK_PACKET_DELIVERY_DATAGRAM;
  if (service == LG_NET_SERVICE_INPUT)
    networkInputSendReady(this->input, delivery);
  if (service == LG_NET_SERVICE_AUDIO &&
      delivery != NETWORK_PACKET_DELIVERY_BULK)
    networkAudioSendReady(this->audio, datagram);
  if ((service == LG_NET_SERVICE_CLIPBOARD ||
      service == LG_NET_SERVICE_FILE) && !datagram)
    networkClipboardSendReady(this->clipboard, service, delivery);
  if (service == LG_NET_SERVICE_VIDEO &&
      delivery != NETWORK_PACKET_DELIVERY_BULK)
    networkVideoSendReady(this->video, datagram);
}

static void networkDatagramLost(void * opaque, LGNetService service,
    uint64_t componentEpoch, uint64_t applicationSequence)
{
  LG_Transport * this = opaque;
  if (service == LG_NET_SERVICE_INPUT)
    networkInputDatagramLost(
        this->input, componentEpoch, applicationSequence);
}

static bool networkCreate(LG_Transport ** result)
{
  if (!result)
  {
    DEBUG_ERROR("Network transport creation requires an output pointer");
    return false;
  }

  LG_Transport * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("Failed to allocate the network transport");
    return false;
  }

  LG_LOCK_INIT(this->callbackLock);
  LG_LOCK_INIT(this->stateLock);
  LG_LOCK_INIT(this->promptLock);

  this->videoStatus.frame.epoch    = 1;
  this->videoStatus.frame.reason   = LG_TRANSPORT_DISCONNECTED;
  this->videoStatus.pointer.epoch  = 1;
  this->videoStatus.pointer.reason = LG_TRANSPORT_DISCONNECTED;
  if (!networkKnownHostsInit(&this->knownHosts))
  {
    DEBUG_ERROR("Failed to initialize the network known-hosts store");
    goto fail;
  }

  this->promptIdle = lgCreateEvent(false, 0);
  if (!this->promptIdle)
  {
    DEBUG_ERROR("Failed to create the network trust-prompt event");
    goto fail;
  }
  if (!lgSignalEvent(this->promptIdle))
  {
    DEBUG_ERROR("Failed to initialize the network trust-prompt event");
    goto fail;
  }

  const char    * address = option_get_string("network", "address");
  const uint16_t  port    = (uint16_t)option_get_int("network", "port");
  char            endpoint[NETWORK_ENDPOINT_TEXT_SIZE];
  if (!address)
  {
    DEBUG_ERROR("The network server address is not configured");
    goto fail;
  }
  snprintf(endpoint, sizeof(endpoint), "[%s]:%u", address, port);
  if (option_get_bool("network", "forgetCertificate") &&
      !networkKnownHostsForget(&this->knownHosts, endpoint))
  {
    DEBUG_ERROR("Failed to forget the network server certificate");
    goto fail;
  }

  const NetworkConnectionParams params =
  {
    .address          = address,
    .port             = port,
    .password         = option_get_string("network", "password"),
    .allowUnencrypted = option_get_bool("network", "allowUnencrypted"),
    .knownHosts       = &this->knownHosts,
    .trustPrompt      = networkTrustPrompt,
    .trustCancel      = networkCancelTrustPrompt,
    .packet           = networkPacket,
    .state            = networkConnectionState,
    .sendReady        = networkSendReady,
    .datagramLost     = networkDatagramLost,
    .capabilities     = NETWORK_CAPABILITIES,
    .capabilityCount  = sizeof(NETWORK_CAPABILITIES) /
      sizeof(NETWORK_CAPABILITIES[0]),
    .opaque           = this,
  };
  if (!networkConnectionCreate(&this->connection, &params))
  {
    DEBUG_ERROR("Failed to create the network connection");
    goto fail;
  }
  if (!networkAudioCreate(&this->audio, this->connection))
  {
    DEBUG_ERROR("Failed to create the network audio service");
    goto fail;
  }
  if (!networkClipboardCreate(&this->clipboard, this->connection))
  {
    DEBUG_ERROR("Failed to create the network clipboard service");
    goto fail;
  }
  if (!networkControlCreate(&this->control, this->connection))
  {
    DEBUG_ERROR("Failed to create the network control service");
    goto fail;
  }
  if (!networkCursorCreate(&this->cursor, this->connection))
  {
    DEBUG_ERROR("Failed to create the network cursor service");
    goto fail;
  }
  if (!networkInputCreate(&this->input, this->connection))
  {
    DEBUG_ERROR("Failed to create the network input service");
    goto fail;
  }
  if (!networkRecoveryCreate(&this->recovery, this->connection))
  {
    DEBUG_ERROR("Failed to create the network recovery service");
    goto fail;
  }
  if (!networkVideoCreate(&this->video, this->connection))
  {
    DEBUG_ERROR("Failed to create the network video service");
    goto fail;
  }
  networkVideoSetStatusListener(this->video, networkVideoStatus, this);

  *result = this;
  return true;

fail:
  networkVideoDestroy(&this->video);
  networkRecoveryDestroy(&this->recovery);
  networkInputDestroy(&this->input);
  networkCursorDestroy(&this->cursor);
  networkControlDestroy(&this->control);
  networkClipboardDestroy(&this->clipboard);
  networkAudioDestroy(&this->audio);
  networkConnectionDestroy(&this->connection);
  if (this->promptIdle)
    lgFreeEvent(this->promptIdle);
  networkKnownHostsFree(&this->knownHosts);
  LG_LOCK_FREE(this->promptLock);
  LG_LOCK_FREE(this->stateLock);
  LG_LOCK_FREE(this->callbackLock);
  free(this);
  return false;
}

static void networkDisconnect(LG_Transport * this);

static void networkDestroy(LG_Transport ** transport)
{
  if (!transport || !*transport)
    return;

  LG_Transport * this = *transport;
  networkDisconnect(this);

  LG_LOCK(this->callbackLock);
  LG_LOCK(this->stateLock);
  this->videoStatusFn     = NULL;
  this->videoStatusOpaque = NULL;
  LG_UNLOCK(this->stateLock);
  LG_UNLOCK(this->callbackLock);

  networkRecoveryDestroy(&this->recovery);
  networkVideoDestroy(&this->video);
  networkInputDestroy(&this->input);
  networkCursorDestroy(&this->cursor);
  networkControlDestroy(&this->control);
  networkClipboardDestroy(&this->clipboard);
  networkAudioDestroy(&this->audio);
  networkConnectionDestroy(&this->connection);
  lgFreeEvent(this->promptIdle);
  networkKnownHostsFree(&this->knownHosts);
  LG_LOCK_FREE(this->promptLock);
  LG_LOCK_FREE(this->stateLock);
  LG_LOCK_FREE(this->callbackLock);
  free(this);
  *transport = NULL;
}

static LG_TransportStatus networkConnectCancellable(LG_Transport * this,
    LG_TransportSession * session, LG_TransportCancelledFn cancelled,
    void * opaque)
{
  return networkConnectionConnect(
      this->connection, session, cancelled, opaque);
}

static LG_TransportStatus networkConnect(LG_Transport * this,
    LG_TransportSession * session)
{
  return networkConnectCancellable(this, session, NULL, NULL);
}

static void networkDisconnect(LG_Transport * this)
{
  networkInputConnectionState(this->input, false);
  networkAudioConnectionState(this->audio, false);
  networkClipboardConnectionState(this->clipboard, false);
  networkControlConnectionState(this->control, false);
  networkRecoveryConnectionState(this->recovery, false);
  networkVideoConnectionState(this->video, false);
  networkCursorConnectionState(this->cursor, false);
  networkConnectionDisconnect(this->connection);
}

static bool networkSessionValid(LG_Transport * this)
{
  return networkConnectionValid(this->connection);
}

static void networkSetStatusListener(LG_Transport * this,
    LG_VideoStatusFn callback, void * opaque)
{
  LG_VideoStatus status;

  LG_LOCK(this->callbackLock);
  LG_LOCK(this->stateLock);
  this->videoStatusFn     = callback;
  this->videoStatusOpaque = opaque;
  status                  = this->videoStatus;
  LG_UNLOCK(this->stateLock);

  if (callback)
    callback(opaque, &status);
  LG_UNLOCK(this->callbackLock);
}

static bool networkSupportsDMA(LG_Transport * this)
{
  (void)this;
  return false;
}

static bool networkAttachRenderer(LG_Transport * this,
    const LG_RendererInterop * interop)
{
  return networkVideoAttachRenderer(this->video, interop);
}

static void networkDetachRenderer(LG_Transport * this)
{
  networkVideoDetachRenderer(this->video);
}

static LG_TransportStatus networkNextFrame(LG_Transport * this, bool useDMA,
    LG_TransportFrame * frame)
{
  return networkVideoNextFrame(this->video, useDMA, frame);
}

static void networkGetFrameTiming(LG_Transport * this,
    const LG_TransportFrame * frame, LG_TransportFrameTiming * timing)
{
  networkVideoGetFrameTiming(this->video, frame, timing);
}

static void networkReleaseFrame(LG_Transport * this,
    LG_TransportFrame * frame)
{
  networkVideoReleaseFrame(this->video, frame);
}

static void networkCancelFrameWait(LG_Transport * this)
{
  networkVideoCancelFrameWait(this->video);
}

static void networkStopFrame(LG_Transport * this)
{
  networkVideoStopFrame(this->video);
}

static LG_TransportStatus networkNextPointer(LG_Transport * this,
    LG_TransportPointer * pointer)
{
  const LG_TransportStatus status =
    networkCursorNext(this->cursor, pointer);
  if (status != LG_TRANSPORT_OK)
    return status;

  LG_LOCK(this->stateLock);
  pointer->epoch = this->videoStatus.pointer.epoch;
  LG_UNLOCK(this->stateLock);
  return LG_TRANSPORT_OK;
}

static void networkReleasePointer(LG_Transport * this,
    LG_TransportPointer * pointer)
{
  networkCursorRelease(this->cursor, pointer);
}

static void networkCancelPointerWait(LG_Transport * this)
{
  networkCursorCancel(this->cursor);
}

static void networkStopPointer(LG_Transport * this)
{
  networkCursorStop(this->cursor);
}

static const LG_FrameOps networkFrameOps =
{
  .setStatusListener = networkSetStatusListener,
  .supportsDMA       = networkSupportsDMA,
  .attachRenderer    = networkAttachRenderer,
  .detachRenderer    = networkDetachRenderer,
  .nextFrame         = networkNextFrame,
  .getFrameTiming    = networkGetFrameTiming,
  .releaseFrame      = networkReleaseFrame,
  .cancelFrameWait   = networkCancelFrameWait,
  .stopFrame         = networkStopFrame,
  .nextPointer       = networkNextPointer,
  .releasePointer    = networkReleasePointer,
  .cancelPointerWait = networkCancelPointerWait,
  .stopPointer       = networkStopPointer,
};

static const LG_VideoOps networkVideoOps =
{
  .name  = "network",
  .type  = LG_VIDEO_TYPE_FRAME,
  .frame = &networkFrameOps,
};

static const LG_VideoOps * networkGetVideoOps(LG_Transport * this)
{
  (void)this;
  return &networkVideoOps;
}

static const LG_InputOps * networkGetInputOps(LG_Transport * this,
    void ** opaque)
{
  *opaque = NULL;
  if (!networkConnectionServiceAvailable(this->connection,
        LG_NET_SERVICE_INPUT, LG_NET_CAPABILITY_RELIABLE))
    return NULL;
  *opaque = this->input;
  return networkInputGetOps();
}

static const LG_AudioOps * networkGetAudioOps(LG_Transport * this,
    void ** opaque)
{
  *opaque = NULL;
  if (!networkConnectionServiceAvailable(this->connection,
        LG_NET_SERVICE_AUDIO, LG_NET_CAPABILITY_RELIABLE))
    return NULL;
  *opaque = this->audio;
  return networkAudioGetOps();
}

static const LG_ClipboardOps * networkGetClipboardOps(LG_Transport * this,
    void ** opaque)
{
  *opaque = NULL;
  if (!networkConnectionServiceAvailable(this->connection,
        LG_NET_SERVICE_CLIPBOARD, LG_NET_CAPABILITY_RELIABLE) ||
      !networkConnectionServiceAvailable(this->connection,
        LG_NET_SERVICE_FILE, LG_NET_CAPABILITY_RELIABLE))
    return NULL;
  *opaque = this->clipboard;
  return networkClipboardGetOps();
}

static LG_TransportStatus networkSendControl(LG_Transport * this,
    const LG_TransportControl * control, LG_TransportControlToken * token)
{
  return networkControlSend(this->control, control, token);
}

static LG_TransportStatus networkControlStatusOp(LG_Transport * this,
    LG_TransportControlToken token)
{
  return networkControlStatus(this->control, token);
}

static LG_TransportStatus networkGetRecoveryInfo(LG_Transport * this,
    LG_RecoveryInfo * info)
{
  return networkRecoveryGetInfo(this->recovery, info);
}

static LG_TransportStatus networkRequestRecovery(LG_Transport * this,
    LG_RecoveryRequest request, uint64_t * instance, uint32_t * serial)
{
  return networkRecoveryRequest(
      this->recovery, request, instance, serial);
}

const LG_TransportOps LGT_Network =
{
  .name               = "network",
  .setup              = networkSetup,
  .create             = networkCreate,
  .destroy            = networkDestroy,
  .connect            = networkConnect,
  .connectCancellable = networkConnectCancellable,
  .disconnect         = networkDisconnect,
  .sessionValid       = networkSessionValid,
  .getVideoOps        = networkGetVideoOps,
  .getInputOps        = networkGetInputOps,
  .getAudioOps        = networkGetAudioOps,
  .getClipboardOps    = networkGetClipboardOps,
  .getRecoveryInfo    = networkGetRecoveryInfo,
  .requestRecovery    = networkRequestRecovery,
  .sendControl        = networkSendControl,
  .controlStatus      = networkControlStatusOp,
};
