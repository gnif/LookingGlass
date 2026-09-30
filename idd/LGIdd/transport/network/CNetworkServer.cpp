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

#include "transport/network/CNetworkServer.h"

#include "transport/network/CNetworkSession.h"
#include "transport/network/CNetworkTransport.h"
#include "transport/network/NetworkSettings.h"

#include "CDebug.h"
#include "CSRWLock.h"
#include "NetworkIdentity.h"
#include <LGProtocol/NetworkProtocol.h>

#include <msquic.h>
#include <bcrypt.h>

#include <algorithm>
#include <limits.h>
#include <memory>
#include <new>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

namespace
{
  static constexpr uint64_t NETWORK_HANDSHAKE_TIMEOUT_MS  = 5000;
  static constexpr uint64_t NETWORK_IDLE_TIMEOUT_MS       = 30000;
  static constexpr uint32_t NETWORK_KEEPALIVE_INTERVAL_MS = 1000;
  static constexpr uint32_t NETWORK_INITIAL_RTT_MS        = 1;
  static constexpr uint16_t NETWORK_PEER_BIDI_STREAMS     = 16;
  static constexpr uint16_t NETWORK_PEER_UNIDI_STREAMS    = 16;
  static constexpr size_t   NETWORK_MAX_HEADER_SIZE       = 4096;
  static constexpr size_t   NETWORK_MAX_CONTROL_PAYLOAD   = 1024 * 1024;
  static constexpr size_t   NETWORK_MAX_CONTROL_BUFFER    =
    NETWORK_MAX_HEADER_SIZE + NETWORK_MAX_CONTROL_PAYLOAD;

  static constexpr QUIC_UINT62 NETWORK_ERROR_PROTOCOL        = 1;
  static constexpr QUIC_UINT62 NETWORK_ERROR_SERVER_STOPPING = 2;

  static const wchar_t NETWORK_REGISTRY_KEY[] =
    L"Software\\LookingGlass\\IDD";

  uint16_t ReadU16(const uint8_t * data)
  {
    return static_cast<uint16_t>(data[0]) |
      static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
  }

  uint32_t ReadU32(const uint8_t * data)
  {
    return static_cast<uint32_t>(data[0])        |
      (static_cast<uint32_t>(data[1]) << 8)      |
      (static_cast<uint32_t>(data[2]) << 16)     |
      (static_cast<uint32_t>(data[3]) << 24);
  }

  bool FillRandom(void * data, size_t size)
  {
    if (!data || size > ULONG_MAX)
      return false;

    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(data),
      static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
  }

  bool MakeAddress(const std::wstring& source, uint16_t port,
    QUIC_ADDR& address)
  {
    if (source == L"*")
    {
      ZeroMemory(&address, sizeof(address));
      QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_UNSPEC);
      QuicAddrSetPort(&address, port);
      return true;
    }

    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
      source.data(), static_cast<int>(source.size()), nullptr, 0,
      nullptr, nullptr);
    if (length <= 0)
      return false;

    std::string text(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        source.data(), static_cast<int>(source.size()), &text[0], length,
        nullptr, nullptr) != length)
      return false;

    ZeroMemory(&address, sizeof(address));
    return QuicAddrFromString(text.c_str(), port, &address) != FALSE;
  }

  bool ReadCertificateHash(QUIC_CERTIFICATE_HASH_STORE& certificate)
  {
    HKEY key = nullptr;
    const LSTATUS openStatus = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
      NETWORK_REGISTRY_KEY, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (openStatus != ERROR_SUCCESS)
    {
      DEBUG_ERROR("Failed to open the network identity registry key: 0x%lx",
        static_cast<unsigned long>(openStatus));
      return false;
    }

    DWORD type = 0;
    DWORD size = sizeof(certificate.ShaHash);
    const LSTATUS readStatus = RegQueryValueExW(key,
      LG_NETWORK_CERTIFICATE_HASH_VALUE, nullptr, &type,
      certificate.ShaHash, &size);
    RegCloseKey(key);
    if (readStatus != ERROR_SUCCESS || type != REG_BINARY ||
        size != sizeof(certificate.ShaHash))
    {
      DEBUG_ERROR("Failed to read the network identity certificate hash: "
        "0x%lx", static_cast<unsigned long>(readStatus));
      return false;
    }

    certificate.Flags = QUIC_CERTIFICATE_HASH_STORE_FLAG_MACHINE_STORE;
    if (strcpy_s(certificate.StoreName,
        sizeof(certificate.StoreName), "MY") != 0)
      return false;
    return true;
  }
}

struct CNetworkServer::Impl
{
  struct Connection;

  struct Send
  {
    QUIC_BUFFER          buffer = {};
    std::vector<uint8_t> data;
  };

  enum class HandshakeState
  {
    WAITING_HELLO,
    READY,
  };

  struct Connection
  {
    explicit Connection(Impl& owner) : owner(owner) {}

    Impl&                            owner;
    CSRWLock                         lock;
    HQUIC                            handle          = nullptr;
    HQUIC                            controlStream   = nullptr;
    uint32_t                         client          = 0;
    uint32_t                         generation      = 0;
    uint64_t                         sessionEpoch    = 0;
    uint64_t                         sendSequence    = 0;
    uint64_t                         receiveSequence = 0;
    uint32_t                         maxPayload      =
      static_cast<uint32_t>(NETWORK_MAX_CONTROL_PAYLOAD);
    bool                             closing         = false;
    HandshakeState                   handshake       =
      HandshakeState::WAITING_HELLO;
    std::shared_ptr<CNetworkSession> session;
    std::vector<uint8_t>             receiveData;
  };

  struct CallbackGuard
  {
    explicit CallbackGuard(Impl& owner) : owner(owner)
    {
      owner.EnterCallback();
    }

    ~CallbackGuard()
    {
      owner.LeaveCallback();
    }

    Impl& owner;
  };

  Impl(CNetworkTransport& transport, const NetworkSettings& settings) :
    transport(transport), settings(settings)
  {
  }

  ~Impl()
  {
    Stop();
    if (drainedEvent)
      CloseHandle(drainedEvent);
  }

  CNetworkTransport&        transport;
  NetworkSettings           settings;
  const QUIC_API_TABLE    * api             = nullptr;
  HQUIC                     registration    = nullptr;
  HQUIC                     configuration   = nullptr;
  HQUIC                     listener        = nullptr;
  HANDLE                    drainedEvent    = nullptr;
  CSRWLock                  lock;
  std::vector<Connection *> connections;
  uint32_t                  nextClient      = 1;
  unsigned                  activeCallbacks = 0;
  bool                      stopping        = true;
  bool                      running         = false;

  void EnterCallback()
  {
    CSRWExclusiveLock guard(lock);
    ++activeCallbacks;
  }

  void LeaveCallback()
  {
    bool drained = false;
    {
      CSRWExclusiveLock guard(lock);
      if (activeCallbacks)
        --activeCallbacks;
      drained = stopping && connections.empty() && !activeCallbacks;
    }
    if (drained && drainedEvent)
      SetEvent(drainedEvent);
  }

  uint32_t AllocateClientLocked()
  {
    for (uint64_t attempt = 0; attempt < UINT32_MAX; ++attempt)
    {
      const uint32_t candidate = nextClient++;
      if (!nextClient)
        nextClient = 1;
      if (!candidate)
        continue;

      bool found = false;
      for (const Connection * connection : connections)
        if (connection->client == candidate)
        {
          found = true;
          break;
        }
      if (!found)
        return candidate;
    }
    return 0;
  }

  void Shutdown(Connection& connection, QUIC_UINT62 error)
  {
    HQUIC handle = nullptr;
    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.closing)
        return;
      connection.closing = true;
      handle             = connection.handle;
    }
    if (handle)
      api->ConnectionShutdown(handle,
        QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, error);
  }

  bool SendPacket(Connection& connection, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
  {
    if ((payloadLength && !payload) ||
        payloadLength > connection.maxPayload)
      return false;

    std::unique_ptr<Send> send(new (std::nothrow) Send());
    if (!send)
      return false;

    const size_t packetSize = LG_NET_ENVELOPE_WIRE_SIZE + payloadLength;
    try
    {
      send->data.resize(packetSize);
    }
    catch (...)
    {
      return false;
    }

    LGNetEnvelope envelope;
    lgNetEnvelopeInit(&envelope, service, messageType,
      LG_NET_CORE_VERSION_CURRENT, LG_NET_MESSAGE_VERSION_CURRENT);
    envelope.flags         = flags | LG_NET_ENVELOPE_RELIABLE;
    envelope.payloadLength = payloadLength;
    envelope.requestID     = requestID;

    send->buffer.Buffer = send->data.data();
    send->buffer.Length = static_cast<uint32_t>(send->data.size());
    QUIC_STATUS status;
    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.closing || !connection.controlStream)
        return false;
      if (connection.sendSequence == UINT64_MAX)
        return false;
      envelope.sessionEpoch = connection.sessionEpoch;
      envelope.sequence     = ++connection.sendSequence;
      if (!lgNetPacketEncode(send->data.data(), send->data.size(),
          &envelope, payload))
        return false;
      status = api->StreamSend(connection.controlStream, &send->buffer, 1,
        QUIC_SEND_FLAG_NONE, send.get());
    }
    if (QUIC_FAILED(status))
      return false;
    send.release();
    return true;
  }

  bool HandleHello(Connection& connection, const LGNetPacketView& packet)
  {
    if (packet.envelope.requestID == 0 ||
        packet.envelope.sessionEpoch != 0 ||
        packet.envelope.componentEpoch != 0)
      return false;

    LGNetHello hello;
    if (lgNetHelloDecode(&hello, packet.payload,
        packet.payloadLength) != LG_NET_PARSE_OK ||
        hello.role != LG_NET_ROLE_CLIENT || hello.capabilityCount != 0 ||
        hello.protocolMin > LG_NET_PROTOCOL_VERSION_CURRENT ||
        hello.protocolMax < LG_NET_PROTOCOL_VERSION_CURRENT ||
        (hello.authModes & ~LG_NET_AUTH_MODES_KNOWN) != 0 ||
        !(hello.authModes & LG_NET_AUTH_MODE_FLAG(LG_NET_AUTH_NONE)))
      return false;

    uint64_t sessionEpoch = 0;
    uint8_t  nonce[LG_NET_HELLO_NONCE_LENGTH];
    if (!FillRandom(&sessionEpoch, sizeof(sessionEpoch)) ||
        !FillRandom(nonce, sizeof(nonce)))
      return false;
    if (!sessionEpoch)
      sessionEpoch = GetTickCount64() | 1;

    std::shared_ptr<CNetworkSession> session = transport.CreateSession(
      connection.client, connection.generation);
    if (!session)
      return false;

    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.closing ||
          connection.handshake != HandshakeState::WAITING_HELLO)
      {
        transport.RemoveSession(connection.client, connection.generation);
        return false;
      }
      connection.session      = std::move(session);
      connection.sessionEpoch = sessionEpoch;
      connection.maxPayload   = hello.maxPayloadLength <
        NETWORK_MAX_CONTROL_PAYLOAD ? hello.maxPayloadLength :
          static_cast<uint32_t>(NETWORK_MAX_CONTROL_PAYLOAD);
    }

    LGNetHelloAck acknowledgement;
    lgNetHelloAckInit(&acknowledgement, sessionEpoch);
    acknowledgement.authMode           = LG_NET_AUTH_NONE;
    acknowledgement.maxPayloadLength   = connection.maxPayload;
    acknowledgement.maxDatagramPayload = 0;
    acknowledgement.capabilityCount    = 0;
    memcpy(acknowledgement.nonce, nonce, sizeof(nonce));

    uint8_t payload[LG_NET_HELLO_ACK_WIRE_SIZE];
    if (!lgNetHelloAckEncode(payload, sizeof(payload), &acknowledgement) ||
        !SendPacket(connection, LG_NET_SERVICE_CORE,
          LG_NET_CORE_MESSAGE_HELLO_ACK,
          LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
          packet.envelope.requestID, payload, sizeof(payload)) ||
        !SendPacket(connection, LG_NET_SERVICE_CORE,
          LG_NET_CORE_MESSAGE_CAPABILITIES_DONE,
          LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
          packet.envelope.requestID, nullptr, 0))
      return false;

    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.closing)
        return false;
      connection.handshake = HandshakeState::READY;
    }
    DEBUG_INFO("Network client %u completed the control handshake",
      connection.client);
    return true;
  }

  bool HandlePacket(Connection& connection, const LGNetPacketView& packet)
  {
    if (packet.envelope.headerSize > NETWORK_MAX_HEADER_SIZE ||
        packet.envelope.payloadLength > NETWORK_MAX_CONTROL_PAYLOAD ||
        packet.envelope.messageVersion != LG_NET_MESSAGE_VERSION_CURRENT ||
        packet.envelope.flags & (LG_NET_ENVELOPE_RESPONSE |
          LG_NET_ENVELOPE_DATAGRAM | LG_NET_ENVELOPE_ERROR |
          LG_NET_ENVELOPE_EARLY_DATA) ||
        !(packet.envelope.flags & LG_NET_ENVELOPE_RELIABLE) ||
        !(packet.envelope.flags & LG_NET_ENVELOPE_FINAL) ||
        !lgNetServiceKnown(packet.envelope.service) ||
        !lgNetMessageKnown(packet.envelope.service,
          packet.envelope.messageType))
      return false;

    HandshakeState handshake;
    uint64_t       sessionEpoch;
    {
      CSRWSharedLock guard(connection.lock);
      handshake    = connection.handshake;
      sessionEpoch = connection.sessionEpoch;
    }

    if (handshake == HandshakeState::WAITING_HELLO)
      return packet.envelope.service == LG_NET_SERVICE_CORE &&
        packet.envelope.messageType == LG_NET_CORE_MESSAGE_HELLO &&
        packet.envelope.serviceVersion == LG_NET_CORE_VERSION_CURRENT &&
        HandleHello(connection, packet);

    if (packet.envelope.sessionEpoch != sessionEpoch ||
        packet.envelope.service != LG_NET_SERVICE_CORE ||
        packet.envelope.serviceVersion != LG_NET_CORE_VERSION_CURRENT)
      return false;

    if (packet.envelope.messageType == LG_NET_CORE_MESSAGE_PING)
      return SendPacket(connection, LG_NET_SERVICE_CORE,
        LG_NET_CORE_MESSAGE_PONG,
        LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
        packet.envelope.requestID, packet.payload,
        static_cast<uint32_t>(packet.payloadLength));

    if (packet.envelope.messageType == LG_NET_CORE_MESSAGE_GOODBYE &&
        packet.payloadLength == 0)
    {
      Shutdown(connection, 0);
      return true;
    }
    return false;
  }

  bool Receive(Connection& connection, const QUIC_BUFFER * buffers,
    uint32_t count)
  {
    size_t additional = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
      if (buffers[i].Length > NETWORK_MAX_CONTROL_BUFFER - additional)
        return false;
      additional += buffers[i].Length;
    }
    if (connection.receiveData.size() >
        NETWORK_MAX_CONTROL_BUFFER - additional)
      return false;

    try
    {
      for (uint32_t i = 0; i < count; ++i)
        connection.receiveData.insert(connection.receiveData.end(),
          buffers[i].Buffer, buffers[i].Buffer + buffers[i].Length);
    }
    catch (...)
    {
      return false;
    }

    while (connection.receiveData.size() >= LG_NET_ENVELOPE_WIRE_SIZE)
    {
      const uint16_t headerSize  = ReadU16(connection.receiveData.data() + 8);
      const uint32_t payloadSize = ReadU32(connection.receiveData.data() + 24);
      if (headerSize < LG_NET_ENVELOPE_WIRE_SIZE ||
          headerSize > NETWORK_MAX_HEADER_SIZE ||
          payloadSize > NETWORK_MAX_CONTROL_PAYLOAD ||
          payloadSize > NETWORK_MAX_CONTROL_BUFFER - headerSize)
        return false;

      const size_t packetSize = static_cast<size_t>(headerSize) + payloadSize;
      if (connection.receiveData.size() < packetSize)
        break;

      LGNetPacketView packet;
      if (lgNetPacketDecode(&packet, connection.receiveData.data(),
          packetSize) != LG_NET_PARSE_OK || packet.wireSize != packetSize)
        return false;

      {
        CSRWExclusiveLock guard(connection.lock);
        if (!packet.envelope.sequence ||
            connection.receiveSequence == UINT64_MAX ||
            packet.envelope.sequence != connection.receiveSequence + 1)
          return false;
        connection.receiveSequence = packet.envelope.sequence;
      }
      if (!HandlePacket(connection, packet))
        return false;

      connection.receiveData.erase(connection.receiveData.begin(),
        connection.receiveData.begin() + packetSize);
    }
    return true;
  }

  void ConnectionClosed(Connection& connection)
  {
    const uint32_t client     = connection.client;
    const uint32_t generation = connection.generation;
    {
      CSRWExclusiveLock guard(lock);
      const auto found = std::find(
        connections.begin(), connections.end(), &connection);
      if (found != connections.end())
        connections.erase(found);
    }

    if (connection.session)
      transport.RemoveSession(client, generation);
    connection.session.reset();
    delete &connection;
    DEBUG_INFO("Network client %u disconnected", client);
  }

  static QUIC_STATUS QUIC_API ListenerCallback(HQUIC handle,
    void * context, QUIC_LISTENER_EVENT * event)
  {
    UNREFERENCED_PARAMETER(handle);
    Impl& owner = *static_cast<Impl *>(context);
    CallbackGuard callback(owner);
    if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION)
      return QUIC_STATUS_SUCCESS;

    Connection * connection = nullptr;
    {
      CSRWExclusiveLock guard(owner.lock);
      if (owner.stopping ||
          owner.connections.size() >= owner.settings.maxClients)
        return QUIC_STATUS_CONNECTION_REFUSED;

      connection = new (std::nothrow) Connection(owner);
      if (!connection)
        return QUIC_STATUS_OUT_OF_MEMORY;
      connection->handle     = event->NEW_CONNECTION.Connection;
      connection->client     = owner.AllocateClientLocked();
      connection->generation = 1;
      if (!connection->client)
      {
        delete connection;
        return QUIC_STATUS_CONNECTION_REFUSED;
      }
      try
      {
        owner.connections.push_back(connection);
      }
      catch (...)
      {
        delete connection;
        return QUIC_STATUS_OUT_OF_MEMORY;
      }
      ResetEvent(owner.drainedEvent);
    }

    owner.api->SetCallbackHandler(event->NEW_CONNECTION.Connection,
      reinterpret_cast<void *>(ConnectionCallback), connection);
    const QUIC_STATUS status = owner.api->ConnectionSetConfiguration(
      event->NEW_CONNECTION.Connection, owner.configuration);
    if (QUIC_FAILED(status))
    {
      {
        CSRWExclusiveLock guard(owner.lock);
        const auto found = std::find(owner.connections.begin(),
          owner.connections.end(), connection);
        if (found != owner.connections.end())
          owner.connections.erase(found);
      }
      delete connection;
    }
    return status;
  }

  static QUIC_STATUS QUIC_API ConnectionCallback(HQUIC handle,
    void * context, QUIC_CONNECTION_EVENT * event)
  {
    Connection& connection = *static_cast<Connection *>(context);
    Impl&       owner      = connection.owner;
    CallbackGuard callback(owner);

    switch (event->Type)
    {
      case QUIC_CONNECTION_EVENT_CONNECTED:
        DEBUG_INFO("Network client %u connected", connection.client);
        return QUIC_STATUS_SUCCESS;

      case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
      {
        const bool invalidFlags =
          event->PEER_STREAM_STARTED.Flags &
            (QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL |
             QUIC_STREAM_OPEN_FLAG_0_RTT);
        bool accepted = false;
        {
          CSRWExclusiveLock guard(connection.lock);
          if (!invalidFlags && !connection.closing &&
              !connection.controlStream)
          {
            connection.controlStream = event->PEER_STREAM_STARTED.Stream;
            accepted                 = true;
          }
        }

        if (accepted)
          owner.api->SetCallbackHandler(event->PEER_STREAM_STARTED.Stream,
            reinterpret_cast<void *>(StreamCallback), &connection);
        else
        {
          owner.api->SetCallbackHandler(event->PEER_STREAM_STARTED.Stream,
            reinterpret_cast<void *>(RejectedStreamCallback), &owner);
          owner.api->StreamShutdown(event->PEER_STREAM_STARTED.Stream,
            QUIC_STREAM_SHUTDOWN_FLAG_ABORT, NETWORK_ERROR_PROTOCOL);
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
        }
        return QUIC_STATUS_SUCCESS;
      }

      case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
        owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
        return QUIC_STATUS_SUCCESS;

      case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
      case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
      {
        CSRWExclusiveLock guard(connection.lock);
        connection.closing = true;
        return QUIC_STATUS_SUCCESS;
      }

      case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        owner.api->ConnectionClose(handle);
        owner.ConnectionClosed(connection);
        return QUIC_STATUS_SUCCESS;

      default:
        return QUIC_STATUS_SUCCESS;
    }
  }

  static QUIC_STATUS QUIC_API StreamCallback(HQUIC handle,
    void * context, QUIC_STREAM_EVENT * event)
  {
    Connection& connection = *static_cast<Connection *>(context);
    Impl&       owner      = connection.owner;
    CallbackGuard callback(owner);

    switch (event->Type)
    {
      case QUIC_STREAM_EVENT_RECEIVE:
        if (!owner.Receive(connection, event->RECEIVE.Buffers,
            event->RECEIVE.BufferCount))
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
        return QUIC_STATUS_SUCCESS;

      case QUIC_STREAM_EVENT_SEND_COMPLETE:
        delete static_cast<Send *>(event->SEND_COMPLETE.ClientContext);
        return QUIC_STATUS_SUCCESS;

      case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
      case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
      case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
        return QUIC_STATUS_SUCCESS;

      case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
      {
        {
          CSRWExclusiveLock guard(connection.lock);
          if (connection.controlStream == handle)
            connection.controlStream = nullptr;
        }
        owner.api->StreamClose(handle);
        return QUIC_STATUS_SUCCESS;
      }

      default:
        return QUIC_STATUS_SUCCESS;
    }
  }

  static QUIC_STATUS QUIC_API RejectedStreamCallback(HQUIC handle,
    void * context, QUIC_STREAM_EVENT * event)
  {
    Impl& owner = *static_cast<Impl *>(context);
    CallbackGuard callback(owner);
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE)
      owner.api->StreamClose(handle);
    return QUIC_STATUS_SUCCESS;
  }

  bool Start()
  {
    {
      CSRWExclusiveLock guard(lock);
      if (running)
        return true;
      stopping = false;
    }

    if (settings.auth != LG_NET_AUTH_NONE)
    {
      DEBUG_ERROR("Network password authentication is configured but the "
        "password proof provider is not implemented; refusing to listen");
      Stop();
      return false;
    }

    if (!drainedEvent)
      drainedEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!drainedEvent || QUIC_FAILED(MsQuicOpen2(&api)))
    {
      DEBUG_ERROR("Failed to initialize the static MsQuic API");
      Stop();
      return false;
    }

    QUIC_REGISTRATION_CONFIG registrationConfig = {};
    registrationConfig.AppName          = "looking-glass-idd";
    registrationConfig.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;
    QUIC_STATUS status = api->RegistrationOpen(
      &registrationConfig, &registration);
    if (QUIC_FAILED(status))
    {
      DEBUG_ERROR("MsQuic RegistrationOpen failed: 0x%lx",
        static_cast<unsigned long>(status));
      Stop();
      return false;
    }

    QUIC_SETTINGS quicSettings = {};
    quicSettings.IsSet.HandshakeIdleTimeoutMs = TRUE;
    quicSettings.IsSet.IdleTimeoutMs          = TRUE;
    quicSettings.IsSet.InitialRttMs           = TRUE;
    quicSettings.IsSet.MaxAckDelayMs          = TRUE;
    quicSettings.IsSet.KeepAliveIntervalMs    = TRUE;
    quicSettings.IsSet.PeerBidiStreamCount    = TRUE;
    quicSettings.IsSet.PeerUnidiStreamCount   = TRUE;
    quicSettings.IsSet.DatagramReceiveEnabled = TRUE;
    quicSettings.IsSet.ServerResumptionLevel  = TRUE;
    quicSettings.HandshakeIdleTimeoutMs       = NETWORK_HANDSHAKE_TIMEOUT_MS;
    quicSettings.IdleTimeoutMs                = NETWORK_IDLE_TIMEOUT_MS;
    quicSettings.InitialRttMs                 = NETWORK_INITIAL_RTT_MS;
    quicSettings.MaxAckDelayMs                = 1;
    quicSettings.KeepAliveIntervalMs          =
      NETWORK_KEEPALIVE_INTERVAL_MS;
    quicSettings.PeerBidiStreamCount          = NETWORK_PEER_BIDI_STREAMS;
    quicSettings.PeerUnidiStreamCount         = NETWORK_PEER_UNIDI_STREAMS;
    quicSettings.DatagramReceiveEnabled       = TRUE;
    quicSettings.ServerResumptionLevel        = QUIC_SERVER_NO_RESUME;

    QUIC_BUFFER alpn = {};
    alpn.Buffer      = reinterpret_cast<uint8_t *>(
      const_cast<char *>(LG_NET_ALPN));
    alpn.Length      = static_cast<uint32_t>(LG_NET_ALPN_LENGTH);
    status = api->ConfigurationOpen(registration, &alpn, 1,
      &quicSettings, sizeof(quicSettings), nullptr, &configuration);
    if (QUIC_FAILED(status))
    {
      DEBUG_ERROR("MsQuic ConfigurationOpen failed: 0x%lx",
        static_cast<unsigned long>(status));
      Stop();
      return false;
    }

    QUIC_CERTIFICATE_HASH_STORE certificate = {};
    if (!ReadCertificateHash(certificate))
    {
      DEBUG_ERROR("The persistent network identity is unavailable; run the "
        "Looking Glass IDD installer to provision it");
      Stop();
      return false;
    }

    QUIC_CREDENTIAL_CONFIG credential = {};
    credential.Type                 =
      QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE;
    credential.Flags                = QUIC_CREDENTIAL_FLAG_NONE;
    credential.CertificateHashStore = &certificate;
    status = api->ConfigurationLoadCredential(configuration, &credential);
    if (QUIC_FAILED(status))
    {
      DEBUG_ERROR("MsQuic failed to load the network identity: 0x%lx",
        static_cast<unsigned long>(status));
      Stop();
      return false;
    }

    status = api->ListenerOpen(
      registration, ListenerCallback, this, &listener);
    if (QUIC_FAILED(status))
    {
      DEBUG_ERROR("MsQuic ListenerOpen failed: 0x%lx",
        static_cast<unsigned long>(status));
      Stop();
      return false;
    }

    QUIC_ADDR address = {};
    if (!MakeAddress(settings.bindAddress, settings.port, address))
    {
      DEBUG_ERROR("Invalid network bind address: %ls",
        settings.bindAddress.c_str());
      Stop();
      return false;
    }
    status = api->ListenerStart(listener, &alpn, 1, &address);
    if (QUIC_FAILED(status))
    {
      DEBUG_ERROR("MsQuic ListenerStart failed on [%ls]:%u: 0x%lx",
        settings.bindAddress.c_str(),
        static_cast<unsigned>(settings.port),
        static_cast<unsigned long>(status));
      Stop();
      return false;
    }

    {
      CSRWExclusiveLock guard(lock);
      running = true;
    }
    DEBUG_INFO("Network transport listening on [%ls]:%u",
      settings.bindAddress.c_str(), static_cast<unsigned>(settings.port));
    return true;
  }

  void Stop()
  {
    {
      CSRWExclusiveLock guard(lock);
      stopping = true;
      running  = false;
      if (connections.empty() && !activeCallbacks && drainedEvent)
        SetEvent(drainedEvent);
    }

    if (listener && api)
    {
      api->ListenerClose(listener);
      listener = nullptr;
    }
    if (registration && api)
      api->RegistrationShutdown(registration,
        QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT,
        NETWORK_ERROR_SERVER_STOPPING);
    if (drainedEvent)
      WaitForSingleObject(drainedEvent, INFINITE);

    if (configuration && api)
    {
      api->ConfigurationClose(configuration);
      configuration = nullptr;
    }
    if (registration && api)
    {
      api->RegistrationClose(registration);
      registration = nullptr;
    }
    if (api)
    {
      MsQuicClose(api);
      api = nullptr;
    }
  }
};

CNetworkServer::CNetworkServer(CNetworkTransport& transport,
  const NetworkSettings& settings) :
  m_impl(new Impl(transport, settings))
{
}

CNetworkServer::~CNetworkServer() = default;

bool CNetworkServer::Start()
{
  return m_impl && m_impl->Start();
}

void CNetworkServer::Stop()
{
  if (m_impl)
    m_impl->Stop();
}
