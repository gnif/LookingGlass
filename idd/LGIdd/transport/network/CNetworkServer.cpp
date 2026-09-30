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
#include "transport/network/NetworkAuthCrypto.h"
#include "transport/network/NetworkSettings.h"

#include "CDebug.h"
#include "CSRWLock.h"
#include "VersionInfo.h"
#include <LGProtocol/NetworkAuth.h>
#include <LGProtocol/NetworkIdentity.h>
#include <LGProtocol/NetworkPacket.h>
#include <LGProtocol/NetworkRoute.h>
#include <LGProtocol/NetworkSequence.h>
#include <LGProtocol/NetworkServices.h>
#include <LGProtocol/NetworkStream.h>

#include <msquic.h>

#include <algorithm>
#include <limits.h>
#include <memory>
#include <new>
#include <ncrypt.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>
#include <wincrypt.h>

namespace
{
  static constexpr uint64_t NETWORK_HANDSHAKE_TIMEOUT_MS     = 5000;
  static constexpr uint64_t NETWORK_IDLE_TIMEOUT_MS          = 30000;
  static constexpr uint32_t NETWORK_KEEPALIVE_INTERVAL_MS    = 1000;
  static constexpr uint32_t NETWORK_INITIAL_RTT_MS           = 1;
  static constexpr uint16_t NETWORK_PEER_BIDI_STREAMS        = 16;
  static constexpr uint16_t NETWORK_PEER_UNIDI_STREAMS       = 16;
  static constexpr uint16_t NETWORK_BULK_STREAMS_PER_SERVICE = 1;
  static constexpr uint32_t NETWORK_MAX_CONTROL_PAYLOAD      =
    LG_NET_CURSOR_SHAPE_HEADER_WIRE_SIZE +
      LG_NET_CURSOR_MAX_SHAPE_LENGTH;
  static constexpr size_t   NETWORK_MAX_CONTROL_BUFFER       =
    LG_NET_ENVELOPE_WIRE_SIZE + NETWORK_MAX_CONTROL_PAYLOAD;
  static constexpr uint32_t NETWORK_MAX_DATAGRAM_PAYLOAD     = 1136;
  static constexpr uint32_t NETWORK_INPUT_LEASE_MS           = 500;
  static constexpr uint8_t  NETWORK_KEYBOARD_ROLLOVER        = 0x01;
  static constexpr uint16_t NETWORK_KEYBOARD_USAGE_PAGE      = 0x07;
  static constexpr size_t   NETWORK_KEYBOARD_KEY_COUNT       = 6;
  static constexpr size_t   NETWORK_SERVICE_COUNT            =
    LG_NET_SERVICE_CONTROL + 1U;
  static constexpr size_t   NETWORK_RECOVERY_REQUESTS        = 16;
  static constexpr uint64_t NETWORK_SEQUENCE_COMPONENT       = UINT64_C(1);
  static constexpr uint8_t  NETWORK_DATAGRAM_WINDOW          = 64;
  static constexpr uint32_t NETWORK_PASSWORD_ROUNDS          = 100000;
  static constexpr uint16_t NETWORK_PASSWORD_SALT_SIZE       = 32;
  static constexpr uint16_t NETWORK_PASSWORD_CHALLENGE_SIZE  = 32;
  static constexpr uint16_t NETWORK_PASSWORD_NONCE_SIZE      = 32;
  static constexpr uint32_t NETWORK_AUTH_RETRY_AFTER_MS      = 1000;

  static constexpr uint32_t NETWORK_SEND_RELIABLE_COUNT_MAX      = 512;
  static constexpr uint64_t NETWORK_SEND_RELIABLE_BYTES_MAX      =
    UINT64_C(128) * 1024U * 1024U;
  static constexpr uint32_t NETWORK_SEND_RELIABLE_COUNT_RESERVED = 64;
  static constexpr uint64_t NETWORK_SEND_RELIABLE_BYTES_RESERVED =
    UINT64_C(32) * 1024U * 1024U;
  static constexpr uint32_t NETWORK_SEND_DATAGRAM_COUNT_MAX      = 512;
  static constexpr uint64_t NETWORK_SEND_DATAGRAM_BYTES_MAX      =
    UINT64_C(1) * 1024U * 1024U;
  static constexpr uint32_t NETWORK_SEND_DATAGRAM_COUNT_RESERVED = 64;
  static constexpr uint64_t NETWORK_SEND_DATAGRAM_BYTES_RESERVED =
    UINT64_C(256) * 1024U;
  static constexpr uint32_t NETWORK_SEND_BULK_COUNT_MAX          = 256;
  static constexpr uint64_t NETWORK_SEND_BULK_BYTES_MAX          =
    UINT64_C(256) * 1024U * 1024U;
  static constexpr uint32_t NETWORK_SEND_RESUME_NUMERATOR        = 3;
  static constexpr uint32_t NETWORK_SEND_RESUME_DENOMINATOR      = 4;

  static constexpr uint32_t NETWORK_KEYBOARD_LED_MASK        =
    LG_NET_KEYBOARD_LED_NUM_LOCK    |
    LG_NET_KEYBOARD_LED_CAPS_LOCK   |
    LG_NET_KEYBOARD_LED_SCROLL_LOCK |
    LG_NET_KEYBOARD_LED_COMPOSE     |
    LG_NET_KEYBOARD_LED_KANA;

  static constexpr QUIC_UINT62 NETWORK_ERROR_PROTOCOL          = 1;
  static constexpr QUIC_UINT62 NETWORK_ERROR_SERVER_STOPPING   = 2;
  static constexpr QUIC_UINT62 NETWORK_ERROR_HANDSHAKE_TIMEOUT = 3;

  static const wchar_t NETWORK_REGISTRY_KEY[] =
    L"Software\\LookingGlass\\IDD";

  static_assert(NetworkAuthCrypto::SHA256_SIZE ==
    LG_NET_PASSWORD_SHA256_LENGTH, "password proof size mismatch");
  static_assert(NETWORK_MAX_CONTROL_PAYLOAD <= LG_NET_MAX_PAYLOAD_LENGTH,
    "control payload limit exceeds the protocol maximum");

  uint64_t NetworkTimeNs()
  {
    const uint64_t timestamp = GetTickCount64() * UINT64_C(1000000);
    return timestamp ? timestamp : 1;
  }

  bool RandomNonzero(void * data, size_t size)
  {
    if (!data || !size)
      return false;

    for (unsigned attempt = 0; attempt < 2; ++attempt)
    {
      if (!NetworkAuthCrypto::RandomBytes(data, size))
        return false;

      const auto * bytes = static_cast<const uint8_t *>(data);
      for (size_t index = 0; index < size; ++index)
        if (bytes[index])
          return true;
    }

    NetworkAuthCrypto::SecureZero(data, size);
    return false;
  }

  bool PasswordUTF8(const std::wstring& source,
    std::vector<uint8_t>& password)
  {
    if (source.empty() || source.size() > INT_MAX)
      return false;

    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
      source.data(), static_cast<int>(source.size()), nullptr, 0,
      nullptr, nullptr);
    if (length <= 0)
      return false;

    try
    {
      password.resize(static_cast<size_t>(length));
    }
    catch (...)
    {
      return false;
    }

    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        source.data(), static_cast<int>(source.size()),
        reinterpret_cast<char *>(password.data()), length,
        nullptr, nullptr) == length)
      return true;

    NetworkAuthCrypto::SecureZero(password.data(), password.size());
    password.clear();
    return false;
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

  bool ReadCertificateHash(QUIC_CERTIFICATE_HASH& certificate)
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

    return true;
  }

  bool LoadCertificateContext(HCERTSTORE& store,
    PCCERT_CONTEXT& certificate)
  {
    QUIC_CERTIFICATE_HASH certificateHash = {};
    if (!ReadCertificateHash(certificateHash))
      return false;

    store = CertOpenStore(
      CERT_STORE_PROV_SYSTEM_W,
      0,
      0,
      CERT_SYSTEM_STORE_LOCAL_MACHINE |
        CERT_STORE_OPEN_EXISTING_FLAG |
        CERT_STORE_READONLY_FLAG,
      L"MY");
    if (!store)
    {
      DEBUG_ERROR("Failed to open the network identity certificate store: "
        "0x%lx", static_cast<unsigned long>(GetLastError()));
      return false;
    }

    CRYPT_HASH_BLOB hash  = {};
    hash.cbData           = static_cast<DWORD>(
      sizeof(certificateHash.ShaHash));
    hash.pbData           = certificateHash.ShaHash;
    certificate           = CertFindCertificateInStore(store,
      X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_HASH,
      &hash, nullptr);
    if (!certificate)
    {
      const DWORD status = GetLastError();
      DEBUG_ERROR("Failed to find the network identity certificate: 0x%lx",
        static_cast<unsigned long>(status));
      CertCloseStore(store, 0);
      store = nullptr;
      return false;
    }

    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE privateKey  = 0;
    DWORD                           keySpec     = 0;
    BOOL                            callerFree  = FALSE;
    const DWORD                     flags       =
      CRYPT_ACQUIRE_CACHE_FLAG           |
      CRYPT_ACQUIRE_COMPARE_KEY_FLAG     |
      CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG |
      CRYPT_ACQUIRE_SILENT_FLAG;
    if (!CryptAcquireCertificatePrivateKey(certificate,
        flags,
        nullptr, &privateKey, &keySpec, &callerFree))
    {
      const DWORD status = GetLastError();
      DEBUG_ERROR("Failed to acquire the network identity private key: "
        "0x%lx", static_cast<unsigned long>(status));
      CertFreeCertificateContext(certificate);
      CertCloseStore(store, 0);
      certificate = nullptr;
      store       = nullptr;
      return false;
    }

    if (!privateKey || keySpec != CERT_NCRYPT_KEY_SPEC || callerFree)
    {
      DEBUG_ERROR("The network identity did not provide a cached CNG "
        "private key");
      if (callerFree && privateKey)
        NCryptFreeObject(static_cast<NCRYPT_HANDLE>(privateKey));
      CertFreeCertificateContext(certificate);
      CertCloseStore(store, 0);
      certificate = nullptr;
      store       = nullptr;
      return false;
    }

    return true;
  }
}

struct CNetworkServer::Impl
{
  struct Connection;
  struct BulkStream;

  enum class Delivery
  {
    RELIABLE,
    DATAGRAM,
    BULK,
  };

  struct Send
  {
    QUIC_BUFFER          buffer    = {};
    std::vector<uint8_t> data;
    LGNetService         service   = LG_NET_SERVICE_CORE;
    uint32_t             length    = 0;
    Delivery             delivery  = Delivery::RELIABLE;
    bool                 accounted = true;
  };

  struct SendLimits
  {
    uint32_t count;
    uint64_t bytes;
  };

  struct SendAccounting
  {
    uint32_t count;
    uint64_t bytes;
    uint32_t serviceCount[NETWORK_SERVICE_COUNT];
    uint64_t serviceBytes[NETWORK_SERVICE_COUNT];
    bool     serviceBlocked[NETWORK_SERVICE_COUNT];
    uint64_t serviceBlockedBytes[NETWORK_SERVICE_COUNT];
    uint16_t writableServices;
  };

  struct BulkStream
  {
    explicit BulkStream(Connection& connection) : connection(connection) {}

    Connection&          connection;
    BulkStream         * next             = nullptr;
    HQUIC                handle           = nullptr;
    LGNetService         service          = LG_NET_SERVICE_CORE;
    uint64_t             streamID         = 0;
    uint64_t             sendSequence     = 0;
    uint64_t             receiveSequence  = 0;
    bool                 outbound         = false;
    bool                 bound            = false;
    bool                 openingReturned  = false;
    bool                 shutdownComplete = false;
    uint8_t              bindingData[LG_NET_STREAM_BINDING_WIRE_SIZE] = {};
    size_t               bindingLength                                = 0;
    LGNetStreamParser    receiveParser                                = {};

    std::vector<uint8_t> receiveData;
    std::vector<uint8_t> receivePayload;
  };

  enum class SendResult
  {
    SENT,
    BLOCKED,
    FAILED,
  };

  enum class HandshakeState
  {
    WAITING_HELLO,
    WAITING_CAPABILITIES,
    WAITING_AUTH_RESPONSE,
    AUTHENTICATING,
    PUBLISHING_CAPABILITIES,
    AUTH_FAILED,
    READY,
  };

  struct Connection
  {
    struct RecoveryRequest
    {
      uint64_t requestID  = 0;
      uint64_t sessionID  = 0;
      uint64_t deadline   = 0;
      uint64_t sequence   = 0;
      uint32_t serial     = 0;
      bool     active     = false;
      bool     dispatched = false;
    };

    explicit Connection(Impl& owner) : owner(owner) {}

    ~Connection()
    {
      NetworkAuthCrypto::SecureZero(clientNonce, sizeof(clientNonce));
      NetworkAuthCrypto::SecureZero(serverNonce, sizeof(serverNonce));
      NetworkAuthCrypto::SecureZero(
        &passwordChallenge, sizeof(passwordChallenge));
    }

    Impl&                            owner;
    CSRWLock                         lock;
    HQUIC                            handle                       = nullptr;
    HQUIC                            controlStream                = nullptr;
    uint32_t                         client                       = 0;
    uint32_t                         generation                   = 0;
    uint64_t                         sessionEpoch                 = 0;
    uint64_t                         sendSequence                 = 0;
    uint64_t                         datagramSendSequence         = 0;
    uint64_t                         receiveSequence              = 0;
    uint64_t                         handshakeRequestID           = 0;
    uint64_t                         handshakeDeadline            = 0;
    uint32_t                         maxPayload                   =
      static_cast<uint32_t>(NETWORK_MAX_CONTROL_PAYLOAD);
    uint32_t                         maxDatagramPayload           = 0;
    uint16_t                         maxDatagramSendLength        = 0;
    uint32_t                         expectedCapabilities         = 0;
    uint32_t                         receivedCapabilities         = 0;
    uint32_t                         selectedCapabilityCount      = 0;
    uint16_t                         offeredServices              = 0;
    uint16_t                         selectedServices             = 0;
    uint16_t                         reliableServices             = 0;
    uint16_t                         datagramServices             = 0;
    uint16_t                         bulkServices                 = 0;
    uint16_t                         maxPeerBulkStreams           = 0;
    uint16_t                         peerBulkStreams              = 0;
    uint16_t                         servicePeerBulkStreams[NETWORK_SERVICE_COUNT] = {};
    LGNetCapability                  offeredCapabilities   [NETWORK_SERVICE_COUNT] = {};
    LGNetCapability                  selectedCapabilities  [NETWORK_SERVICE_COUNT] = {};

    SendAccounting                   reliableSend                = {};
    SendAccounting                   datagramSend                = {};
    SendAccounting                   bulkSend                    = {};

    BulkStream                     * outboundBulk[NETWORK_SERVICE_COUNT] = {};
    BulkStream                     * inboundBulk                         = nullptr;

    bool                             datagramSendEnabled          = false;
    bool                             closing                      = false;
    HandshakeState                   handshake                    =
      HandshakeState::WAITING_HELLO;
    uint64_t                         inputClaimantID              = 0;
    uint64_t                         inputClaimEpoch              = 0;
    uint64_t                         inputReliableSequence        = 0;
    uint64_t                         inputDatagramSequence        = 0;
    uint64_t                         inputLEDSequence             = 0;
    LGNetInputClaimFlags             inputFlags                   = 0;
    LGNetPointerButtons              inputPointerButtons          = 0;
    LGNetKeyboardLEDs                inputLEDState                = 0;
    uint8_t                          keyboardModifiers            = 0;
    bool                             inputLEDStateKnown           = false;
    bool                             keyboardKeys[256]            = {};

    RecoveryRequest                  recoveryRequests[NETWORK_RECOVERY_REQUESTS] = {};

    uint64_t                         recoveryStatusSequence       = 1;
    uint64_t                         recoveryNextRequestSequence  = 1;
    uint32_t                         recoveryNextSerial           = 1;
    LGNetRecoveryState               recoveryState                =
      LG_NET_RECOVERY_STATE_UNKNOWN;
    LGNetRecoveryError               recoveryError                =
      LG_NET_RECOVERY_ERROR_NONE;
    LGNetRecoveryFlags               recoveryFlags                =
      LG_NET_RECOVERY_SUPPORTED;

    uint64_t                         cursorSnapshotRevision       = 0;
    uint64_t                         cursorShapeRevision          = 0;
    uint64_t                         cursorPositionRevision       = 0;
    uint64_t                         cursorColorRevision          = 0;
    uint64_t                         cursorStateSequence          = 0;
    uint64_t                         cursorAppliedStateSequence   = 0;
    uint64_t                         cursorAppliedShapeID         = 0;
    uint64_t                         cursorAppliedPositionID      = 0;
    uint64_t                         cursorAppliedTransformID     = 0;

    uint8_t                          clientNonce[LG_NET_HELLO_NONCE_LENGTH] = {};
    uint8_t                          serverNonce[LG_NET_HELLO_NONCE_LENGTH] = {};

    LGNetPasswordChallenge           passwordChallenge = {};

    std::shared_ptr<CNetworkSession> session;
    LGNetSequenceTracker             datagramReceive              = {};
    LGNetStreamParser                receiveParser                = {};
    std::vector<uint8_t>             receiveData;
    std::vector<uint8_t>             receivePayload;
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

  Impl(CNetworkTransport& transport, CNetworkAudio& audio,
      CNetworkClipboard& clipboard, CNetworkVideo& video,
      const NetworkSettings& settings) :
    transport(transport),
    audio(audio),
    clipboard(clipboard),
    video(video),
    settings(settings)
  {
  }

  ~Impl()
  {
    Stop();
    if (!settings.password.empty())
    {
      NetworkAuthCrypto::SecureZero(&settings.password[0],
        settings.password.size() * sizeof(settings.password[0]));
      settings.password.clear();
    }
    if (drainedEvent)
      CloseHandle(drainedEvent);
  }

  CNetworkTransport&        transport;
  CNetworkAudio&            audio;
  CNetworkClipboard&        clipboard;
  CNetworkVideo&            video;
  NetworkSettings           settings;
  const QUIC_API_TABLE    * api                = nullptr;
  HQUIC                     registration       = nullptr;
  HQUIC                     configuration      = nullptr;
  HQUIC                     listener           = nullptr;
  HCERTSTORE                certificateStore   = nullptr;
  PCCERT_CONTEXT            certificate        = nullptr;
  HANDLE                    drainedEvent       = nullptr;
  CSRWLock                  lock;
  std::vector<Connection *> connections;
  uint32_t                  nextClient         = 1;
  unsigned                  activeCallbacks    = 0;
  bool                      stopping           = true;
  bool                      running            = false;

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

  static NetworkAudioPeer AudioPeer(const Connection& connection)
  {
    return
    {
      connection.client,
      connection.generation,
    };
  }

  static NetworkClipboardPeer ClipboardPeer(const Connection& connection)
  {
    return
    {
      connection.client,
      connection.generation,
    };
  }

  static NetworkVideoPeer VideoPeer(const Connection& connection)
  {
    return
    {
      connection.client,
      connection.generation,
    };
  }

  static bool PeerInfoAvailableLocked(const Connection& connection)
  {
    return connection.handshake == HandshakeState::READY ||
      (connection.handshake == HandshakeState::PUBLISHING_CAPABILITIES &&
        connection.session);
  }

  bool AudioPeerInfo(const NetworkAudioPeer& peer,
    NetworkAudioPeerInfo& info)
  {
    CSRWSharedLock guard(lock);
    for (Connection * connection : connections)
    {
      if (connection->client != peer.client ||
          connection->generation != peer.generation)
        continue;

      CSRWSharedLock connectionGuard(connection->lock);
      const uint16_t serviceMask =
        UINT16_C(1) << LG_NET_SERVICE_AUDIO;
      if (connection->closing ||
          !PeerInfoAvailableLocked(*connection) ||
          !(connection->selectedServices & serviceMask))
        return false;

      const LGNetCapability& capability =
        connection->selectedCapabilities[LG_NET_SERVICE_AUDIO];
      info.serviceVersion      = capability.versionMin;
      info.capabilityFlags     = capability.flags;
      info.maxReliablePayload  = capability.maxPayloadLength;
      info.maxDatagramPayload  = capability.maxDatagramPayload;
      return true;
    }
    return false;
  }

  NetworkAudioSendResult SendAudio(
    const NetworkAudioPeer& peer, bool datagram, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
  {
    if (service != LG_NET_SERVICE_AUDIO)
      return NetworkAudioSendResult::FAILED;

    CSRWSharedLock guard(lock);
    for (Connection * connection : connections)
    {
      if (connection->client != peer.client ||
          connection->generation != peer.generation)
        continue;

      const SendResult result = datagram ?
        TrySendDatagram(*connection, service, messageType, flags,
          componentEpoch, requestID, payload, payloadLength, true) :
        TrySendPacket(*connection, service, messageType, flags,
          componentEpoch, requestID, payload, payloadLength);
      switch (result)
      {
        case SendResult::SENT:
          return NetworkAudioSendResult::ACCEPTED;

        case SendResult::BLOCKED:
          return NetworkAudioSendResult::BLOCKED;

        case SendResult::FAILED:
          return NetworkAudioSendResult::FAILED;
      }
    }
    return NetworkAudioSendResult::FAILED;
  }

  bool ClipboardPeerInfo(const NetworkClipboardPeer& peer,
    NetworkClipboardPeerInfo& info)
  {
    CSRWSharedLock guard(lock);
    for (Connection * connection : connections)
    {
      if (connection->client != peer.client ||
          connection->generation != peer.generation)
        continue;

      CSRWSharedLock connectionGuard(connection->lock);
      const uint16_t clipboardMask =
        UINT16_C(1) << LG_NET_SERVICE_CLIPBOARD;
      const uint16_t fileMask      = UINT16_C(1) << LG_NET_SERVICE_FILE;
      if (connection->closing ||
          !PeerInfoAvailableLocked(*connection) ||
          !(connection->selectedServices & clipboardMask) ||
          !(connection->selectedServices & fileMask))
        return false;

      const LGNetCapability& clipboardCapability =
        connection->selectedCapabilities[LG_NET_SERVICE_CLIPBOARD];
      const LGNetCapability& fileCapability =
        connection->selectedCapabilities[LG_NET_SERVICE_FILE];
      info.clipboardVersion         = clipboardCapability.versionMin;
      info.fileVersion              = fileCapability.versionMin;
      info.clipboardFlags           = clipboardCapability.flags;
      info.fileFlags                = fileCapability.flags;
      info.clipboardReliablePayload = clipboardCapability.maxPayloadLength;
      info.fileReliablePayload      = fileCapability.maxPayloadLength;
      info.clipboardBulkPayload     =
        clipboardCapability.flags & LG_NET_CAPABILITY_BULK ?
          clipboardCapability.maxPayloadLength : 0;
      info.fileBulkPayload          =
        fileCapability.flags & LG_NET_CAPABILITY_BULK ?
          fileCapability.maxPayloadLength : 0;
      return true;
    }
    return false;
  }

  NetworkClipboardSendResult SendClipboard(
    const NetworkClipboardPeer& peer, bool bulk, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
  {
    if (service != LG_NET_SERVICE_CLIPBOARD &&
        service != LG_NET_SERVICE_FILE)
      return NetworkClipboardSendResult::FAILED;

    CSRWSharedLock guard(lock);
    for (Connection * connection : connections)
    {
      if (connection->client != peer.client ||
          connection->generation != peer.generation)
        continue;

      const SendResult result = bulk ?
        TrySendBulk(*connection, service, messageType, flags,
          componentEpoch, requestID, payload, payloadLength) :
        TrySendPacket(*connection, service, messageType, flags,
          componentEpoch, requestID, payload, payloadLength);
      switch (result)
      {
        case SendResult::SENT:
          return NetworkClipboardSendResult::ACCEPTED;

        case SendResult::BLOCKED:
          return NetworkClipboardSendResult::BLOCKED;

        case SendResult::FAILED:
          return NetworkClipboardSendResult::FAILED;
      }
    }
    return NetworkClipboardSendResult::FAILED;
  }

  bool VideoPeerInfo(const NetworkVideoPeer& peer,
    NetworkVideoPeerInfo& info)
  {
    CSRWSharedLock guard(lock);
    for (Connection * connection : connections)
    {
      if (connection->client != peer.client ||
          connection->generation != peer.generation)
        continue;

      CSRWSharedLock connectionGuard(connection->lock);
      const uint16_t serviceMask = UINT16_C(1) << LG_NET_SERVICE_VIDEO;
      if (connection->closing ||
          !PeerInfoAvailableLocked(*connection) ||
          !(connection->selectedServices & serviceMask))
        return false;

      const LGNetCapability& capability =
        connection->selectedCapabilities[LG_NET_SERVICE_VIDEO];
      info.serviceVersion     = capability.versionMin;
      info.capabilityFlags    = capability.flags;
      info.maxReliablePayload = capability.maxPayloadLength;
      info.maxDatagramPayload = capability.maxDatagramPayload;
      info.maxBulkPayload     =
        capability.flags & LG_NET_CAPABILITY_BULK ?
          capability.maxPayloadLength : 0;
      return true;
    }
    return false;
  }

  NetworkVideoSendResult SendVideo(
    const NetworkVideoPeer& peer, NetworkVideoDelivery delivery,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
  {
    CSRWSharedLock guard(lock);
    for (Connection * connection : connections)
    {
      if (connection->client != peer.client ||
          connection->generation != peer.generation)
        continue;

      SendResult result = SendResult::FAILED;
      switch (delivery)
      {
        case NetworkVideoDelivery::RELIABLE:
          result = TrySendPacket(*connection, LG_NET_SERVICE_VIDEO,
            messageType, flags, componentEpoch, requestID,
            payload, payloadLength);
          break;

        case NetworkVideoDelivery::DATAGRAM:
          result = TrySendDatagram(*connection, LG_NET_SERVICE_VIDEO,
            messageType, flags, componentEpoch, requestID,
            payload, payloadLength, true);
          break;

        case NetworkVideoDelivery::BULK:
          result = TrySendBulk(*connection, LG_NET_SERVICE_VIDEO,
            messageType, flags, componentEpoch, requestID,
            payload, payloadLength);
          break;
      }

      switch (result)
      {
        case SendResult::SENT:
          return NetworkVideoSendResult::ACCEPTED;

        case SendResult::BLOCKED:
          return NetworkVideoSendResult::BLOCKED;

        case SendResult::FAILED:
          return NetworkVideoSendResult::FAILED;
      }
    }
    return NetworkVideoSendResult::FAILED;
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

  static SendLimits GetSendLimits(LGNetService service, Delivery delivery)
  {
    if (delivery == Delivery::DATAGRAM)
    {
      switch (service)
      {
        case LG_NET_SERVICE_VIDEO:
          return { 256U, 768U * 1024U };

        case LG_NET_SERVICE_AUDIO:
          return { 256U, 512U * 1024U };

        case LG_NET_SERVICE_INPUT:
          return { 128U, 256U * 1024U };

        default:
          return { 64U, 256U * 1024U };
      }
    }

    if (delivery == Delivery::BULK)
    {
      switch (service)
      {
        case LG_NET_SERVICE_VIDEO:
          return { 128U, UINT64_C(192) * 1024U * 1024U };

        case LG_NET_SERVICE_CLIPBOARD:
        case LG_NET_SERVICE_FILE:
        case LG_NET_SERVICE_USB:
          return { 128U, UINT64_C(128) * 1024U * 1024U };

        default:
          return { 64U, UINT64_C(64) * 1024U * 1024U };
      }
    }

    switch (service)
    {
      case LG_NET_SERVICE_CURSOR:
        return { 8U, UINT64_C(32) * 1024U * 1024U };

      case LG_NET_SERVICE_VIDEO:
        return { 64U, UINT64_C(72) * 1024U * 1024U };

      case LG_NET_SERVICE_AUDIO:
        return { 256U, UINT64_C(16) * 1024U * 1024U };

      case LG_NET_SERVICE_CLIPBOARD:
      case LG_NET_SERVICE_FILE:
        return { 64U, UINT64_C(32) * 1024U * 1024U };

      case LG_NET_SERVICE_CORE:
        return { 64U, UINT64_C(20) * 1024U * 1024U };

      default:
        return { 64U, UINT64_C(20) * 1024U * 1024U };
    }
  }

  static bool SendPriority(LGNetService service)
  {
    return service == LG_NET_SERVICE_CORE ||
      service == LG_NET_SERVICE_RECOVERY ||
      service == LG_NET_SERVICE_CURSOR ||
      service == LG_NET_SERVICE_INPUT ||
      service == LG_NET_SERVICE_CONTROL;
  }

  static bool BulkServiceSupported(LGNetService service)
  {
    return service == LG_NET_SERVICE_VIDEO ||
      service == LG_NET_SERVICE_CLIPBOARD ||
      service == LG_NET_SERVICE_FILE;
  }

  static bool BulkOriginAllowed(LGNetService service, LGNetRole origin)
  {
    if (service == LG_NET_SERVICE_VIDEO)
      return origin == LG_NET_ROLE_SERVER;
    return (service == LG_NET_SERVICE_CLIPBOARD ||
      service == LG_NET_SERVICE_FILE) &&
      (origin == LG_NET_ROLE_CLIENT || origin == LG_NET_ROLE_SERVER);
  }

  static SendAccounting * GetSendAccountingLocked(Connection& connection,
    Delivery delivery)
  {
    switch (delivery)
    {
      case Delivery::RELIABLE:
        return &connection.reliableSend;

      case Delivery::DATAGRAM:
        return &connection.datagramSend;

      case Delivery::BULK:
        return &connection.bulkSend;
    }

    return nullptr;
  }

  static void BlockSendLocked(Connection& connection,
    LGNetService service, Delivery delivery, uint64_t packetSize)
  {
    SendAccounting * accounting =
      GetSendAccountingLocked(connection, delivery);
    if (!accounting || service >= NETWORK_SERVICE_COUNT)
      return;

    accounting->serviceBlocked[service]      = true;
    accounting->serviceBlockedBytes[service] =
      accounting->serviceBlockedBytes[service] > packetSize ?
        accounting->serviceBlockedBytes[service] : packetSize;
    accounting->writableServices            &= ~(UINT16_C(1) << service);
  }

  static SendResult ReserveSendLocked(Connection& connection,
    LGNetService service, Delivery delivery, uint64_t packetSize)
  {
    if (service >= NETWORK_SERVICE_COUNT)
      return SendResult::FAILED;

    const SendLimits limits = GetSendLimits(service, delivery);
    uint32_t         totalCountLimit;
    uint64_t         totalByteLimit;
    switch (delivery)
    {
      case Delivery::RELIABLE:
        totalCountLimit = NETWORK_SEND_RELIABLE_COUNT_MAX;
        totalByteLimit  = NETWORK_SEND_RELIABLE_BYTES_MAX;
        break;

      case Delivery::DATAGRAM:
        totalCountLimit = NETWORK_SEND_DATAGRAM_COUNT_MAX;
        totalByteLimit  = NETWORK_SEND_DATAGRAM_BYTES_MAX;
        break;

      case Delivery::BULK:
        totalCountLimit = NETWORK_SEND_BULK_COUNT_MAX;
        totalByteLimit  = NETWORK_SEND_BULK_BYTES_MAX;
        break;

      default:
        return SendResult::FAILED;
    }
    if (delivery != Delivery::BULK && !SendPriority(service))
    {
      totalCountLimit -= delivery == Delivery::DATAGRAM ?
        NETWORK_SEND_DATAGRAM_COUNT_RESERVED :
        NETWORK_SEND_RELIABLE_COUNT_RESERVED;
      totalByteLimit  -= delivery == Delivery::DATAGRAM ?
        NETWORK_SEND_DATAGRAM_BYTES_RESERVED :
        NETWORK_SEND_RELIABLE_BYTES_RESERVED;
    }
    SendAccounting * accounting =
      GetSendAccountingLocked(connection, delivery);
    if (!accounting)
      return SendResult::FAILED;

    if (packetSize > limits.bytes || packetSize > totalByteLimit)
      return SendResult::FAILED;

    if (accounting->count >= totalCountLimit ||
        accounting->serviceCount[service] >= limits.count ||
        accounting->bytes > totalByteLimit - packetSize ||
        accounting->serviceBytes[service] > limits.bytes - packetSize)
    {
      BlockSendLocked(connection, service, delivery, packetSize);
      return SendResult::BLOCKED;
    }

    ++accounting->count;
    ++accounting->serviceCount[service];
    accounting->bytes                 += packetSize;
    accounting->serviceBytes[service] += packetSize;
    accounting->writableServices      &= ~(UINT16_C(1) << service);
    return SendResult::SENT;
  }

  static bool SendCanResumeLocked(Connection& connection,
    LGNetService service, Delivery delivery)
  {
    const SendLimits limits = GetSendLimits(service, delivery);
    uint32_t         totalCountLimit;
    uint64_t         totalByteLimit;
    switch (delivery)
    {
      case Delivery::RELIABLE:
        totalCountLimit = NETWORK_SEND_RELIABLE_COUNT_MAX;
        totalByteLimit  = NETWORK_SEND_RELIABLE_BYTES_MAX;
        break;

      case Delivery::DATAGRAM:
        totalCountLimit = NETWORK_SEND_DATAGRAM_COUNT_MAX;
        totalByteLimit  = NETWORK_SEND_DATAGRAM_BYTES_MAX;
        break;

      case Delivery::BULK:
        totalCountLimit = NETWORK_SEND_BULK_COUNT_MAX;
        totalByteLimit  = NETWORK_SEND_BULK_BYTES_MAX;
        break;

      default:
        return false;
    }
    if (delivery != Delivery::BULK && !SendPriority(service))
    {
      totalCountLimit -= delivery == Delivery::DATAGRAM ?
        NETWORK_SEND_DATAGRAM_COUNT_RESERVED :
        NETWORK_SEND_RELIABLE_COUNT_RESERVED;
      totalByteLimit  -= delivery == Delivery::DATAGRAM ?
        NETWORK_SEND_DATAGRAM_BYTES_RESERVED :
        NETWORK_SEND_RELIABLE_BYTES_RESERVED;
    }
    const SendAccounting * accounting =
      GetSendAccountingLocked(connection, delivery);
    if (!accounting)
      return false;

    const uint32_t totalCount   = accounting->count;
    const uint64_t totalBytes   = accounting->bytes;
    const uint32_t serviceCount = accounting->serviceCount[service];
    const uint64_t serviceBytes = accounting->serviceBytes[service];
    const uint64_t blockedBytes =
      accounting->serviceBlockedBytes[service];

    if (connection.closing ||
        (delivery == Delivery::RELIABLE && !connection.controlStream) ||
        (delivery == Delivery::DATAGRAM &&
          (!connection.datagramSendEnabled ||
            blockedBytes > connection.maxDatagramSendLength)) ||
        (delivery == Delivery::BULK &&
          (!connection.outboundBulk[service] ||
            !connection.outboundBulk[service]->bound)))
      return false;

    const uint32_t totalResumeCount   =
      totalCountLimit * NETWORK_SEND_RESUME_NUMERATOR /
        NETWORK_SEND_RESUME_DENOMINATOR;
    const uint64_t totalResumeBytes   =
      totalByteLimit * NETWORK_SEND_RESUME_NUMERATOR /
        NETWORK_SEND_RESUME_DENOMINATOR;
    const uint32_t serviceResumeCount =
      limits.count * NETWORK_SEND_RESUME_NUMERATOR /
        NETWORK_SEND_RESUME_DENOMINATOR;
    const uint64_t serviceResumeBytes =
      limits.bytes * NETWORK_SEND_RESUME_NUMERATOR /
        NETWORK_SEND_RESUME_DENOMINATOR;

    return totalCount <= totalResumeCount &&
      totalBytes <= totalResumeBytes &&
      serviceCount <= serviceResumeCount &&
      serviceBytes <= serviceResumeBytes &&
      totalCount < totalCountLimit &&
      serviceCount < limits.count &&
      blockedBytes <= totalByteLimit - totalBytes &&
      blockedBytes <= limits.bytes - serviceBytes;
  }

  static uint16_t ReadyServicesLocked(Connection& connection,
    Delivery delivery)
  {
    uint16_t         ready      = 0;
    SendAccounting * accounting =
      GetSendAccountingLocked(connection, delivery);
    if (!accounting)
      return 0;

    for (LGNetService service = LG_NET_SERVICE_CORE;
         service < NETWORK_SERVICE_COUNT; ++service)
    {
      if (!accounting->serviceBlocked[service] || !SendCanResumeLocked(
            connection, service, delivery))
        continue;

      accounting->serviceBlocked[service]      = false;
      accounting->serviceBlockedBytes[service] = 0;
      ready                                   |= UINT16_C(1) << service;
    }
    return ready;
  }

  static void ReleaseSendLocked(Connection& connection, const Send& send)
  {
    if (!send.accounted)
      return;

    if (send.service >= NETWORK_SERVICE_COUNT)
    {
      DEBUG_ERROR("Network send accounting service out of range for "
        "client %u", connection.client);
      return;
    }

    SendAccounting * accounting =
      GetSendAccountingLocked(connection, send.delivery);
    if (!accounting)
    {
      DEBUG_ERROR("Network send accounting delivery out of range for "
        "client %u", connection.client);
      return;
    }
    if (!accounting->count || !accounting->serviceCount[send.service] ||
        accounting->bytes < send.length ||
        accounting->serviceBytes[send.service] < send.length)
    {
      DEBUG_ERROR("Network send accounting underflow for client %u",
        connection.client);
      return;
    }

    --accounting->count;
    --accounting->serviceCount[send.service];
    accounting->bytes                      -= send.length;
    accounting->serviceBytes[send.service] -= send.length;
    accounting->writableServices           |=
      ReadyServicesLocked(connection, send.delivery);
  }

  static bool ConsumeSendReady(Connection& connection,
    LGNetService service, Delivery delivery)
  {
    if (service >= NETWORK_SERVICE_COUNT)
      return false;

    CSRWExclusiveLock guard(connection.lock);
    SendAccounting * accounting =
      GetSendAccountingLocked(connection, delivery);
    if (!accounting)
      return false;
    const uint16_t serviceMask = UINT16_C(1) << service;
    const bool     ready       =
      (accounting->writableServices & serviceMask) != 0;
    accounting->writableServices &= ~serviceMask;
    return ready;
  }

  void NotifyAudioSendReady(Connection& connection, bool datagram)
  {
    const Delivery delivery = datagram ?
      Delivery::DATAGRAM : Delivery::RELIABLE;
    if (ConsumeSendReady(connection, LG_NET_SERVICE_AUDIO, delivery))
      audio.SendReady(
        AudioPeer(connection), LG_NET_SERVICE_AUDIO, datagram);
  }

  void NotifyClipboardSendReady(Connection& connection, Delivery delivery)
  {
    const bool bulk = delivery == Delivery::BULK;
    if (ConsumeSendReady(
          connection, LG_NET_SERVICE_CLIPBOARD, delivery))
      clipboard.SendReady(
        ClipboardPeer(connection), LG_NET_SERVICE_CLIPBOARD, bulk);
    if (ConsumeSendReady(connection, LG_NET_SERVICE_FILE, delivery))
      clipboard.SendReady(
        ClipboardPeer(connection), LG_NET_SERVICE_FILE, bulk);
  }

  void NotifyVideoSendReady(Connection& connection, Delivery delivery)
  {
    NetworkVideoDelivery videoDelivery = NetworkVideoDelivery::RELIABLE;
    switch (delivery)
    {
      case Delivery::RELIABLE:
        videoDelivery = NetworkVideoDelivery::RELIABLE;
        break;

      case Delivery::DATAGRAM:
        videoDelivery = NetworkVideoDelivery::DATAGRAM;
        break;

      case Delivery::BULK:
        videoDelivery = NetworkVideoDelivery::BULK;
        break;

      default:
        return;
    }
    if (!ConsumeSendReady(connection, LG_NET_SERVICE_VIDEO, delivery))
      return;
    video.SendReady(VideoPeer(connection), videoDelivery);
  }

  SendResult TrySendPacket(Connection& connection, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID, const void * payload,
    uint32_t payloadLength, bool activateSession = false)
  {
    const LGNetServiceVersionInfo * version =
      lgNetServiceVersionInfo(service);
    const LGNetMessageRoute       * route   =
      lgNetMessageRoute(service, messageType);
    if (!version ||
        !lgNetMessageRouteAllows(route, LG_NET_ROLE_SERVER,
          LG_NET_ROUTE_DELIVERY_RELIABLE) ||
        (activateSession &&
          (service != LG_NET_SERVICE_CORE ||
            messageType != LG_NET_CORE_MESSAGE_CAPABILITIES_DONE ||
            flags != (LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL) ||
            componentEpoch || !requestID || payloadLength)) ||
        (payloadLength && !payload) ||
        payloadLength > NETWORK_MAX_CONTROL_PAYLOAD ||
        flags & (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM |
          LG_NET_ENVELOPE_EARLY_DATA))
      return SendResult::FAILED;

    const size_t          packetSize =
      LG_NET_ENVELOPE_WIRE_SIZE + payloadLength;
    std::unique_ptr<Send> send;
    QUIC_STATUS           status;
    {
      CSRWExclusiveLock guard(connection.lock);
      const uint16_t serviceMask = UINT16_C(1) << service;
      if (connection.closing || !connection.controlStream ||
          (activateSession &&
            (connection.handshake !=
              HandshakeState::PUBLISHING_CAPABILITIES ||
              !connection.session ||
              requestID != connection.handshakeRequestID)) ||
          (service != LG_NET_SERVICE_CORE &&
            (connection.handshake != HandshakeState::READY ||
             !(connection.selectedServices & serviceMask) ||
             !(connection.reliableServices & serviceMask))))
        return SendResult::FAILED;
      const uint32_t maxPayload     = service == LG_NET_SERVICE_CORE ?
        connection.maxPayload :
        connection.selectedCapabilities[service].maxPayloadLength;
      const uint16_t serviceVersion = service == LG_NET_SERVICE_CORE ?
        version->current :
        connection.selectedCapabilities[service].versionMin;
      if (payloadLength > maxPayload ||
          !lgNetServiceVersionSupported(service, serviceVersion) ||
          serviceVersion < route->serviceVersionMin ||
          connection.sendSequence == UINT64_MAX)
        return SendResult::FAILED;

      const SendResult reserve =
        ReserveSendLocked(
          connection, service, Delivery::RELIABLE, packetSize);
      if (reserve != SendResult::SENT)
        return reserve;

      send.reset(new (std::nothrow) Send());
      if (!send)
      {
        Send rollback;
        rollback.service  = service;
        rollback.length   = static_cast<uint32_t>(packetSize);
        rollback.delivery = Delivery::RELIABLE;
        ReleaseSendLocked(connection, rollback);
        return SendResult::FAILED;
      }
      try
      {
        send->data.resize(packetSize);
      }
      catch (...)
      {
        send->service  = service;
        send->length   = static_cast<uint32_t>(packetSize);
        send->delivery = Delivery::RELIABLE;
        ReleaseSendLocked(connection, *send);
        return SendResult::FAILED;
      }
      send->service  = service;
      send->length   = static_cast<uint32_t>(packetSize);
      send->delivery = Delivery::RELIABLE;

      LGNetPacketParameters parameters;
      lgNetPacketParametersInit(&parameters, service, messageType,
        serviceVersion, route->messageVersion);
      parameters.flags          = flags | LG_NET_ENVELOPE_RELIABLE;
      parameters.sessionEpoch   = connection.sessionEpoch;
      parameters.componentEpoch = componentEpoch;
      parameters.sequence       = ++connection.sendSequence;
      parameters.requestID      = requestID;

      size_t wireSize;
      if (!lgNetPacketConstruct(send->data.data(), send->data.size(),
            &wireSize, &parameters, payload, payloadLength) ||
          wireSize > UINT32_MAX)
      {
        --connection.sendSequence;
        ReleaseSendLocked(connection, *send);
        return SendResult::FAILED;
      }

      send->buffer.Buffer = send->data.data();
      send->buffer.Length = static_cast<uint32_t>(wireSize);
      status = api->StreamSend(connection.controlStream, &send->buffer, 1,
        QUIC_SEND_FLAG_NONE, send.get());
      if (QUIC_FAILED(status))
      {
        --connection.sendSequence;
        ReleaseSendLocked(connection, *send);
      }
      else if (activateSession)
      {
        connection.handshake         = HandshakeState::READY;
        connection.handshakeDeadline = 0;
      }
    }
    if (QUIC_FAILED(status))
      return SendResult::FAILED;
    send.release();
    return SendResult::SENT;
  }

  bool SendPacket(Connection& connection, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID, const void * payload,
    uint32_t payloadLength, bool activateSession = false)
  {
    return TrySendPacket(connection, service, messageType, flags,
      componentEpoch, requestID, payload, payloadLength, activateSession) ==
        SendResult::SENT;
  }

  static void DetachBulkStreamLocked(BulkStream& stream)
  {
    Connection& connection = stream.connection;
    if (stream.outbound)
    {
      if (stream.service < NETWORK_SERVICE_COUNT &&
          connection.outboundBulk[stream.service] == &stream)
        connection.outboundBulk[stream.service] = nullptr;
      return;
    }

    BulkStream ** link = &connection.inboundBulk;
    while (*link && *link != &stream)
      link = &(*link)->next;
    if (*link != &stream)
      return;

    *link = stream.next;
    if (connection.peerBulkStreams)
      --connection.peerBulkStreams;
    if (stream.bound && stream.service < NETWORK_SERVICE_COUNT &&
        connection.servicePeerBulkStreams[stream.service])
      --connection.servicePeerBulkStreams[stream.service];
  }

  SendResult OpenBulkStream(Connection& connection, LGNetService service,
    uint64_t packetSize)
  {
    std::unique_ptr<BulkStream> stream(
      new (std::nothrow) BulkStream(connection));
    if (!stream)
      return SendResult::FAILED;
    stream->service  = service;
    stream->outbound = true;

    HQUIC connectionHandle;
    {
      CSRWExclusiveLock guard(connection.lock);
      const uint16_t serviceMask = UINT16_C(1) << service;
      if (connection.closing || !connection.handle ||
          !BulkOriginAllowed(service, LG_NET_ROLE_SERVER) ||
          connection.handshake != HandshakeState::READY ||
          !(connection.selectedServices & serviceMask) ||
          !(connection.bulkServices & serviceMask) ||
          !connection.selectedCapabilities
            [service].maxUnidirectionalStreams)
        return SendResult::FAILED;
      if (connection.outboundBulk[service])
      {
        BlockSendLocked(
          connection, service, Delivery::BULK, packetSize);
        return SendResult::BLOCKED;
      }

      connection.outboundBulk[service] = stream.get();
      connectionHandle                 = connection.handle;
      BlockSendLocked(connection, service, Delivery::BULK, packetSize);
    }

    HQUIC       handle = nullptr;
    QUIC_STATUS status = api->StreamOpen(connectionHandle,
      QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, BulkStreamCallback,
      stream.get(), &handle);
    if (QUIC_FAILED(status))
    {
      CSRWExclusiveLock guard(connection.lock);
      DetachBulkStreamLocked(*stream);
      connection.bulkSend.serviceBlocked[service]      = false;
      connection.bulkSend.serviceBlockedBytes[service] = 0;
      return SendResult::FAILED;
    }
    stream->handle = handle;

    status = api->StreamStart(handle,
      QUIC_STREAM_START_FLAG_IMMEDIATE |
        QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL);
    if (QUIC_FAILED(status))
    {
      {
        CSRWExclusiveLock guard(connection.lock);
        DetachBulkStreamLocked(*stream);
        connection.bulkSend.serviceBlocked[service]      = false;
        connection.bulkSend.serviceBlockedBytes[service] = 0;
      }
      api->StreamClose(handle);
      return SendResult::FAILED;
    }

    bool bound;
    bool shutdownComplete;
    {
      CSRWExclusiveLock guard(connection.lock);
      stream->openingReturned  = true;
      bound                    = stream->bound &&
        !stream->shutdownComplete;
      shutdownComplete         = stream->shutdownComplete;
      if (bound)
        ReadyServicesLocked(connection, Delivery::BULK);
    }
    if (shutdownComplete)
      return SendResult::FAILED;

    stream.release();
    return bound ? SendResult::SENT : SendResult::BLOCKED;
  }

  SendResult TrySendBulk(Connection& connection, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID, const void * payload,
    uint32_t payloadLength)
  {
    const LGNetServiceVersionInfo * version =
      lgNetServiceVersionInfo(service);
    const LGNetMessageRoute       * route   =
      lgNetMessageRoute(service, messageType);
    if (!version ||
        !lgNetMessageRouteAllows(route, LG_NET_ROLE_SERVER,
          LG_NET_ROUTE_DELIVERY_BULK) ||
        (payloadLength && !payload) ||
        payloadLength > LG_NET_MAX_PAYLOAD_LENGTH ||
        flags & (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM |
          LG_NET_ENVELOPE_EARLY_DATA))
      return SendResult::FAILED;

    const size_t     packetSize =
      LG_NET_ENVELOPE_WIRE_SIZE + payloadLength;
    const SendLimits limits     =
      GetSendLimits(service, Delivery::BULK);
    if (packetSize > limits.bytes ||
        packetSize > NETWORK_SEND_BULK_BYTES_MAX)
      return SendResult::FAILED;

retry:
    std::unique_ptr<Send> send;
    QUIC_STATUS           status;
    {
      CSRWExclusiveLock guard(connection.lock);
      const uint16_t serviceMask = UINT16_C(1) << service;
      if (connection.closing || !connection.handle ||
          connection.handshake != HandshakeState::READY ||
          !(connection.selectedServices & serviceMask) ||
          !(connection.bulkServices & serviceMask) ||
          !connection.selectedCapabilities
            [service].maxUnidirectionalStreams)
        return SendResult::FAILED;

      const uint16_t serviceVersion =
        connection.selectedCapabilities[service].versionMin;
      if (payloadLength > connection.selectedCapabilities
            [service].maxPayloadLength ||
          !lgNetServiceVersionSupported(service, serviceVersion) ||
          serviceVersion < route->serviceVersionMin)
        return SendResult::FAILED;

      BulkStream * stream = connection.outboundBulk[service];
      if (!stream)
      {
        guard.Unlock();
        const SendResult open =
          OpenBulkStream(connection, service, packetSize);
        if (open == SendResult::SENT)
          goto retry;
        return open;
      }
      if (!stream->bound)
      {
        stream->openingReturned = true;
        BlockSendLocked(connection, service, Delivery::BULK, packetSize);
        return SendResult::BLOCKED;
      }
      if (stream->sendSequence == UINT64_MAX)
        return SendResult::FAILED;

      const SendResult reserve = ReserveSendLocked(
        connection, service, Delivery::BULK, packetSize);
      if (reserve != SendResult::SENT)
        return reserve;

      send.reset(new (std::nothrow) Send());
      if (!send)
      {
        Send rollback;
        rollback.service  = service;
        rollback.length   = static_cast<uint32_t>(packetSize);
        rollback.delivery = Delivery::BULK;
        ReleaseSendLocked(connection, rollback);
        return SendResult::FAILED;
      }
      try
      {
        send->data.resize(packetSize);
      }
      catch (...)
      {
        send->service  = service;
        send->length   = static_cast<uint32_t>(packetSize);
        send->delivery = Delivery::BULK;
        ReleaseSendLocked(connection, *send);
        return SendResult::FAILED;
      }
      send->service  = service;
      send->length   = static_cast<uint32_t>(packetSize);
      send->delivery = Delivery::BULK;

      LGNetPacketParameters parameters;
      lgNetPacketParametersInit(&parameters, service, messageType,
        serviceVersion, route->messageVersion);
      parameters.flags          = flags | LG_NET_ENVELOPE_FINAL;
      parameters.sessionEpoch   = connection.sessionEpoch;
      parameters.componentEpoch = componentEpoch;
      parameters.sequence       = ++stream->sendSequence;
      parameters.requestID      = requestID;

      size_t wireSize;
      if (!lgNetPacketConstruct(send->data.data(), send->data.size(),
            &wireSize, &parameters, payload, payloadLength) ||
          wireSize != packetSize)
      {
        --stream->sendSequence;
        ReleaseSendLocked(connection, *send);
        return SendResult::FAILED;
      }

      send->buffer.Buffer = send->data.data();
      send->buffer.Length = static_cast<uint32_t>(wireSize);
      status = api->StreamSend(stream->handle, &send->buffer, 1,
        QUIC_SEND_FLAG_NONE, send.get());
      if (QUIC_FAILED(status))
      {
        --stream->sendSequence;
        ReleaseSendLocked(connection, *send);
      }
    }
    if (QUIC_FAILED(status))
      return SendResult::FAILED;
    send.release();
    return SendResult::SENT;
  }

  SendResult TrySendDatagram(Connection& connection, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID, const void * payload,
    uint32_t payloadLength, bool discardIfBlocked)
  {
    const LGNetServiceVersionInfo * version =
      lgNetServiceVersionInfo(service);
    const LGNetMessageRoute       * route   =
      lgNetMessageRoute(service, messageType);
    if (!version ||
        !lgNetMessageRouteAllows(route, LG_NET_ROLE_SERVER,
          LG_NET_ROUTE_DELIVERY_DATAGRAM) ||
        (payloadLength && !payload) ||
        payloadLength > NETWORK_MAX_DATAGRAM_PAYLOAD ||
        flags & (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM |
          LG_NET_ENVELOPE_EARLY_DATA))
      return SendResult::FAILED;

    const size_t          packetSize =
      LG_NET_ENVELOPE_WIRE_SIZE + payloadLength;
    std::unique_ptr<Send> send;
    QUIC_STATUS           status;
    {
      CSRWExclusiveLock guard(connection.lock);
      const uint16_t serviceMask    = UINT16_C(1) << service;
      const bool     serviceReady   = service == LG_NET_SERVICE_CORE ||
        ((connection.selectedServices & serviceMask) &&
          (connection.datagramServices & serviceMask));
      const uint32_t maxPayload     = service == LG_NET_SERVICE_CORE ?
        connection.maxDatagramPayload :
        connection.selectedCapabilities[service].maxDatagramPayload;
      const uint16_t serviceVersion = service == LG_NET_SERVICE_CORE ?
        version->current :
        connection.selectedCapabilities[service].versionMin;
      if (connection.closing || !connection.handle ||
          connection.handshake != HandshakeState::READY ||
          !serviceReady ||
          !maxPayload || payloadLength > maxPayload ||
          !lgNetServiceVersionSupported(service, serviceVersion) ||
          serviceVersion < route->serviceVersionMin)
        return SendResult::FAILED;
      if (connection.datagramSendSequence == UINT64_MAX)
        return SendResult::FAILED;
      if (!connection.datagramSendEnabled ||
          packetSize > connection.maxDatagramSendLength)
      {
        const SendLimits limits = GetSendLimits(service, Delivery::DATAGRAM);
        if (packetSize <= limits.bytes &&
            packetSize <= NETWORK_SEND_DATAGRAM_BYTES_MAX)
          BlockSendLocked(
            connection, service, Delivery::DATAGRAM, packetSize);
        return SendResult::BLOCKED;
      }

      const SendResult reserve =
        ReserveSendLocked(
          connection, service, Delivery::DATAGRAM, packetSize);
      if (reserve != SendResult::SENT)
        return reserve;

      send.reset(new (std::nothrow) Send());
      if (!send)
      {
        Send rollback;
        rollback.service  = service;
        rollback.length   = static_cast<uint32_t>(packetSize);
        rollback.delivery = Delivery::DATAGRAM;
        ReleaseSendLocked(connection, rollback);
        return SendResult::FAILED;
      }
      try
      {
        send->data.resize(packetSize);
      }
      catch (...)
      {
        send->service  = service;
        send->length   = static_cast<uint32_t>(packetSize);
        send->delivery = Delivery::DATAGRAM;
        ReleaseSendLocked(connection, *send);
        return SendResult::FAILED;
      }
      send->service  = service;
      send->length   = static_cast<uint32_t>(packetSize);
      send->delivery = Delivery::DATAGRAM;

      LGNetPacketParameters parameters;
      lgNetPacketParametersInit(&parameters, service, messageType,
        serviceVersion, route->messageVersion);
      parameters.flags          = flags | LG_NET_ENVELOPE_DATAGRAM |
        LG_NET_ENVELOPE_FINAL;
      parameters.sessionEpoch   = connection.sessionEpoch;
      parameters.componentEpoch = componentEpoch;
      parameters.sequence       = ++connection.datagramSendSequence;
      parameters.requestID      = requestID;

      size_t wireSize;
      if (!lgNetPacketConstruct(send->data.data(), send->data.size(),
            &wireSize, &parameters, payload, payloadLength) ||
          wireSize > UINT32_MAX)
      {
        --connection.datagramSendSequence;
        ReleaseSendLocked(connection, *send);
        return SendResult::FAILED;
      }

      send->buffer.Buffer = send->data.data();
      send->buffer.Length = static_cast<uint32_t>(wireSize);
      QUIC_SEND_FLAGS sendFlags = QUIC_SEND_FLAG_PRIORITY_WORK;
      if (discardIfBlocked)
        sendFlags |= QUIC_SEND_FLAG_CANCEL_ON_BLOCKED;
      status = api->DatagramSend(connection.handle, &send->buffer, 1,
        sendFlags, send.get());
      if (QUIC_FAILED(status))
      {
        --connection.datagramSendSequence;
        ReleaseSendLocked(connection, *send);
      }
    }
    if (QUIC_FAILED(status))
      return SendResult::FAILED;
    send.release();
    return SendResult::SENT;
  }

  bool SendDatagram(Connection& connection, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID, const void * payload,
    uint32_t payloadLength, bool discardIfBlocked)
  {
    return TrySendDatagram(connection, service, messageType, flags,
      componentEpoch, requestID, payload, payloadLength,
      discardIfBlocked) == SendResult::SENT;
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
        hello.role != LG_NET_ROLE_CLIENT ||
        hello.protocolMin > LG_NET_PROTOCOL_VERSION_CURRENT ||
        hello.protocolMax < LG_NET_PROTOCOL_VERSION_CURRENT ||
        (hello.authModes & ~LG_NET_AUTH_MODES_KNOWN) != 0 ||
        !(hello.authModes & LG_NET_AUTH_MODE_FLAG(settings.auth)))
      return false;

    uint64_t sessionEpoch = 0;
    uint8_t  serverNonce[LG_NET_HELLO_NONCE_LENGTH];
    if (!NetworkAuthCrypto::RandomBytes(
          &sessionEpoch, sizeof(sessionEpoch)) ||
        !RandomNonzero(serverNonce, sizeof(serverNonce)))
      return false;
    if (!sessionEpoch)
      sessionEpoch = GetTickCount64() | 1;

    LGNetSequenceTracker datagramReceive;
    if (!lgNetSequenceTrackerInit(&datagramReceive, sessionEpoch,
          NETWORK_SEQUENCE_COMPONENT, NETWORK_DATAGRAM_WINDOW, false))
      return false;

    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.closing ||
          connection.handshake != HandshakeState::WAITING_HELLO)
        return false;
      connection.sessionEpoch             = sessionEpoch;
      connection.datagramReceive          = datagramReceive;
      connection.handshakeRequestID       = packet.envelope.requestID;
      connection.expectedCapabilities     = hello.capabilityCount;
      connection.receivedCapabilities     = 0;
      connection.selectedCapabilityCount  = 0;
      connection.maxPayload               = hello.maxPayloadLength <
        NETWORK_MAX_CONTROL_PAYLOAD ? hello.maxPayloadLength :
          static_cast<uint32_t>(NETWORK_MAX_CONTROL_PAYLOAD);
      connection.maxDatagramPayload       = hello.maxDatagramPayload <
        NETWORK_MAX_DATAGRAM_PAYLOAD ? hello.maxDatagramPayload :
          NETWORK_MAX_DATAGRAM_PAYLOAD;
      connection.offeredServices          = 0;
      connection.selectedServices         = 0;
      connection.reliableServices         = 0;
      connection.datagramServices         = 0;
      connection.bulkServices             = 0;
      connection.maxPeerBulkStreams       = 0;
      connection.peerBulkStreams          = 0;
      memset(connection.servicePeerBulkStreams, 0,
        sizeof(connection.servicePeerBulkStreams));
      memcpy(connection.clientNonce, hello.nonce,
        sizeof(connection.clientNonce));
      memcpy(connection.serverNonce, serverNonce,
        sizeof(connection.serverNonce));
      connection.handshake                =
        HandshakeState::WAITING_CAPABILITIES;
    }

    return true;
  }

  bool HandleCapability(Connection& connection,
    const LGNetPacketView& packet)
  {
    LGNetCapability capability;
    if (lgNetCapabilityDecode(&capability, packet.payload,
          packet.payloadLength) != LG_NET_PARSE_OK ||
        capability.service <= LG_NET_SERVICE_CORE ||
        capability.service > LG_NET_SERVICE_CONTROL ||
        capability.service == LG_NET_SERVICE_USB ||
        capability.flags & LG_NET_CAPABILITY_SELECTED ||
        capability.maxBidirectionalStreams ||
        capability.maxUnidirectionalStreams > NETWORK_PEER_UNIDI_STREAMS ||
        (!!(capability.flags & LG_NET_CAPABILITY_BULK) !=
          !!capability.maxUnidirectionalStreams) ||
        !(capability.flags & (LG_NET_CAPABILITY_RELIABLE |
          LG_NET_CAPABILITY_DATAGRAM | LG_NET_CAPABILITY_BULK)))
      return false;

    const LGNetServiceVersionInfo * version =
      lgNetServiceVersionInfo(capability.service);
    if (!version || capability.versionMax < version->minimum ||
        capability.versionMin > version->maximum)
      return false;

    const uint16_t serviceMask = UINT16_C(1) << capability.service;
    CSRWExclusiveLock guard(connection.lock);
    if (connection.closing ||
        connection.handshake != HandshakeState::WAITING_CAPABILITIES ||
        packet.envelope.requestID != connection.handshakeRequestID ||
        packet.envelope.sessionEpoch != 0 ||
        packet.envelope.componentEpoch != 0 ||
        connection.receivedCapabilities >=
          connection.expectedCapabilities ||
        connection.offeredServices & serviceMask)
      return false;

    connection.offeredCapabilities[capability.service]  = capability;
    connection.offeredServices                          |= serviceMask;
    ++connection.receivedCapabilities;
    return true;
  }

  bool SelectCapability(const Connection& connection,
    const LGNetCapability& offered, LGNetCapability& selected) const
  {
    const LGNetServiceVersionInfo * version =
      lgNetServiceVersionInfo(offered.service);
    uint32_t minimumPayload;
    switch (offered.service)
    {
      case LG_NET_SERVICE_VIDEO:
        minimumPayload = LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE;
        break;

      case LG_NET_SERVICE_RECOVERY:
        minimumPayload = LG_NET_RECOVERY_REQUEST_WIRE_SIZE;
        break;

      case LG_NET_SERVICE_CURSOR:
        minimumPayload = LG_NET_CURSOR_POSITION_WIRE_SIZE;
        break;

      case LG_NET_SERVICE_INPUT:
        minimumPayload = LG_NET_INPUT_ABSOLUTE_WIRE_SIZE;
        break;

      case LG_NET_SERVICE_AUDIO:
        minimumPayload = LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE +
          AUDIO_MAX_PCM_BYTES;
        break;

      case LG_NET_SERVICE_CLIPBOARD:
        minimumPayload = LG_NET_CLIPBOARD_OFFER_HEADER_WIRE_SIZE;
        break;

      case LG_NET_SERVICE_FILE:
        minimumPayload = LG_NET_FILE_ENTRY_HEADER_WIRE_SIZE;
        break;

      case LG_NET_SERVICE_CONTROL:
        minimumPayload = LG_NET_CONTROL_FRAME_SCHEDULE_WIRE_SIZE;
        break;

      default:
        return false;
    }

    if (!version ||
        !transport.ServiceEnabled(offered.service) ||
        !(offered.flags & LG_NET_CAPABILITY_RELIABLE) ||
        offered.versionMin > version->current ||
        offered.versionMax < version->minimum ||
        (offered.service == LG_NET_SERVICE_AUDIO &&
          offered.versionMax <
            LG_NET_AUDIO_OWNERSHIP_INTRODUCED_SERVICE_VERSION) ||
        offered.maxPayloadLength < minimumPayload ||
        connection.maxPayload < minimumPayload)
      return false;

    const uint16_t selectedVersion = offered.versionMax < version->current ?
      offered.versionMax : version->current;
    if (selectedVersion < offered.versionMin)
      return false;

    selected                          = {};
    selected.service                  = offered.service;
    selected.versionMin               = selectedVersion;
    selected.versionMax               = selectedVersion;
    selected.flags                    = LG_NET_CAPABILITY_SELECTED |
      LG_NET_CAPABILITY_RELIABLE;
    if (offered.service == LG_NET_SERVICE_AUDIO &&
        (offered.flags & LG_NET_CAPABILITY_MULTI_CLIENT))
      selected.flags |= LG_NET_CAPABILITY_MULTI_CLIENT;
    selected.features                 = 0;
    selected.maxPayloadLength         = offered.maxPayloadLength <
      connection.maxPayload ? offered.maxPayloadLength :
        connection.maxPayload;
    selected.maxDatagramPayload       = 0;
    selected.maxBidirectionalStreams  = 0;
    selected.maxUnidirectionalStreams = 0;
    const uint32_t minimumDatagram = offered.service ==
        LG_NET_SERVICE_VIDEO ?
          LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE + 1U :
      offered.service == LG_NET_SERVICE_INPUT ?
        LG_NET_INPUT_ABSOLUTE_WIRE_SIZE :
      offered.service == LG_NET_SERVICE_CURSOR ?
        LG_NET_CURSOR_POSITION_WIRE_SIZE :
      offered.service == LG_NET_SERVICE_AUDIO ?
        LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE + 2U : 0;
    if (minimumDatagram &&
        (offered.flags & LG_NET_CAPABILITY_DATAGRAM) &&
        offered.maxDatagramPayload >= minimumDatagram &&
        connection.maxDatagramPayload >= minimumDatagram)
    {
      selected.flags               |= LG_NET_CAPABILITY_DATAGRAM;
      selected.maxDatagramPayload   = offered.maxDatagramPayload <
        connection.maxDatagramPayload ? offered.maxDatagramPayload :
          connection.maxDatagramPayload;
    }
    if (BulkServiceSupported(offered.service) &&
        (offered.flags & LG_NET_CAPABILITY_BULK) &&
        offered.maxUnidirectionalStreams)
    {
      selected.flags                    |= LG_NET_CAPABILITY_BULK;
      selected.maxUnidirectionalStreams  =
        offered.maxUnidirectionalStreams <
            NETWORK_BULK_STREAMS_PER_SERVICE ?
          offered.maxUnidirectionalStreams :
          NETWORK_BULK_STREAMS_PER_SERVICE;
    }
    return lgNetCapabilityValid(&selected);
  }

  bool SendHelloAcknowledgement(Connection& connection)
  {
    uint64_t        sessionEpoch;
    uint32_t        maxPayload;
    uint32_t        maxDatagramPayload;
    uint32_t        capabilityCount;
    uint64_t        requestID;
    uint8_t         serverNonce[LG_NET_HELLO_NONCE_LENGTH];
    {
      CSRWSharedLock guard(connection.lock);
      if (connection.closing)
        return false;
      sessionEpoch       = connection.sessionEpoch;
      maxPayload         = connection.maxPayload;
      maxDatagramPayload = connection.maxDatagramPayload;
      capabilityCount    = connection.selectedCapabilityCount;
      requestID          = connection.handshakeRequestID;
      memcpy(serverNonce, connection.serverNonce, sizeof(serverNonce));
    }

    LGNetHelloAck acknowledgement;
    lgNetHelloAckInit(&acknowledgement, sessionEpoch);
    acknowledgement.authMode           = settings.auth;
    acknowledgement.maxPayloadLength   = maxPayload;
    acknowledgement.maxDatagramPayload = maxDatagramPayload;
    acknowledgement.capabilityCount    = capabilityCount;
    memcpy(acknowledgement.nonce, serverNonce, sizeof(serverNonce));

    uint8_t payload[LG_NET_HELLO_ACK_WIRE_SIZE];

    const bool sent = lgNetHelloAckEncode(
        payload, sizeof(payload), &acknowledgement) &&
      SendPacket(connection, LG_NET_SERVICE_CORE,
        LG_NET_CORE_MESSAGE_HELLO_ACK,
        LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
        0, requestID, payload, sizeof(payload));
    NetworkAuthCrypto::SecureZero(&acknowledgement,
      sizeof(acknowledgement));
    NetworkAuthCrypto::SecureZero(payload, sizeof(payload));
    NetworkAuthCrypto::SecureZero(serverNonce, sizeof(serverNonce));
    return sent;
  }

  bool SendPasswordChallenge(Connection& connection)
  {
    LGNetPasswordChallenge challenge;
    uint64_t               requestID;
    {
      CSRWSharedLock guard(connection.lock);
      if (connection.closing ||
          connection.handshake != HandshakeState::WAITING_AUTH_RESPONSE)
        return false;
      challenge = connection.passwordChallenge;
      requestID = connection.handshakeRequestID;
    }

    const size_t payloadSize = lgNetPasswordChallengeSize(&challenge);
    uint8_t payload[LG_NET_PASSWORD_CHALLENGE_HEADER_SIZE +
      LG_NET_PASSWORD_SALT_MAX_LENGTH +
      LG_NET_PASSWORD_CHALLENGE_MAX_LENGTH];

    const bool sent = payloadSize &&
      lgNetPasswordChallengeEncode(payload, payloadSize, &challenge) &&
      SendPacket(connection, LG_NET_SERVICE_CORE,
        LG_NET_CORE_MESSAGE_AUTH_CHALLENGE,
        LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
        0, requestID, payload,
        static_cast<uint32_t>(payloadSize));
    NetworkAuthCrypto::SecureZero(payload, sizeof(payload));
    NetworkAuthCrypto::SecureZero(&challenge, sizeof(challenge));
    return sent;
  }

  bool PublishCapabilities(Connection& connection)
  {
    LGNetCapability selected[NETWORK_SERVICE_COUNT] = {};
    uint64_t        requestID;
    {
      CSRWSharedLock guard(connection.lock);
      if (connection.closing ||
          connection.handshake !=
            HandshakeState::PUBLISHING_CAPABILITIES ||
          connection.session)
        return false;
      memcpy(selected, connection.selectedCapabilities, sizeof(selected));
      requestID = connection.handshakeRequestID;
    }

    for (LGNetService service = LG_NET_SERVICE_RECOVERY;
         service <= LG_NET_SERVICE_CONTROL; ++service)
    {
      if (service == LG_NET_SERVICE_USB)
        continue;
      if (!selected[service].service)
        continue;

      uint8_t capabilityPayload[LG_NET_CAPABILITY_WIRE_SIZE];
      if (!lgNetCapabilityEncode(capabilityPayload,
            sizeof(capabilityPayload), &selected[service]) ||
          !SendPacket(connection, LG_NET_SERVICE_CORE,
            LG_NET_CORE_MESSAGE_CAPABILITY,
            LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
            0, requestID, capabilityPayload,
            sizeof(capabilityPayload)))
        return false;
    }

    std::shared_ptr<CNetworkSession> session;
    try
    {
      session = transport.CreateSession(
        connection.client, connection.generation);
    }
    catch (...)
    {
      return false;
    }
    if (!session)
      return false;

    bool prepared          = false;
    bool audioSelected     = false;
    bool clipboardSelected = false;
    bool videoSelected     = false;
    {
      CSRWExclusiveLock guard(connection.lock);
      prepared = !connection.closing &&
        connection.handshake == HandshakeState::PUBLISHING_CAPABILITIES &&
        !connection.session;
      if (prepared)
      {
        connection.session = std::move(session);
        NetworkAuthCrypto::SecureZero(&connection.passwordChallenge,
          sizeof(connection.passwordChallenge));
        NetworkAuthCrypto::SecureZero(
          connection.clientNonce, sizeof(connection.clientNonce));
        NetworkAuthCrypto::SecureZero(
          connection.serverNonce, sizeof(connection.serverNonce));
        audioSelected                = (connection.selectedServices &
          (UINT16_C(1) << LG_NET_SERVICE_AUDIO)) != 0;
        const uint16_t clipboardServices =
          (UINT16_C(1) << LG_NET_SERVICE_CLIPBOARD) |
          (UINT16_C(1) << LG_NET_SERVICE_FILE);
        clipboardSelected            =
          (connection.selectedServices & clipboardServices) ==
            clipboardServices;
        videoSelected                = (connection.selectedServices &
          (UINT16_C(1) << LG_NET_SERVICE_VIDEO)) != 0;
      }
    }
    if (!prepared)
    {
      transport.RemoveSession(connection.client, connection.generation);
      return false;
    }
    if (audioSelected && !audio.PeerConnected(AudioPeer(connection)))
      return false;
    if (clipboardSelected &&
        !clipboard.PeerConnected(ClipboardPeer(connection)))
      return false;
    if (videoSelected && !video.PeerConnected(VideoPeer(connection)))
      return false;
    if (!SendPacket(connection, LG_NET_SERVICE_CORE,
          LG_NET_CORE_MESSAGE_CAPABILITIES_DONE,
          LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
          0, requestID, nullptr, 0, true))
      return false;
    DEBUG_INFO("Network client %u completed the control handshake",
      connection.client);
    return true;
  }

  bool FinishCapabilities(Connection& connection,
    const LGNetPacketView& packet)
  {
    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.closing ||
          connection.handshake != HandshakeState::WAITING_CAPABILITIES ||
          packet.envelope.requestID != connection.handshakeRequestID ||
          packet.envelope.sessionEpoch != 0 ||
          packet.envelope.componentEpoch != 0 || packet.payloadLength ||
          connection.receivedCapabilities !=
            connection.expectedCapabilities)
        return false;

      for (LGNetService service = LG_NET_SERVICE_RECOVERY;
           service <= LG_NET_SERVICE_CONTROL; ++service)
      {
        if (service == LG_NET_SERVICE_USB)
          continue;
        const uint16_t serviceMask = UINT16_C(1) << service;
        if (!(connection.offeredServices & serviceMask))
          continue;

        LGNetCapability capability;
        if (!SelectCapability(connection,
              connection.offeredCapabilities[service], capability))
        {
          if (connection.offeredCapabilities[service].flags &
              LG_NET_CAPABILITY_REQUIRED)
            return false;
          continue;
        }

        connection.selectedCapabilities[service] = capability;
        connection.selectedServices              |= serviceMask;
        if (capability.flags & LG_NET_CAPABILITY_RELIABLE)
          connection.reliableServices |= serviceMask;
        if (capability.flags & LG_NET_CAPABILITY_DATAGRAM)
          connection.datagramServices |= serviceMask;
        if (capability.flags & LG_NET_CAPABILITY_BULK)
        {
          if (capability.maxUnidirectionalStreams >
              NETWORK_PEER_UNIDI_STREAMS -
                connection.maxPeerBulkStreams)
            return false;
          connection.bulkServices       |= serviceMask;
          connection.maxPeerBulkStreams +=
            capability.maxUnidirectionalStreams;
        }
        ++connection.selectedCapabilityCount;
      }
    }

    LGNetPasswordChallenge challenge = {};
    if (settings.auth == LG_NET_AUTH_PASSWORD)
    {
      challenge.algorithm       = LG_NET_PASSWORD_PBKDF2_HMAC_SHA256;
      challenge.rounds          = NETWORK_PASSWORD_ROUNDS;
      challenge.saltLength      = NETWORK_PASSWORD_SALT_SIZE;
      challenge.challengeLength = NETWORK_PASSWORD_CHALLENGE_SIZE;
      if (!NetworkAuthCrypto::RandomBytes(
            challenge.salt, challenge.saltLength) ||
          !NetworkAuthCrypto::RandomBytes(
            challenge.challenge, challenge.challengeLength))
      {
        NetworkAuthCrypto::SecureZero(&challenge, sizeof(challenge));
        return false;
      }
    }

    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.closing ||
          connection.handshake != HandshakeState::WAITING_CAPABILITIES)
      {
        NetworkAuthCrypto::SecureZero(&challenge, sizeof(challenge));
        return false;
      }

      if (settings.auth == LG_NET_AUTH_PASSWORD)
      {
        connection.passwordChallenge = challenge;
        connection.handshake         = HandshakeState::WAITING_AUTH_RESPONSE;
      }
      else
        connection.handshake = HandshakeState::PUBLISHING_CAPABILITIES;
    }

    NetworkAuthCrypto::SecureZero(&challenge, sizeof(challenge));

    if (!SendHelloAcknowledgement(connection))
      return false;

    return settings.auth == LG_NET_AUTH_PASSWORD ?
      SendPasswordChallenge(connection) : PublishCapabilities(connection);
  }

  bool SendAuthResult(Connection& connection,
    const LGNetAuthResult& result, uint64_t requestID)
  {
    const size_t payloadSize = lgNetAuthResultSize(&result);
    uint8_t payload[LG_NET_AUTH_RESULT_HEADER_SIZE +
      LG_NET_PASSWORD_PROOF_MAX_LENGTH];

    const bool sent = payloadSize &&
      lgNetAuthResultEncode(payload, payloadSize, &result) &&
      SendPacket(connection, LG_NET_SERVICE_CORE,
        LG_NET_CORE_MESSAGE_AUTH_RESULT,
        LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
        0, requestID, payload, static_cast<uint32_t>(payloadSize));
    NetworkAuthCrypto::SecureZero(payload, sizeof(payload));
    return sent;
  }

  bool HandleAuthResponse(Connection& connection,
    const LGNetPacketView& packet)
  {
    LGNetPasswordResponse response = {};
    if (lgNetPasswordResponseDecode(&response, packet.payload,
          packet.payloadLength) != LG_NET_PARSE_OK)
    {
      NetworkAuthCrypto::SecureZero(&response, sizeof(response));
      return false;
    }

    LGNetPasswordChallenge challenge    = {};
    uint64_t               sessionEpoch = 0;

    uint8_t clientNonce[LG_NET_HELLO_NONCE_LENGTH] = {};
    uint8_t serverNonce[LG_NET_HELLO_NONCE_LENGTH] = {};
    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.closing ||
          connection.handshake != HandshakeState::WAITING_AUTH_RESPONSE ||
          packet.envelope.requestID != connection.handshakeRequestID ||
          packet.envelope.sessionEpoch != connection.sessionEpoch ||
          packet.envelope.componentEpoch != 0)
      {
        NetworkAuthCrypto::SecureZero(&response, sizeof(response));
        return false;
      }

      connection.handshake = HandshakeState::AUTHENTICATING;
      challenge            = connection.passwordChallenge;
      sessionEpoch         = connection.sessionEpoch;
      memcpy(clientNonce, connection.clientNonce, sizeof(clientNonce));
      memcpy(serverNonce, connection.serverNonce, sizeof(serverNonce));
    }

    std::vector<uint8_t> password;

    uint8_t derivedKey      [LG_NET_PASSWORD_SHA256_LENGTH]                    = {};
    uint8_t expectedProof   [LG_NET_PASSWORD_SHA256_LENGTH]                    = {};
    uint8_t serverProof     [LG_NET_PASSWORD_SHA256_LENGTH]                    = {};
    uint8_t clientTranscript[LG_NET_PASSWORD_CLIENT_PROOF_TRANSCRIPT_MAX_SIZE] = {};
    uint8_t serverTranscript[LG_NET_PASSWORD_SERVER_PROOF_TRANSCRIPT_MAX_SIZE] = {};

    size_t clientTranscriptSize = 0;
    size_t serverTranscriptSize = 0;
    bool   cryptoError          = false;
    bool   accepted             = false;

    LGNetPasswordProofContext context = {};
    context.protocolVersion = LG_NET_PROTOCOL_VERSION_CURRENT;
    context.sessionEpoch    = sessionEpoch;
    context.helloNonce      = clientNonce;
    context.helloAckNonce   = serverNonce;
    context.challenge       = &challenge;
    context.response        = &response;

    const bool responseShapeValid =
      response.algorithm == LG_NET_PASSWORD_PBKDF2_HMAC_SHA256 &&
      response.nonceLength == NETWORK_PASSWORD_NONCE_SIZE &&
      response.proofLength == LG_NET_PASSWORD_SHA256_LENGTH;
    if (responseShapeValid)
    {
      if (!PasswordUTF8(settings.password, password) ||
          !NetworkAuthCrypto::PBKDF2HMACSHA256(
            password.data(), password.size(), challenge.salt,
            challenge.saltLength, challenge.rounds,
            derivedKey, sizeof(derivedKey)) ||
          !lgNetPasswordClientProofTranscriptSize(
            &context, &clientTranscriptSize) ||
          clientTranscriptSize > sizeof(clientTranscript) ||
          !lgNetPasswordClientProofTranscriptEncode(
            clientTranscript, clientTranscriptSize, &context) ||
          !NetworkAuthCrypto::HMACSHA256(
            derivedKey, sizeof(derivedKey), clientTranscript,
            clientTranscriptSize, expectedProof))
        cryptoError = true;
      else
        accepted = NetworkAuthCrypto::ConstantTimeEqual(
          expectedProof, response.proof, sizeof(expectedProof));

      if (accepted &&
          (!lgNetPasswordServerProofTranscriptSize(
             &context, &serverTranscriptSize) ||
           serverTranscriptSize > sizeof(serverTranscript) ||
           !lgNetPasswordServerProofTranscriptEncode(
             serverTranscript, serverTranscriptSize, &context) ||
           !NetworkAuthCrypto::HMACSHA256(
             derivedKey, sizeof(derivedKey), serverTranscript,
             serverTranscriptSize, serverProof)))
      {
        accepted    = false;
        cryptoError = true;
      }
    }

    LGNetAuthResult result = {};
    if (accepted)
    {
      result.result      = LG_NET_AUTH_RESULT_ACCEPTED;
      result.proofLength = LG_NET_PASSWORD_SHA256_LENGTH;
      memcpy(result.proof, serverProof, result.proofLength);
    }
    else
    {
      result.result       = static_cast<LGNetAuthResultCode>(
        cryptoError ?
          LG_NET_AUTH_RESULT_ERROR : LG_NET_AUTH_RESULT_REJECTED);
      result.retryAfterMs = NETWORK_AUTH_RETRY_AFTER_MS;
    }

    bool connectionValid;
    {
      CSRWExclusiveLock guard(connection.lock);
      connectionValid = !connection.closing &&
        connection.handshake == HandshakeState::AUTHENTICATING;
      if (connectionValid)
      {
        if (accepted)
          connection.handshake = HandshakeState::PUBLISHING_CAPABILITIES;
        else
        {
          connection.handshake         = HandshakeState::AUTH_FAILED;
          connection.handshakeDeadline =
            GetTickCount64() + NETWORK_AUTH_RETRY_AFTER_MS;
        }
      }
      NetworkAuthCrypto::SecureZero(
        &connection.passwordChallenge, sizeof(connection.passwordChallenge));
      NetworkAuthCrypto::SecureZero(
        connection.clientNonce, sizeof(connection.clientNonce));
      NetworkAuthCrypto::SecureZero(
        connection.serverNonce, sizeof(connection.serverNonce));
    }

    NetworkAuthCrypto::SecureZero(password.data(), password.size());
    NetworkAuthCrypto::SecureZero(derivedKey, sizeof(derivedKey));
    NetworkAuthCrypto::SecureZero(expectedProof, sizeof(expectedProof));
    NetworkAuthCrypto::SecureZero(serverProof, sizeof(serverProof));
    NetworkAuthCrypto::SecureZero(
      clientTranscript, sizeof(clientTranscript));
    NetworkAuthCrypto::SecureZero(
      serverTranscript, sizeof(serverTranscript));
    NetworkAuthCrypto::SecureZero(clientNonce, sizeof(clientNonce));
    NetworkAuthCrypto::SecureZero(serverNonce, sizeof(serverNonce));
    NetworkAuthCrypto::SecureZero(&challenge, sizeof(challenge));
    NetworkAuthCrypto::SecureZero(&response, sizeof(response));

    const bool resultSent = connectionValid &&
      SendAuthResult(connection, result, packet.envelope.requestID);
    NetworkAuthCrypto::SecureZero(&result, sizeof(result));
    if (!resultSent)
      return false;

    if (!accepted)
    {
      if (cryptoError)
        DEBUG_ERROR("Password authentication failed internally for network "
          "client %u", connection.client);
      else
        DEBUG_WARN("Network client %u failed password authentication",
          connection.client);
      return true;
    }

    return PublishCapabilities(connection);
  }

  static LGNetRecoveryState RecoveryStateCode(RecoveryState state)
  {
    switch (state)
    {
      case RecoveryState::NORMAL:
        return LG_NET_RECOVERY_STATE_NORMAL;

      case RecoveryState::ACTIVE:
        return LG_NET_RECOVERY_STATE_ACTIVE;

      case RecoveryState::FAILED:
        return LG_NET_RECOVERY_STATE_FAILED;
    }

    return LG_NET_RECOVERY_STATE_FAILED;
  }

  static LGNetRecoveryError RecoveryErrorCode(uint32_t error)
  {
    switch (error)
    {
      case ERROR_SUCCESS:
        return LG_NET_RECOVERY_ERROR_NONE;

      case ERROR_BUSY:
        return LG_NET_RECOVERY_ERROR_BUSY;

      case ERROR_NOT_ENOUGH_QUOTA:
      case ERROR_OUTOFMEMORY:
        return LG_NET_RECOVERY_ERROR_CAPACITY;

      case ERROR_TIMEOUT:
        return LG_NET_RECOVERY_ERROR_TIMEOUT;

      case RPC_S_SERVER_UNAVAILABLE:
        return LG_NET_RECOVERY_ERROR_HELPER_UNAVAILABLE;

      default:
        return LG_NET_RECOVERY_ERROR_TOPOLOGY_FAILED;
    }
  }

  static LGNetRecoveryFlags RecoveryFlags(
    LGNetRecoveryState state, bool active)
  {
    LGNetRecoveryFlags flags = LG_NET_RECOVERY_SUPPORTED;
    if (state == LG_NET_RECOVERY_STATE_ACTIVE ||
        (state == LG_NET_RECOVERY_STATE_SWITCHING && active))
      flags |= LG_NET_RECOVERY_ACTIVE;
    return flags;
  }

  bool SendRecoveryStatus(Connection& connection, uint64_t requestID,
    LGNetRecoveryState state, LGNetRecoveryError error, bool active,
    uint32_t detail, LGNetEnvelopeFlags envelopeFlags, bool updateState)
  {
    LGNetRecoveryStatus status = {};
    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.closing ||
          connection.recoveryStatusSequence == UINT64_MAX)
        return false;

      status.requestID      = requestID;
      status.sessionID      = connection.sessionEpoch;
      status.statusSequence = ++connection.recoveryStatusSequence;
      status.state          = state;
      status.error          = error;
      status.flags          = RecoveryFlags(state, active);
      status.detail         = detail;
      if (updateState)
      {
        connection.recoveryState = state;
        connection.recoveryError = error;
        connection.recoveryFlags = status.flags;
      }
    }

    uint8_t payload[LG_NET_RECOVERY_STATUS_WIRE_SIZE];
    return lgNetRecoveryStatusEncode(payload, sizeof(payload), &status) &&
      SendPacket(connection, LG_NET_SERVICE_RECOVERY,
        LG_NET_RECOVERY_MESSAGE_STATUS,
        envelopeFlags,
        0, requestID, payload, sizeof(payload));
  }

  bool SendCurrentRecoveryStatus(Connection& connection,
    uint64_t requestID, uint32_t detail)
  {
    LGNetRecoveryStatus status = {};
    {
      CSRWSharedLock guard(connection.lock);
      status.requestID      = requestID;
      status.sessionID      = connection.sessionEpoch;
      status.statusSequence = connection.recoveryStatusSequence;
      status.state          = connection.recoveryState;
      status.error          = connection.recoveryError;
      status.flags          = connection.recoveryFlags;
      status.detail         = detail;
    }

    uint8_t payload[LG_NET_RECOVERY_STATUS_WIRE_SIZE];
    return lgNetRecoveryStatusEncode(payload, sizeof(payload), &status) &&
      SendPacket(connection, LG_NET_SERVICE_RECOVERY,
        LG_NET_RECOVERY_MESSAGE_STATUS,
        LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
        0, requestID, payload, sizeof(payload));
  }

  bool SendRecoveryInfo(Connection& connection,
    const LGNetPacketView& packet)
  {
    if (packet.payloadLength || packet.envelope.componentEpoch ||
        !packet.envelope.requestID)
      return false;

    static const char version[] = LG_VERSION_STR;

    LGNetRecoveryInfo info = {};
    {
      CSRWSharedLock guard(connection.lock);
      info.sessionID        = connection.sessionEpoch;
      info.statusSequence   = connection.recoveryStatusSequence;
      info.capabilities     = LG_NET_RECOVERY_CAP_DISPLAY;
      info.state            = connection.recoveryState;
      info.error            = connection.recoveryError;
      info.flags            = connection.recoveryFlags;
      info.maxTransitionMs  = LG_NET_RECOVERY_MAX_TIMEOUT_MS;
      info.versionLength    = static_cast<uint16_t>(sizeof(version) - 1U);
      info.version          = reinterpret_cast<const uint8_t *>(version);
    }

    const size_t payloadSize = lgNetRecoveryInfoSize(&info);
    std::vector<uint8_t> payload;
    try
    {
      payload.resize(payloadSize);
    }
    catch (...)
    {
      return false;
    }

    return payloadSize &&
      lgNetRecoveryInfoEncode(payload.data(), payload.size(), &info) &&
      SendPacket(connection, LG_NET_SERVICE_RECOVERY,
        LG_NET_RECOVERY_MESSAGE_INFO,
        LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
        0, packet.envelope.requestID, payload.data(),
        static_cast<uint32_t>(payload.size()));
  }

  uint32_t AllocateRecoverySerialLocked(Connection& connection)
  {
    for (uint64_t attempt = 0; attempt < UINT32_MAX; ++attempt)
    {
      const uint32_t serial = connection.recoveryNextSerial++;
      if (!connection.recoveryNextSerial)
        connection.recoveryNextSerial = 1;
      if (!serial)
        continue;

      bool used = false;
      for (const Connection::RecoveryRequest& request :
          connection.recoveryRequests)
        if (request.requestID && request.serial == serial)
        {
          used = true;
          break;
        }
      if (!used)
        return serial;
    }
    return 0;
  }

  Connection::RecoveryRequest * AllocateRecoveryRequestLocked(
    Connection& connection, uint64_t requestID)
  {
    for (const Connection::RecoveryRequest& request :
        connection.recoveryRequests)
      if (request.requestID == requestID)
        return nullptr;

    for (Connection::RecoveryRequest& request :
        connection.recoveryRequests)
      if (!request.requestID)
        return &request;
    return nullptr;
  }

  static bool DeadlineReached(uint64_t now, uint64_t deadline)
  {
    return static_cast<int64_t>(now - deadline) >= 0;
  }

  static void RemoveRecoveryRequest(Connection& connection,
    uint32_t serial)
  {
    CSRWExclusiveLock guard(connection.lock);
    for (Connection::RecoveryRequest& request :
        connection.recoveryRequests)
      if (request.requestID && request.serial == serial)
      {
        request = {};
        return;
      }
  }

  bool HandleRecoveryRequest(Connection& connection,
    const LGNetPacketView& packet)
  {
    LGNetRecoveryRequest request;
    if (lgNetRecoveryRequestDecode(&request, packet.payload,
          packet.payloadLength) != LG_NET_PARSE_OK ||
        packet.envelope.componentEpoch ||
        packet.envelope.requestID != request.requestID)
      return false;

    std::shared_ptr<CNetworkSession> session;
    uint64_t                         statusSequence;
    {
      CSRWSharedLock guard(connection.lock);
      if (request.sessionID != connection.sessionEpoch)
        return false;
      session        = connection.session;
      statusSequence = connection.recoveryStatusSequence;
    }
    if (!session)
      return false;

    if (request.expectedStatusSequence != statusSequence &&
        !(request.flags & LG_NET_RECOVERY_REQUEST_FORCE))
      return SendCurrentRecoveryStatus(
        connection, request.requestID, ERROR_REVISION_MISMATCH);

    const bool active =
      (request.flags & LG_NET_RECOVERY_REQUEST_ACTIVE) != 0;
    uint32_t serial      = 0;
    bool     dispatchNow = true;
    {
      CSRWExclusiveLock guard(connection.lock);
      Connection::RecoveryRequest * correlation =
        AllocateRecoveryRequestLocked(connection, request.requestID);
      if (correlation)
      {
        for (const Connection::RecoveryRequest& pending :
            connection.recoveryRequests)
          if (pending.requestID && pending.active != active)
          {
            correlation = nullptr;
            break;
          }
      }
      if (correlation)
      {
        serial = AllocateRecoverySerialLocked(connection);
        if (serial)
        {
          for (const Connection::RecoveryRequest& pending :
              connection.recoveryRequests)
            if (pending.requestID)
            {
              dispatchNow = false;
              break;
            }

          correlation->requestID  = request.requestID;
          correlation->sessionID  = request.sessionID;
          correlation->deadline   = GetTickCount64() + request.timeoutMs;
          correlation->sequence   =
            connection.recoveryNextRequestSequence++;
          if (!connection.recoveryNextRequestSequence)
            connection.recoveryNextRequestSequence = 1;
          correlation->serial     = serial;
          correlation->active     = active;
          correlation->dispatched = dispatchNow;
        }
      }
    }

    if (!serial)
      return SendRecoveryStatus(connection, request.requestID,
        LG_NET_RECOVERY_STATE_FAILED, LG_NET_RECOVERY_ERROR_BUSY,
        active, ERROR_BUSY,
        LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL, false);

    if (dispatchNow &&
        !session->RequestRecovery(request.sessionID, serial, active))
    {
      RemoveRecoveryRequest(connection, serial);
      return SendRecoveryStatus(connection, request.requestID,
        LG_NET_RECOVERY_STATE_FAILED, LG_NET_RECOVERY_ERROR_CAPACITY,
        active, ERROR_NOT_ENOUGH_QUOTA,
        LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL, false);
    }

    return SendRecoveryStatus(connection, request.requestID,
      LG_NET_RECOVERY_STATE_SWITCHING, LG_NET_RECOVERY_ERROR_NONE,
      active, ERROR_SUCCESS, LG_NET_ENVELOPE_RESPONSE, true);
  }

  bool HandleRecovery(Connection& connection,
    const LGNetPacketView& packet)
  {
    switch (packet.envelope.messageType)
    {
      case LG_NET_RECOVERY_MESSAGE_GET_INFO:
        return SendRecoveryInfo(connection, packet);

      case LG_NET_RECOVERY_MESSAGE_REQUEST:
        return HandleRecoveryRequest(connection, packet);

      default:
        return false;
    }
  }

  static LGNetInputStatusCode InputStatusCode(InputResult result)
  {
    switch (result)
    {
      case InputResult::ACCEPTED:
        return LG_NET_INPUT_STATUS_ACCEPTED;

      case InputResult::BUSY:
        return LG_NET_INPUT_STATUS_REJECTED;

      case InputResult::UNAVAILABLE:
        return LG_NET_INPUT_STATUS_UNSUPPORTED;

      case InputResult::STALE:
        return LG_NET_INPUT_STATUS_EXPIRED;
    }

    return LG_NET_INPUT_STATUS_ERROR;
  }

  static void ClearInputStateLocked(Connection& connection,
    bool clearClaim)
  {
    if (clearClaim)
    {
      connection.inputClaimantID    = 0;
      connection.inputClaimEpoch    = 0;
      connection.inputFlags         = 0;
      connection.inputLEDState      = 0;
      connection.inputLEDStateKnown = false;
    }
    connection.inputPointerButtons = 0;
    connection.keyboardModifiers   = 0;
    memset(connection.keyboardKeys, 0, sizeof(connection.keyboardKeys));
  }

  static void InvalidateInputClaim(Connection& connection,
    uint64_t claimEpoch)
  {
    CSRWExclusiveLock guard(connection.lock);
    if (connection.inputClaimEpoch == claimEpoch)
      ClearInputStateLocked(connection, true);
  }

  bool SendInputStatus(Connection& connection, uint16_t messageType,
    LGNetInputStatusCode code, uint64_t claimEpoch, uint64_t requestID,
    LGNetEnvelopeFlags flags)
  {
    LGNetInputStatus status = {};
    {
      CSRWSharedLock guard(connection.lock);
      status.claimEpoch       = claimEpoch;
      status.lastSequence     = connection.inputReliableSequence >
        connection.inputDatagramSequence ?
          connection.inputReliableSequence :
          connection.inputDatagramSequence;
      status.status           = code;
      status.flags            = code == LG_NET_INPUT_STATUS_ACCEPTED ?
        connection.inputFlags : 0;
      status.leaseRemainingMs = code == LG_NET_INPUT_STATUS_ACCEPTED ?
        NETWORK_INPUT_LEASE_MS : 0;
      status.detail           = 0;
    }

    uint8_t payload[LG_NET_INPUT_STATUS_WIRE_SIZE];
    return lgNetInputStatusEncode(payload, sizeof(payload), &status) &&
      SendPacket(connection, LG_NET_SERVICE_INPUT, messageType,
        flags | LG_NET_ENVELOPE_FINAL, claimEpoch, requestID,
        payload, sizeof(payload));
  }

  bool SendInputLEDs(Connection& connection)
  {
    std::shared_ptr<CNetworkSession> session;
    uint64_t                         claimEpoch;
    {
      CSRWSharedLock guard(connection.lock);
      session    = connection.session;
      claimEpoch = connection.inputClaimEpoch;
    }
    if (!session || !claimEpoch)
      return false;

    const InputTargetState state = session->InputState();
    if (!state.owned || !state.keyboardLEDsValid)
      return true;

    LGNetInputLEDs leds = {};
    leds.state           = state.keyboardLEDs & NETWORK_KEYBOARD_LED_MASK;
    leds.validMask       = NETWORK_KEYBOARD_LED_MASK;
    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.inputClaimEpoch != claimEpoch)
        return false;
      if (connection.inputLEDStateKnown &&
          connection.inputLEDState == leds.state)
        return true;
      if (connection.inputLEDSequence == UINT64_MAX)
        return false;
      leds.sequence                 = ++connection.inputLEDSequence;
      connection.inputLEDState      = leds.state;
      connection.inputLEDStateKnown = true;
    }

    uint8_t payload[LG_NET_INPUT_LEDS_WIRE_SIZE];
    return lgNetInputLEDsEncode(payload, sizeof(payload), &leds) &&
      SendPacket(connection, LG_NET_SERVICE_INPUT,
        LG_NET_INPUT_MESSAGE_KEYBOARD_LEDS, LG_NET_ENVELOPE_FINAL,
        claimEpoch, 0, payload, sizeof(payload));
  }

  bool HandleInputClaim(Connection& connection,
    const LGNetPacketView& packet)
  {
    LGNetInputClaim claim;
    if (lgNetInputClaimDecode(&claim, packet.payload,
          packet.payloadLength) != LG_NET_PARSE_OK ||
        packet.envelope.componentEpoch != claim.claimEpoch ||
        packet.envelope.requestID != claim.claimEpoch)
      return false;

    std::shared_ptr<CNetworkSession> session;
    {
      CSRWSharedLock guard(connection.lock);
      session = connection.session;
    }
    if (!session)
      return false;

    bool hadClaim;
    {
      CSRWSharedLock guard(connection.lock);
      hadClaim = connection.inputClaimantID != 0;
    }

    InputResult result = session->ClaimInput();
    if (result == InputResult::ACCEPTED && hadClaim)
      result = session->ResetInput();
    {
      CSRWExclusiveLock guard(connection.lock);
      const bool accepted = result == InputResult::ACCEPTED;
      connection.inputClaimantID       = accepted ? claim.claimantID : 0;
      connection.inputClaimEpoch       = accepted ? claim.claimEpoch : 0;
      connection.inputReliableSequence = 0;
      connection.inputDatagramSequence = 0;
      connection.inputFlags            = accepted ? claim.flags : 0;
      connection.inputLEDState         = 0;
      connection.inputLEDStateKnown    = false;
      ClearInputStateLocked(connection, false);
    }

    const LGNetInputStatusCode code = InputStatusCode(result);
    return SendInputStatus(connection, LG_NET_INPUT_MESSAGE_CLAIM_RESULT,
        code, claim.claimEpoch, packet.envelope.requestID,
        LG_NET_ENVELOPE_RESPONSE) &&
      (result != InputResult::ACCEPTED || SendInputLEDs(connection));
  }

  bool ValidateInputOwner(Connection& connection,
    uint64_t claimEpoch, uint64_t sequence, bool datagram,
    uint64_t claimantID, LGNetInputClaimFlags requiredFlags)
  {
    CSRWExclusiveLock guard(connection.lock);
    uint64_t& lastSequence = datagram ?
      connection.inputDatagramSequence :
      connection.inputReliableSequence;
    if (!connection.inputClaimantID ||
        connection.inputClaimEpoch != claimEpoch ||
        (claimantID && connection.inputClaimantID != claimantID) ||
        (connection.inputFlags & requiredFlags) != requiredFlags ||
        (datagram && sequence <= connection.inputReliableSequence) ||
        sequence <= lastSequence)
      return false;

    lastSequence = sequence;
    return true;
  }

  bool ValidateInputPointer(Connection& connection, uint64_t claimEpoch,
    uint64_t sequence, bool datagram, LGNetPointerButtons& buttons)
  {
    CSRWExclusiveLock guard(connection.lock);
    uint64_t& lastSequence = datagram ?
      connection.inputDatagramSequence :
      connection.inputReliableSequence;
    if (!connection.inputClaimantID ||
        connection.inputClaimEpoch != claimEpoch ||
        !(connection.inputFlags & LG_NET_INPUT_CLAIM_POINTER) ||
        (datagram && sequence <= connection.inputReliableSequence) ||
        sequence <= lastSequence)
      return false;

    lastSequence = sequence;
    if (datagram)
      buttons = connection.inputPointerButtons;
    else
      connection.inputPointerButtons = buttons;
    return true;
  }

  bool HandleInputControl(Connection& connection,
    const LGNetPacketView& packet, bool datagram)
  {
    LGNetInputControl control;
    if (lgNetInputControlDecode(&control, packet.payload,
          packet.payloadLength) != LG_NET_PARSE_OK ||
        packet.envelope.componentEpoch != control.claimEpoch ||
        (datagram && packet.envelope.messageType !=
          LG_NET_INPUT_MESSAGE_KEEPALIVE))
      return false;

    if (!ValidateInputOwner(connection, control.claimEpoch,
          control.sequence, datagram, control.claimantID, 0))
      return true;

    std::shared_ptr<CNetworkSession> session;
    {
      CSRWSharedLock guard(connection.lock);
      session = connection.session;
    }
    if (!session)
      return false;

    InputResult result;
    switch (packet.envelope.messageType)
    {
      case LG_NET_INPUT_MESSAGE_KEEPALIVE:
        result = session->TouchInput();
        break;

      case LG_NET_INPUT_MESSAGE_RESET:
        result = session->ResetInput();
        {
          CSRWExclusiveLock guard(connection.lock);
          ClearInputStateLocked(connection, false);
        }
        break;

      case LG_NET_INPUT_MESSAGE_RELEASE:
        result = session->ReleaseInput(false);
        {
          CSRWExclusiveLock guard(connection.lock);
          ClearInputStateLocked(connection, true);
        }
        break;

      default:
        return false;
    }

    if (result == InputResult::ACCEPTED &&
        packet.envelope.messageType == LG_NET_INPUT_MESSAGE_KEEPALIVE)
      return SendInputLEDs(connection);

    const LGNetInputStatusCode code =
      packet.envelope.messageType == LG_NET_INPUT_MESSAGE_RELEASE &&
        result == InputResult::ACCEPTED ?
          static_cast<LGNetInputStatusCode>(LG_NET_INPUT_STATUS_RELEASED) :
          InputStatusCode(result);
    if (result != InputResult::ACCEPTED)
      InvalidateInputClaim(connection, control.claimEpoch);
    return SendInputStatus(connection, LG_NET_INPUT_MESSAGE_STATUS,
      code, control.claimEpoch, 0, 0);
  }

  bool HandleInputKeyboard(Connection& connection,
    const LGNetPacketView& packet)
  {
    LGNetInputKeyboard keyboard;
    if (lgNetInputKeyboardDecode(&keyboard, packet.payload,
        packet.payloadLength) != LG_NET_PARSE_OK ||
        keyboard.usagePage != NETWORK_KEYBOARD_USAGE_PAGE ||
        !keyboard.usage || keyboard.usage >= 256 ||
        packet.envelope.componentEpoch == 0)
      return false;

    if (!ValidateInputOwner(connection, packet.envelope.componentEpoch,
          keyboard.sequence, false, 0, LG_NET_INPUT_CLAIM_KEYBOARD))
      return true;

    uint8_t keys[NETWORK_KEYBOARD_KEY_COUNT] = {};
    {
      CSRWExclusiveLock guard(connection.lock);
      connection.keyboardModifiers =
        static_cast<uint8_t>(keyboard.modifiers);
      if (keyboard.usage < 224 || keyboard.usage > 231)
        connection.keyboardKeys[keyboard.usage] =
          (keyboard.flags & LG_NET_KEYBOARD_DOWN) != 0;

      size_t count = 0;
      for (size_t usage = 1; usage < 224; ++usage)
      {
        if (!connection.keyboardKeys[usage])
          continue;
        if (count < NETWORK_KEYBOARD_KEY_COUNT)
          keys[count] = static_cast<uint8_t>(usage);
        ++count;
      }
      if (count > NETWORK_KEYBOARD_KEY_COUNT)
        memset(keys, NETWORK_KEYBOARD_ROLLOVER, sizeof(keys));
    }

    std::shared_ptr<CNetworkSession> session;
    uint8_t                          modifiers;
    {
      CSRWSharedLock guard(connection.lock);
      session   = connection.session;
      modifiers = connection.keyboardModifiers;
    }
    if (!session)
      return false;

    const InputResult result = session->SendKeyboard(modifiers, keys);
    if (result != InputResult::ACCEPTED)
    {
      InvalidateInputClaim(connection, packet.envelope.componentEpoch);
      return SendInputStatus(connection, LG_NET_INPUT_MESSAGE_STATUS,
        InputStatusCode(result), packet.envelope.componentEpoch, 0, 0);
    }
    return SendInputLEDs(connection);
  }

  static uint16_t NormalizeAbsolute(int32_t position, uint32_t extent)
  {
    if (extent <= 1)
      return 0;
    return static_cast<uint16_t>(
      static_cast<uint64_t>(position) * UINT16_C(0x7FFF) /
        (extent - 1U));
  }

  bool HandleInputPointer(Connection& connection,
    const LGNetPacketView& packet, bool datagram)
  {
    uint64_t                         sequence;
    InputResult                      result;
    std::shared_ptr<CNetworkSession> session;
    {
      CSRWSharedLock guard(connection.lock);
      session = connection.session;
    }
    if (!session)
      return false;

    if (packet.envelope.messageType ==
        LG_NET_INPUT_MESSAGE_MOUSE_RELATIVE)
    {
      LGNetInputRelative relative;
      if (lgNetInputRelativeDecode(&relative, packet.payload,
            packet.payloadLength) != LG_NET_PARSE_OK || relative.wheelX ||
          (datagram && (relative.wheelY || relative.changedButtons)))
        return false;
      sequence = relative.sequence;
      if (!ValidateInputPointer(connection, packet.envelope.componentEpoch,
            sequence, datagram, relative.buttons))
        return true;
      result = session->SendMouseRelative(relative.deltaX, relative.deltaY,
        relative.wheelY, relative.buttons);
    }
    else
    {
      LGNetInputAbsolute absolute;
      if (lgNetInputAbsoluteDecode(&absolute, packet.payload,
            packet.payloadLength) != LG_NET_PARSE_OK || absolute.wheelX ||
          (datagram && (absolute.wheelY || absolute.changedButtons)))
        return false;
      sequence = absolute.sequence;
      if (!ValidateInputPointer(connection, packet.envelope.componentEpoch,
            sequence, datagram, absolute.buttons))
        return true;
      result = session->SendMouseAbsolute(
        NormalizeAbsolute(absolute.x, absolute.width),
        NormalizeAbsolute(absolute.y, absolute.height),
        absolute.wheelY, absolute.buttons);
    }

    if (result == InputResult::ACCEPTED)
      return true;
    InvalidateInputClaim(connection, packet.envelope.componentEpoch);
    return SendInputStatus(connection, LG_NET_INPUT_MESSAGE_STATUS,
      InputStatusCode(result), packet.envelope.componentEpoch, 0, 0);
  }

  bool HandleInput(Connection& connection, const LGNetPacketView& packet,
    bool datagram)
  {
    if (datagram &&
        packet.envelope.messageType != LG_NET_INPUT_MESSAGE_KEEPALIVE &&
        packet.envelope.messageType !=
          LG_NET_INPUT_MESSAGE_MOUSE_RELATIVE &&
        packet.envelope.messageType !=
          LG_NET_INPUT_MESSAGE_MOUSE_ABSOLUTE)
      return false;

    switch (packet.envelope.messageType)
    {
      case LG_NET_INPUT_MESSAGE_CLAIM:
        return HandleInputClaim(connection, packet);

      case LG_NET_INPUT_MESSAGE_KEEPALIVE:
      case LG_NET_INPUT_MESSAGE_RELEASE:
      case LG_NET_INPUT_MESSAGE_RESET:
        return HandleInputControl(connection, packet, datagram);

      case LG_NET_INPUT_MESSAGE_KEYBOARD:
        return HandleInputKeyboard(connection, packet);

      case LG_NET_INPUT_MESSAGE_MOUSE_RELATIVE:
      case LG_NET_INPUT_MESSAGE_MOUSE_ABSOLUTE:
        return HandleInputPointer(connection, packet, datagram);

      default:
        return false;
    }
  }

  bool SendControlStatus(Connection& connection,
    const NetworkControlStatus& source, LGNetEnvelopeFlags flags)
  {
    LGNetControlStatus status = {};
    status.controlID = source.controlID;
    status.status    = source.status;
    status.detail    = source.detail;

    uint8_t payload[LG_NET_CONTROL_STATUS_WIRE_SIZE];
    return lgNetControlStatusEncode(payload, sizeof(payload), &status) &&
      SendPacket(connection, LG_NET_SERVICE_CONTROL,
        LG_NET_CONTROL_MESSAGE_STATUS, flags,
        0, status.controlID, payload, sizeof(payload));
  }

  bool HandleControl(Connection& connection,
    const LGNetPacketView& packet)
  {
    if (packet.envelope.componentEpoch || !packet.envelope.requestID)
      return false;

    std::shared_ptr<CNetworkSession> session;
    uint32_t                         client;
    {
      CSRWSharedLock guard(connection.lock);
      session = connection.session;
      client  = connection.client;
    }
    if (!session)
      return false;

    uint64_t controlID = 0;
    bool     queued    = false;
    switch (packet.envelope.messageType)
    {
      case LG_NET_CONTROL_MESSAGE_CURSOR_POSITION:
      {
        LGNetControlCursorPosition position;
        if (lgNetControlCursorPositionDecode(&position, packet.payload,
              packet.payloadLength) != LG_NET_PARSE_OK ||
            packet.envelope.requestID != position.controlID)
          return false;
        controlID = position.controlID;
        queued    = session->SetCursorPosition(
          position.controlID, position.x, position.y);
        break;
      }

      case LG_NET_CONTROL_MESSAGE_DISPLAY_SIZE:
      {
        LGNetControlDisplaySize display;
        if (lgNetControlDisplaySizeDecode(&display, packet.payload,
              packet.payloadLength) != LG_NET_PARSE_OK ||
            packet.envelope.requestID != display.controlID)
          return false;
        controlID = display.controlID;
        queued    = session->SetResolution(
          display.controlID, display.width, display.height);
        break;
      }

      case LG_NET_CONTROL_MESSAGE_FRAME_SCHEDULE:
      {
        LGNetControlFrameSchedule schedule;
        if (lgNetControlFrameScheduleDecode(&schedule, packet.payload,
              packet.payloadLength) != LG_NET_PARSE_OK ||
            packet.envelope.requestID != schedule.controlID)
          return false;

        FrameScheduleUpdate update = {};
        update.clientID                 = client;
        update.generation               = schedule.generation;
        update.period                   = schedule.periodNs;
        update.targetSlack              = schedule.targetSlackNs;
        update.phaseError               = schedule.phaseErrorNs;
        update.feedbackFrameSerial      = schedule.feedbackFrameSerial;
        update.feedbackScheduleEpoch    = schedule.feedbackScheduleEpoch;
        update.feedbackDeadlineSerial   = schedule.feedbackDeadlineSerial;
        update.lease                    = schedule.leaseMs;
        if (schedule.flags & LG_NET_CONTROL_FRAME_SCHEDULE_ACTIVE)
          update.flags |= FRAME_SCHEDULE_ACTIVE;
        if (schedule.flags & LG_NET_CONTROL_FRAME_SCHEDULE_RELEASE)
          update.flags |= FRAME_SCHEDULE_RELEASE;
        if (schedule.flags & LG_NET_CONTROL_FRAME_SCHEDULE_RESET)
          update.flags |= FRAME_SCHEDULE_RESET;
        if (schedule.flags & LG_NET_CONTROL_FRAME_SCHEDULE_IMMEDIATE)
          update.flags |= FRAME_SCHEDULE_IMMEDIATE;
        controlID = schedule.controlID;
        queued    = session->SetFrameSchedule(schedule.controlID, update);
        break;
      }

      default:
        return false;
    }

    NetworkControlStatus status;
    status.controlID = controlID;
    status.status    = queued ?
      LG_NET_CONTROL_STATUS_PENDING : LG_NET_CONTROL_STATUS_BUSY;
    status.detail    = queued ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_QUOTA;
    return SendControlStatus(connection, status,
      LG_NET_ENVELOPE_RESPONSE |
        (queued ? 0 : LG_NET_ENVELOPE_FINAL));
  }

  bool HandleCursorStatus(Connection& connection,
    const LGNetPacketView& packet)
  {
    LGNetCursorStatus status;
    if (packet.envelope.messageType != LG_NET_CURSOR_MESSAGE_STATUS ||
        packet.envelope.componentEpoch ||
        lgNetCursorStatusDecode(&status, packet.payload,
          packet.payloadLength) != LG_NET_PARSE_OK ||
        packet.envelope.requestID != status.stateSequence)
      return false;

    CSRWExclusiveLock guard(connection.lock);
    if (!status.stateSequence ||
        status.stateSequence > connection.cursorStateSequence ||
        status.appliedShapeID > connection.cursorShapeRevision ||
        status.appliedPositionID > connection.cursorPositionRevision ||
        status.appliedTransformID)
      return false;
    if (status.stateSequence < connection.cursorAppliedStateSequence)
      return true;

    connection.cursorAppliedStateSequence = status.stateSequence;
    connection.cursorAppliedShapeID       = status.appliedShapeID;
    connection.cursorAppliedPositionID    = status.appliedPositionID;
    connection.cursorAppliedTransformID   = status.appliedTransformID;
    if (status.status != LG_NET_CURSOR_STATUS_APPLIED)
    {
      connection.cursorSnapshotRevision = 0;
      connection.cursorShapeRevision    = 0;
      connection.cursorPositionRevision = 0;
      connection.cursorColorRevision    = 0;
    }
    return true;
  }

  bool SendCursorShape(Connection& connection,
    const NetworkControlSnapshot& snapshot)
  {
    LGNetCursorShape shape = {};
    shape.shapeID          = snapshot.shapeRevision;
    shape.width            = snapshot.cursor.CursorShapeInfo.Width;
    shape.height           = snapshot.cursor.CursorShapeInfo.Height;
    shape.pitch            = snapshot.cursor.CursorShapeInfo.Pitch;
    shape.dataLength       = static_cast<uint32_t>(
      snapshot.cursorData.size());
    shape.format           = static_cast<LGNetCursorFormat>(
      snapshot.cursor.CursorShapeInfo.CursorType ==
          IDDCX_CURSOR_SHAPE_TYPE_ALPHA ?
        LG_NET_CURSOR_FORMAT_BGRA8_PREMULTIPLIED :
        LG_NET_CURSOR_FORMAT_MASKED_COLOR);
    shape.hotspotX         = snapshot.cursor.CursorShapeInfo.XHot;
    shape.hotspotY         = snapshot.cursor.CursorShapeInfo.YHot;
    shape.data             = snapshot.cursorData.data();

    const size_t payloadSize = lgNetCursorShapeSize(&shape);
    std::vector<uint8_t> payload;
    try
    {
      payload.resize(payloadSize);
    }
    catch (...)
    {
      return false;
    }

    return payloadSize &&
      lgNetCursorShapeEncode(payload.data(), payload.size(), &shape) &&
      SendPacket(connection, LG_NET_SERVICE_CURSOR,
        LG_NET_CURSOR_MESSAGE_SHAPE, LG_NET_ENVELOPE_FINAL,
        0, shape.shapeID, payload.data(),
        static_cast<uint32_t>(payload.size()));
  }

  bool SendCursorPosition(Connection& connection,
    const NetworkControlSnapshot& snapshot)
  {
    LGNetCursorPosition position = {};
    position.updateID      = snapshot.positionRevision;
    position.timestampNs   = NetworkTimeNs();
    position.x             = snapshot.cursor.X;
    position.y             = snapshot.cursor.Y;
    position.desktopWidth  = LG_NET_CURSOR_MAX_DESKTOP_WIDTH;
    position.desktopHeight = LG_NET_CURSOR_MAX_DESKTOP_HEIGHT;
    position.flags         = snapshot.cursor.IsCursorVisible ?
      LG_NET_CURSOR_POSITION_VISIBLE : 0;
    position.displayID     = 0;

    uint8_t payload[LG_NET_CURSOR_POSITION_WIRE_SIZE];
    return lgNetCursorPositionEncode(
        payload, sizeof(payload), &position) &&
      SendPacket(connection, LG_NET_SERVICE_CURSOR,
        LG_NET_CURSOR_MESSAGE_POSITION, LG_NET_ENVELOPE_FINAL,
        0, position.updateID, payload, sizeof(payload));
  }

  bool SendCursorColorTransform(Connection& connection,
    const NetworkControlSnapshot& snapshot)
  {
    std::vector<uint8_t>      lut;
    LGNetCursorColorTransform transform = {};
    transform.updateID                  = snapshot.colorRevision;
    transform.sdrWhiteLevel             = snapshot.sdrWhiteLevel ?
      snapshot.sdrWhiteLevel : LG_SDR_WHITE_LEVEL_DEFAULT;
    transform.scalar                    = 1.0f;
    if (snapshot.transform)
    {
      if (snapshot.transform->matrixEnabled)
        transform.flags |= LG_NET_CURSOR_COLOR_TRANSFORM_MATRIX;
      if (snapshot.transform->lutEnabled)
        transform.flags |= LG_NET_CURSOR_COLOR_TRANSFORM_LUT;
      memcpy(transform.matrix, snapshot.transform->matrix,
        sizeof(transform.matrix));
      transform.scalar = snapshot.transform->scalar;

      if (snapshot.transform->lutEnabled)
      {
        try
        {
          lut.resize(LG_NET_CURSOR_COLOR_LUT_BYTES);
        }
        catch (...)
        {
          return false;
        }
        if (!lgNetCursorColorLUTEncode(lut.data(), lut.size(),
              &snapshot.transform->lut[0][0]))
          return false;
        transform.lut = lut.data();
      }
    }

    const size_t payloadSize = lgNetCursorColorTransformSize(&transform);
    std::vector<uint8_t> payload;
    try
    {
      payload.resize(payloadSize);
    }
    catch (...)
    {
      return false;
    }

    return payloadSize && lgNetCursorColorTransformEncode(
        payload.data(), payload.size(), &transform) &&
      SendPacket(connection, LG_NET_SERVICE_CURSOR,
        LG_NET_CURSOR_MESSAGE_COLOR_TRANSFORM, LG_NET_ENVELOPE_FINAL,
        0, transform.updateID, payload.data(),
        static_cast<uint32_t>(payload.size()));
  }

  bool SendCursorState(Connection& connection,
    const NetworkControlSnapshot& snapshot, bool shapeAvailable)
  {
    LGNetCursorState state = {};
    state.stateSequence = snapshot.stateRevision;
    state.flags         = snapshot.cursor.IsCursorVisible ?
      LG_NET_CURSOR_STATE_VISIBLE : 0;
    if (shapeAvailable)
    {
      state.shapeID = snapshot.shapeRevision;
      state.flags  |= LG_NET_CURSOR_STATE_SHAPE_VALID;
    }
    if (snapshot.cursorValid)
    {
      state.positionID = snapshot.positionRevision;
      state.flags     |= LG_NET_CURSOR_STATE_POSITION_VALID;
    }
    state.displayID = 0;

    uint8_t payload[LG_NET_CURSOR_STATE_WIRE_SIZE];
    return lgNetCursorStateEncode(payload, sizeof(payload), &state) &&
      SendPacket(connection, LG_NET_SERVICE_CURSOR,
        LG_NET_CURSOR_MESSAGE_STATE, LG_NET_ENVELOPE_FINAL,
        0, state.stateSequence, payload, sizeof(payload));
  }

  bool PublishCursor(Connection& connection)
  {
    uint64_t knownRevision;
    uint64_t shapeRevision;
    uint64_t positionRevision;
    uint64_t colorRevision;
    uint32_t maxPayload;
    uint16_t serviceVersion;
    {
      CSRWSharedLock guard(connection.lock);
      const uint16_t serviceMask =
        UINT16_C(1) << LG_NET_SERVICE_CURSOR;
      if (connection.handshake != HandshakeState::READY ||
          !(connection.selectedServices & serviceMask))
        return true;
      knownRevision    = connection.cursorSnapshotRevision;
      shapeRevision    = connection.cursorShapeRevision;
      positionRevision = connection.cursorPositionRevision;
      colorRevision    = connection.cursorColorRevision;
      maxPayload       = connection.selectedCapabilities
        [LG_NET_SERVICE_CURSOR].maxPayloadLength;
      serviceVersion   = connection.selectedCapabilities
        [LG_NET_SERVICE_CURSOR].versionMin;
    }

    NetworkControlSnapshot snapshot;
    if (!transport.ControlSnapshot(snapshot, knownRevision))
      return true;

    bool shapeAvailable = snapshot.shapeValid;
    if (snapshot.shapeValid)
    {
      const uint64_t shapeSize =
        LG_NET_CURSOR_SHAPE_HEADER_WIRE_SIZE + snapshot.cursorData.size();
      shapeAvailable = shapeSize <= maxPayload;
      if (snapshot.shapeRevision != shapeRevision && shapeAvailable &&
          !SendCursorShape(connection, snapshot))
        return false;
    }
    if (snapshot.cursorValid &&
        snapshot.positionRevision != positionRevision &&
        !SendCursorPosition(connection, snapshot))
      return false;
    const uint32_t colorPayloadSize =
      LG_NET_CURSOR_COLOR_TRANSFORM_HEADER_WIRE_SIZE +
        (snapshot.transform && snapshot.transform->lutEnabled ?
          LG_NET_CURSOR_COLOR_LUT_BYTES : 0);
    if (snapshot.colorRevision != colorRevision &&
        serviceVersion >=
          LG_NET_CURSOR_COLOR_TRANSFORM_INTRODUCED_SERVICE_VERSION &&
        colorPayloadSize <= maxPayload &&
        !SendCursorColorTransform(connection, snapshot))
      return false;
    if (!SendCursorState(connection, snapshot, shapeAvailable))
      return false;

    {
      CSRWExclusiveLock guard(connection.lock);
      connection.cursorSnapshotRevision = snapshot.stateRevision;
      connection.cursorShapeRevision    = snapshot.shapeRevision;
      connection.cursorPositionRevision = snapshot.positionRevision;
      connection.cursorColorRevision    = snapshot.colorRevision;
      connection.cursorStateSequence    = snapshot.stateRevision;
    }
    return true;
  }

  bool DispatchQueuedRecovery(Connection& connection,
    const std::shared_ptr<CNetworkSession>& session)
  {
    for (;;)
    {
      Connection::RecoveryRequest selected;
      {
        CSRWExclusiveLock guard(connection.lock);
        for (const Connection::RecoveryRequest& request :
            connection.recoveryRequests)
          if (request.requestID && request.dispatched)
            return true;

        Connection::RecoveryRequest * candidate = nullptr;
        for (Connection::RecoveryRequest& request :
            connection.recoveryRequests)
          if (request.requestID &&
              (!candidate || request.sequence < candidate->sequence))
            candidate = &request;
        if (!candidate)
          return true;

        candidate->dispatched = true;
        selected              = *candidate;
      }

      if (session->RequestRecovery(
            selected.sessionID, selected.serial, selected.active))
        return true;

      RemoveRecoveryRequest(connection, selected.serial);
      if (!SendRecoveryStatus(connection, selected.requestID,
            LG_NET_RECOVERY_STATE_FAILED,
            LG_NET_RECOVERY_ERROR_CAPACITY, selected.active,
            ERROR_NOT_ENOUGH_QUOTA,
            LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL, false))
        return false;
    }
  }

  bool PumpSession(Connection& connection, uint64_t now)
  {
    std::shared_ptr<CNetworkSession> session;
    {
      CSRWSharedLock guard(connection.lock);
      if (connection.handshake != HandshakeState::READY)
        return true;
      session = connection.session;
    }
    if (!session)
      return false;

    NetworkRecoveryStatus recovery;
    while (session->TakeRecoveryStatus(recovery))
    {
      Connection::RecoveryRequest correlation;
      {
        CSRWExclusiveLock guard(connection.lock);
        for (Connection::RecoveryRequest& request :
            connection.recoveryRequests)
          if (request.requestID && request.sessionID == recovery.session &&
              request.serial == recovery.serial &&
              request.active == recovery.active)
          {
            correlation = request;
            request     = {};
            break;
          }
      }
      if (!correlation.requestID)
        continue;

      LGNetRecoveryState state = RecoveryStateCode(recovery.state);
      LGNetRecoveryError error = RecoveryErrorCode(recovery.error);
      if (state == LG_NET_RECOVERY_STATE_FAILED &&
          error == LG_NET_RECOVERY_ERROR_NONE)
        error = LG_NET_RECOVERY_ERROR_TOPOLOGY_FAILED;
      if (state != LG_NET_RECOVERY_STATE_FAILED)
        error = LG_NET_RECOVERY_ERROR_NONE;
      if (!SendRecoveryStatus(connection, correlation.requestID,
            state, error, recovery.active, recovery.error,
            LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL, true))
        return false;
    }

    Connection::RecoveryRequest expired[NETWORK_RECOVERY_REQUESTS]  = {};
    size_t                      expiredCount                        = 0;
    {
      CSRWExclusiveLock guard(connection.lock);
      for (Connection::RecoveryRequest& request :
          connection.recoveryRequests)
        if (request.requestID && DeadlineReached(now, request.deadline))
        {
          expired[expiredCount++] = request;
          request                 = {};
        }
    }
    for (size_t index = 0; index < expiredCount; ++index)
      if (!SendRecoveryStatus(connection, expired[index].requestID,
            LG_NET_RECOVERY_STATE_FAILED, LG_NET_RECOVERY_ERROR_TIMEOUT,
            expired[index].active, ERROR_TIMEOUT,
            LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL, true))
        return false;

    if (!DispatchQueuedRecovery(connection, session))
      return false;

    NetworkControlStatus control;
    while (session->TakeControlStatus(control))
      if (!SendControlStatus(connection, control,
            LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL))
        return false;

    return PublishCursor(connection);
  }

  bool HandlePacket(Connection& connection, const LGNetPacketView& packet)
  {
    const bool         cursorStatus     =
      packet.envelope.service == LG_NET_SERVICE_CURSOR &&
      packet.envelope.messageType == LG_NET_CURSOR_MESSAGE_STATUS;
    const bool         audioSubscribe   =
      packet.envelope.service == LG_NET_SERVICE_AUDIO &&
      packet.envelope.messageType == LG_NET_AUDIO_MESSAGE_SUBSCRIBE;
    const bool         videoSubscribe   =
      packet.envelope.service == LG_NET_SERVICE_VIDEO &&
      packet.envelope.messageType == LG_NET_VIDEO_MESSAGE_SUBSCRIBE;
    const bool         audioResponse    =
      packet.envelope.service == LG_NET_SERVICE_AUDIO &&
      packet.envelope.messageType == LG_NET_AUDIO_MESSAGE_STATE_ACK;
    const bool         clipboardPacket  =
      packet.envelope.service == LG_NET_SERVICE_CLIPBOARD ||
      packet.envelope.service == LG_NET_SERVICE_FILE;
    LGNetEnvelopeFlags expectedFlags    =
      LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_FINAL;
    if (audioSubscribe || videoSubscribe)
      expectedFlags |= LG_NET_ENVELOPE_ACK_REQUIRED;
    if (cursorStatus || audioResponse)
      expectedFlags |= LG_NET_ENVELOPE_RESPONSE;
    constexpr LGNetEnvelopeFlags DELIVERY_FLAGS =
      LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM |
      LG_NET_ENVELOPE_FINAL | LG_NET_ENVELOPE_ERROR |
      LG_NET_ENVELOPE_EARLY_DATA;
    if (packet.envelope.headerSize != LG_NET_ENVELOPE_WIRE_SIZE ||
        packet.envelope.payloadLength > NETWORK_MAX_CONTROL_PAYLOAD ||
        (clipboardPacket ?
          (packet.envelope.flags & DELIVERY_FLAGS) !=
            (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_FINAL) :
          packet.envelope.flags != expectedFlags) ||
        !lgNetEnvelopeRouteValid(&packet.envelope, LG_NET_ROLE_CLIENT,
          LG_NET_ROUTE_DELIVERY_RELIABLE))
      return false;

    HandshakeState handshake;
    uint64_t       sessionEpoch;
    uint32_t       maxPayload;
    {
      CSRWSharedLock guard(connection.lock);
      handshake    = connection.handshake;
      sessionEpoch = connection.sessionEpoch;
      maxPayload   = connection.maxPayload;
    }

    if (handshake == HandshakeState::WAITING_HELLO)
      return packet.envelope.service == LG_NET_SERVICE_CORE &&
        packet.envelope.messageType == LG_NET_CORE_MESSAGE_HELLO &&
        packet.envelope.serviceVersion == LG_NET_CORE_VERSION_CURRENT &&
        HandleHello(connection, packet);

    if (handshake == HandshakeState::WAITING_CAPABILITIES)
    {
      if (packet.envelope.service != LG_NET_SERVICE_CORE ||
          packet.envelope.serviceVersion != LG_NET_CORE_VERSION_CURRENT ||
          packet.envelope.payloadLength > maxPayload)
        return false;

      if (packet.envelope.messageType == LG_NET_CORE_MESSAGE_CAPABILITY)
        return HandleCapability(connection, packet);
      if (packet.envelope.messageType ==
          LG_NET_CORE_MESSAGE_CAPABILITIES_DONE)
        return FinishCapabilities(connection, packet);
      return false;
    }

    if (handshake == HandshakeState::WAITING_AUTH_RESPONSE)
    {
      return packet.envelope.service == LG_NET_SERVICE_CORE &&
        packet.envelope.messageType == LG_NET_CORE_MESSAGE_AUTH_RESPONSE &&
        packet.envelope.serviceVersion == LG_NET_CORE_VERSION_CURRENT &&
        packet.envelope.sessionEpoch == sessionEpoch &&
        packet.envelope.payloadLength <= maxPayload &&
        HandleAuthResponse(connection, packet);
    }

    if (handshake != HandshakeState::READY)
      return false;

    if (packet.envelope.sessionEpoch != sessionEpoch ||
        packet.envelope.payloadLength > maxPayload ||
        (packet.envelope.service == LG_NET_SERVICE_CORE &&
          packet.envelope.serviceVersion != LG_NET_CORE_VERSION_CURRENT))
      return false;

    if (packet.envelope.service == LG_NET_SERVICE_CORE)
    {
      if (packet.envelope.messageType == LG_NET_CORE_MESSAGE_PING)
        return SendPacket(connection, LG_NET_SERVICE_CORE,
          LG_NET_CORE_MESSAGE_PONG,
          LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
          0, packet.envelope.requestID, packet.payload,
          static_cast<uint32_t>(packet.payloadLength));

      if (packet.envelope.messageType == LG_NET_CORE_MESSAGE_GOODBYE &&
          packet.payloadLength == 0)
      {
        Shutdown(connection, 0);
        return true;
      }
      return false;
    }

    const uint16_t serviceMask = UINT16_C(1) << packet.envelope.service;
    {
      CSRWSharedLock guard(connection.lock);
      if (!(connection.selectedServices & serviceMask) ||
          !(connection.reliableServices & serviceMask) ||
          packet.envelope.serviceVersion !=
            connection.selectedCapabilities
              [packet.envelope.service].versionMin ||
          packet.envelope.payloadLength >
            connection.selectedCapabilities
              [packet.envelope.service].maxPayloadLength)
        return false;
    }

    switch (packet.envelope.service)
    {
      case LG_NET_SERVICE_RECOVERY:
        return HandleRecovery(connection, packet);

      case LG_NET_SERVICE_CURSOR:
        return HandleCursorStatus(connection, packet);

      case LG_NET_SERVICE_INPUT:
        return HandleInput(connection, packet, false);

      case LG_NET_SERVICE_AUDIO:
        return audio.HandlePacket(AudioPeer(connection), packet, false) !=
          NetworkAudioPacketResult::INVALID;

      case LG_NET_SERVICE_VIDEO:
        return video.HandlePacket(VideoPeer(connection), packet,
          NetworkVideoDelivery::RELIABLE) !=
            NetworkVideoPacketResult::INVALID;

      case LG_NET_SERVICE_CLIPBOARD:
      case LG_NET_SERVICE_FILE:
        return clipboard.HandlePacket(
          ClipboardPeer(connection), packet, false) !=
            NetworkClipboardPacketResult::INVALID;

      case LG_NET_SERVICE_CONTROL:
        return HandleControl(connection, packet);

      default:
        return false;
    }
  }

  bool HandleDatagram(Connection& connection, const QUIC_BUFFER * buffer)
  {
    LGNetPacketView packet;
    if (!buffer || !buffer->Buffer ||
        lgNetPacketDecode(&packet, buffer->Buffer,
          buffer->Length) != LG_NET_PARSE_OK ||
        packet.wireSize != buffer->Length ||
        packet.envelope.headerSize != LG_NET_ENVELOPE_WIRE_SIZE ||
        !lgNetEnvelopeRouteValid(&packet.envelope, LG_NET_ROLE_CLIENT,
          LG_NET_ROUTE_DELIVERY_DATAGRAM) ||
        (packet.envelope.flags &
          (LG_NET_ENVELOPE_DATAGRAM | LG_NET_ENVELOPE_FINAL)) !=
            (LG_NET_ENVELOPE_DATAGRAM | LG_NET_ENVELOPE_FINAL) ||
        packet.envelope.flags & (LG_NET_ENVELOPE_RELIABLE |
          LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_ERROR |
          LG_NET_ENVELOPE_EARLY_DATA) ||
        !packet.envelope.sequence)
      return false;

    const uint16_t serviceMask = UINT16_C(1) << packet.envelope.service;
    {
      CSRWExclusiveLock guard(connection.lock);
      const bool     serviceReady   = packet.envelope.service ==
        LG_NET_SERVICE_CORE ||
        ((connection.selectedServices & serviceMask) &&
          (connection.datagramServices & serviceMask));
      const uint32_t maxPayload     = packet.envelope.service ==
        LG_NET_SERVICE_CORE ? connection.maxDatagramPayload :
        connection.selectedCapabilities
          [packet.envelope.service].maxDatagramPayload;
      const uint16_t serviceVersion = packet.envelope.service ==
        LG_NET_SERVICE_CORE ? LG_NET_CORE_VERSION_CURRENT :
        connection.selectedCapabilities
          [packet.envelope.service].versionMin;
      if (connection.handshake != HandshakeState::READY ||
          packet.envelope.sessionEpoch != connection.sessionEpoch ||
          !serviceReady || !maxPayload ||
          packet.envelope.serviceVersion != serviceVersion ||
          packet.envelope.payloadLength > maxPayload)
        return false;

      const LGNetSequenceResult sequenceResult = lgNetSequenceTrackerCheck(
        &connection.datagramReceive, packet.envelope.sessionEpoch,
        NETWORK_SEQUENCE_COMPONENT, packet.envelope.sequence);
      if (sequenceResult == LG_NET_SEQUENCE_DUPLICATE ||
          sequenceResult == LG_NET_SEQUENCE_TOO_OLD)
        return true;
      if (!lgNetSequenceAccepted(sequenceResult))
        return false;
    }

    if (packet.envelope.service == LG_NET_SERVICE_CORE)
      return packet.envelope.messageType == LG_NET_CORE_MESSAGE_PING &&
        SendDatagram(connection, LG_NET_SERVICE_CORE,
          LG_NET_CORE_MESSAGE_PONG, LG_NET_ENVELOPE_RESPONSE,
          0, packet.envelope.requestID, packet.payload,
          static_cast<uint32_t>(packet.payloadLength), true);

    if (packet.envelope.service == LG_NET_SERVICE_INPUT)
      return HandleInput(connection, packet, true);
    if (packet.envelope.service == LG_NET_SERVICE_AUDIO)
      return audio.HandlePacket(AudioPeer(connection), packet, true) !=
        NetworkAudioPacketResult::INVALID;
    if (packet.envelope.service == LG_NET_SERVICE_VIDEO)
      return video.HandlePacket(VideoPeer(connection), packet,
        NetworkVideoDelivery::DATAGRAM) !=
          NetworkVideoPacketResult::INVALID;
    return false;
  }

  bool DrainReceive(Connection& connection)
  {
    for (;;)
    {
      LGNetStreamPacket       streamPacket;
      const LGNetStreamResult result = lgNetStreamParserNext(
        &connection.receiveParser, &streamPacket, nullptr);
      if (result == LG_NET_STREAM_NEED_DATA)
        return true;
      if (result != LG_NET_STREAM_PACKET)
        return false;

      LGNetPacketView packet = {};
      packet.envelope        = streamPacket.envelope;
      packet.payloadLength   = streamPacket.payloadLength;
      packet.wireSize        = streamPacket.wireSize;
      if (!streamPacket.payload[1].size)
        packet.payload = streamPacket.payload[0].data;
      else
      {
        try
        {
          connection.receivePayload.resize(streamPacket.payloadLength);
        }
        catch (...)
        {
          return false;
        }
        memcpy(connection.receivePayload.data(),
          streamPacket.payload[0].data, streamPacket.payload[0].size);
        memcpy(connection.receivePayload.data() +
            streamPacket.payload[0].size, streamPacket.payload[1].data,
          streamPacket.payload[1].size);
        packet.payload = connection.receivePayload.data();
      }

      {
        CSRWExclusiveLock guard(connection.lock);
        if (!packet.envelope.sequence ||
            connection.receiveSequence == UINT64_MAX ||
            packet.envelope.sequence != connection.receiveSequence + 1)
          return false;
        connection.receiveSequence = packet.envelope.sequence;
      }
      if (!HandlePacket(connection, packet) ||
          !lgNetStreamParserConsume(&connection.receiveParser))
        return false;
    }
  }

  bool Receive(Connection& connection, const QUIC_BUFFER * buffers,
    uint32_t count)
  {
    if (count && !buffers)
      return false;

    for (uint32_t i = 0; i < count; ++i)
    {
      if (buffers[i].Length && !buffers[i].Buffer)
        return false;

      size_t offset = 0;
      while (offset < buffers[i].Length)
      {
        size_t accepted;
        if (!lgNetStreamParserAppend(&connection.receiveParser,
              buffers[i].Buffer + offset, buffers[i].Length - offset,
              &accepted))
          return false;

        offset += accepted;
        if (!DrainReceive(connection) ||
            (!accepted &&
              !lgNetStreamParserWritable(&connection.receiveParser)))
          return false;
      }
    }
    return DrainReceive(connection);
  }

  bool ActivateBulkReceive(BulkStream& stream)
  {
    LGNetStreamBinding binding;
    if (lgNetStreamBindingDecode(&binding, stream.bindingData,
          sizeof(stream.bindingData)) != LG_NET_PARSE_OK ||
        binding.originRole != LG_NET_ROLE_CLIENT ||
        binding.flags != (LG_NET_STREAM_BINDING_UNIDIRECTIONAL |
          LG_NET_STREAM_BINDING_BULK) ||
        binding.streamID != stream.streamID ||
        binding.service <= LG_NET_SERVICE_CORE ||
        binding.service >= NETWORK_SERVICE_COUNT ||
        !BulkOriginAllowed(binding.service, LG_NET_ROLE_CLIENT))
      return false;

    Connection& connection = stream.connection;
    uint32_t    maxPayload;
    {
      CSRWSharedLock guard(connection.lock);
      const uint16_t serviceMask = UINT16_C(1) << binding.service;
      const bool     valid       = !connection.closing &&
        connection.handshake == HandshakeState::READY &&
        binding.sessionEpoch == connection.sessionEpoch &&
        (connection.selectedServices & serviceMask) &&
        (connection.bulkServices & serviceMask) &&
        connection.selectedCapabilities
          [binding.service].maxUnidirectionalStreams &&
        connection.servicePeerBulkStreams[binding.service] <
          connection.selectedCapabilities
            [binding.service].maxUnidirectionalStreams;
      maxPayload = valid ? connection.selectedCapabilities
        [binding.service].maxPayloadLength : 0;
    }
    if (!maxPayload)
      return false;

    try
    {
      stream.receiveData.resize(
        LG_NET_ENVELOPE_WIRE_SIZE + static_cast<size_t>(maxPayload));
    }
    catch (...)
    {
      return false;
    }
    if (!lgNetStreamParserInit(&stream.receiveParser,
          stream.receiveData.data(), stream.receiveData.size(), maxPayload))
      return false;

    {
      CSRWExclusiveLock guard(connection.lock);
      const uint16_t serviceMask = UINT16_C(1) << binding.service;
      const bool     valid       = !connection.closing &&
        connection.handshake == HandshakeState::READY &&
        binding.sessionEpoch == connection.sessionEpoch &&
        (connection.selectedServices & serviceMask) &&
        (connection.bulkServices & serviceMask) &&
        connection.selectedCapabilities
          [binding.service].maxPayloadLength == maxPayload &&
        connection.selectedCapabilities
          [binding.service].maxUnidirectionalStreams &&
        connection.servicePeerBulkStreams[binding.service] <
          connection.selectedCapabilities
            [binding.service].maxUnidirectionalStreams;
      if (!valid)
        return false;

      stream.service = binding.service;
      stream.bound   = true;
      ++connection.servicePeerBulkStreams[binding.service];
    }
    return true;
  }

  bool HandleBulkPacket(BulkStream& stream, const LGNetPacketView& packet)
  {
    if (packet.envelope.service <= LG_NET_SERVICE_CORE ||
        packet.envelope.service >= NETWORK_SERVICE_COUNT ||
        !BulkOriginAllowed(
          packet.envelope.service, LG_NET_ROLE_CLIENT))
      return false;

    Connection& connection = stream.connection;
    {
      CSRWSharedLock guard(connection.lock);
      const uint16_t serviceMask = UINT16_C(1) << packet.envelope.service;
      if (connection.closing ||
          connection.handshake != HandshakeState::READY ||
          !stream.bound || packet.envelope.service != stream.service ||
          packet.envelope.sessionEpoch != connection.sessionEpoch ||
          !(connection.selectedServices & serviceMask) ||
          !(connection.bulkServices & serviceMask) ||
          packet.envelope.serviceVersion !=
            connection.selectedCapabilities
              [packet.envelope.service].versionMin ||
          packet.envelope.payloadLength >
            connection.selectedCapabilities
              [packet.envelope.service].maxPayloadLength)
        return false;
    }

    if (packet.envelope.headerSize != LG_NET_ENVELOPE_WIRE_SIZE ||
        !(packet.envelope.flags & LG_NET_ENVELOPE_FINAL) ||
        packet.envelope.flags & (LG_NET_ENVELOPE_RELIABLE |
          LG_NET_ENVELOPE_DATAGRAM | LG_NET_ENVELOPE_EARLY_DATA) ||
        !lgNetEnvelopeRouteValid(&packet.envelope, LG_NET_ROLE_CLIENT,
          LG_NET_ROUTE_DELIVERY_BULK))
      return false;

    switch (packet.envelope.service)
    {
      case LG_NET_SERVICE_CLIPBOARD:
      case LG_NET_SERVICE_FILE:
        return clipboard.HandlePacket(
          ClipboardPeer(connection), packet, true) !=
            NetworkClipboardPacketResult::INVALID;

      default:
        return false;
    }
  }

  bool DrainBulkReceive(BulkStream& stream)
  {
    for (;;)
    {
      LGNetStreamPacket       streamPacket;
      const LGNetStreamResult result = lgNetStreamParserNext(
        &stream.receiveParser, &streamPacket, nullptr);
      if (result == LG_NET_STREAM_NEED_DATA)
        return true;
      if (result != LG_NET_STREAM_PACKET)
        return false;

      LGNetPacketView packet = {};
      packet.envelope        = streamPacket.envelope;
      packet.payloadLength   = streamPacket.payloadLength;
      packet.wireSize        = streamPacket.wireSize;
      if (!streamPacket.payload[1].size)
        packet.payload = streamPacket.payload[0].data;
      else
      {
        try
        {
          stream.receivePayload.resize(streamPacket.payloadLength);
        }
        catch (...)
        {
          return false;
        }
        memcpy(stream.receivePayload.data(), streamPacket.payload[0].data,
          streamPacket.payload[0].size);
        memcpy(stream.receivePayload.data() + streamPacket.payload[0].size,
          streamPacket.payload[1].data, streamPacket.payload[1].size);
        packet.payload = stream.receivePayload.data();
      }

      {
        CSRWExclusiveLock guard(stream.connection.lock);
        if (!stream.bound || !packet.envelope.sequence ||
            stream.receiveSequence == UINT64_MAX ||
            packet.envelope.sequence != stream.receiveSequence + 1)
          return false;
        stream.receiveSequence = packet.envelope.sequence;
      }
      if (!HandleBulkPacket(stream, packet) ||
          !lgNetStreamParserConsume(&stream.receiveParser))
        return false;

      if (!lgNetStreamParserBuffered(&stream.receiveParser))
        lgNetStreamParserReset(&stream.receiveParser);
    }
  }

  bool ReceiveBulk(BulkStream& stream, const QUIC_BUFFER * buffers,
    uint32_t count)
  {
    if (count && !buffers)
      return false;

    for (uint32_t index = 0; index < count; ++index)
    {
      if (buffers[index].Length && !buffers[index].Buffer)
        return false;

      size_t offset = 0;
      while (offset < buffers[index].Length)
      {
        if (!stream.bound)
        {
          const size_t remaining = buffers[index].Length - offset;
          size_t       length    = sizeof(stream.bindingData) -
            stream.bindingLength;
          if (length > remaining)
            length = remaining;
          memcpy(stream.bindingData + stream.bindingLength,
            buffers[index].Buffer + offset, length);
          stream.bindingLength += length;
          offset               += length;
          if (stream.bindingLength == sizeof(stream.bindingData) &&
              !ActivateBulkReceive(stream))
            return false;
          continue;
        }

        size_t accepted;
        if (!lgNetStreamParserAppend(&stream.receiveParser,
              buffers[index].Buffer + offset,
              buffers[index].Length - offset, &accepted))
          return false;
        offset += accepted;
        if (!DrainBulkReceive(stream) ||
            (!accepted &&
              !lgNetStreamParserWritable(&stream.receiveParser)))
          return false;
      }
    }

    return !stream.bound || DrainBulkReceive(stream);
  }

  bool AcceptBulkStream(Connection& connection, HQUIC handle,
    QUIC_STREAM_OPEN_FLAGS flags)
  {
    if (flags != QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL)
      return false;

    QUIC_UINT62 streamID;
    uint32_t    streamIDSize = sizeof(streamID);
    if (QUIC_FAILED(api->GetParam(handle, QUIC_PARAM_STREAM_ID,
          &streamIDSize, &streamID)) ||
        streamIDSize != sizeof(streamID) || (streamID & 3U) != 2U)
      return false;

    std::unique_ptr<BulkStream> stream(
      new (std::nothrow) BulkStream(connection));
    if (!stream)
      return false;
    stream->handle   = handle;
    stream->streamID = streamID;

    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.closing ||
          connection.handshake != HandshakeState::READY ||
          !connection.maxPeerBulkStreams ||
          connection.peerBulkStreams >= connection.maxPeerBulkStreams)
        return false;

      stream->next             = connection.inboundBulk;
      connection.inboundBulk   = stream.get();
      ++connection.peerBulkStreams;
    }

    api->SetCallbackHandler(handle,
      reinterpret_cast<void *>(BulkStreamCallback), stream.get());
    stream.release();
    return true;
  }

  void ConnectionClosed(Connection& connection)
  {
    const uint32_t client     = connection.client;
    const uint32_t generation = connection.generation;
    std::shared_ptr<CNetworkSession> session;
    {
      CSRWExclusiveLock guard(lock);
      const auto found = std::find(
        connections.begin(), connections.end(), &connection);
      if (found != connections.end())
        connections.erase(found);
    }

    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.reliableSend.count || connection.reliableSend.bytes ||
          connection.datagramSend.count || connection.datagramSend.bytes ||
          connection.bulkSend.count || connection.bulkSend.bytes)
        DEBUG_ERROR("Network client %u closed with sends in flight",
          connection.client);
      if (connection.inboundBulk)
        DEBUG_ERROR("Network client %u closed with inbound bulk streams",
          connection.client);
      for (LGNetService service = LG_NET_SERVICE_CORE;
           service < NETWORK_SERVICE_COUNT; ++service)
        if (connection.outboundBulk[service])
          DEBUG_ERROR("Network client %u closed with a bulk stream for "
            "service %u", connection.client,
            static_cast<unsigned>(service));
      session = std::move(connection.session);
    }
    video.PeerDisconnected(VideoPeer(connection));
    clipboard.PeerDisconnected(ClipboardPeer(connection));
    audio.PeerDisconnected(AudioPeer(connection));
    if (session)
      transport.RemoveSession(client, generation);
    session.reset();
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
      try
      {
        connection->receiveData.resize(NETWORK_MAX_CONTROL_BUFFER);
      }
      catch (...)
      {
        delete connection;
        return QUIC_STATUS_OUT_OF_MEMORY;
      }
      if (!lgNetStreamParserInit(&connection->receiveParser,
            connection->receiveData.data(), connection->receiveData.size(),
            static_cast<uint32_t>(NETWORK_MAX_CONTROL_PAYLOAD)))
      {
        delete connection;
        return QUIC_STATUS_OUT_OF_MEMORY;
      }
      connection->handle            = event->NEW_CONNECTION.Connection;
      connection->client            = owner.AllocateClientLocked();
      connection->generation        = 1;
      connection->handshakeDeadline =
        GetTickCount64() + NETWORK_HANDSHAKE_TIMEOUT_MS;
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
        const QUIC_STREAM_OPEN_FLAGS flags =
          event->PEER_STREAM_STARTED.Flags;
        if (flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL)
        {
          if (!owner.AcceptBulkStream(connection,
                event->PEER_STREAM_STARTED.Stream, flags))
          {
            owner.api->SetCallbackHandler(
              event->PEER_STREAM_STARTED.Stream,
              reinterpret_cast<void *>(RejectedStreamCallback), &owner);
            owner.api->StreamShutdown(event->PEER_STREAM_STARTED.Stream,
              QUIC_STREAM_SHUTDOWN_FLAG_ABORT |
                QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE,
              NETWORK_ERROR_PROTOCOL);
            owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
          }
          return QUIC_STATUS_SUCCESS;
        }

        bool accepted = false;
        {
          CSRWExclusiveLock guard(connection.lock);
          if (flags == QUIC_STREAM_OPEN_FLAG_NONE &&
              !connection.closing &&
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
        if (!owner.HandleDatagram(
              connection, event->DATAGRAM_RECEIVED.Buffer))
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
        return QUIC_STATUS_SUCCESS;

      case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
      {
        {
          CSRWExclusiveLock guard(connection.lock);
          connection.datagramSendEnabled            =
            event->DATAGRAM_STATE_CHANGED.SendEnabled != FALSE;
          connection.maxDatagramSendLength          =
            event->DATAGRAM_STATE_CHANGED.MaxSendLength;
          connection.datagramSend.writableServices |=
            ReadyServicesLocked(connection, Delivery::DATAGRAM);
        }
        owner.NotifyAudioSendReady(connection, true);
        owner.NotifyVideoSendReady(connection, Delivery::DATAGRAM);
        return QUIC_STATUS_SUCCESS;
      }

      case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
        if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(
              event->DATAGRAM_SEND_STATE_CHANGED.State))
        {
          std::unique_ptr<Send> send(static_cast<Send *>(
            event->DATAGRAM_SEND_STATE_CHANGED.ClientContext));
          {
            CSRWExclusiveLock guard(connection.lock);
            ReleaseSendLocked(connection, *send);
          }
          owner.NotifyAudioSendReady(connection, true);
          owner.NotifyVideoSendReady(connection, Delivery::DATAGRAM);
        }
        return QUIC_STATUS_SUCCESS;

      case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
      {
        DEBUG_WARN("Network client %u transport shutdown: status 0x%lx, "
          "error 0x%llx", connection.client,
          static_cast<unsigned long>(
            event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status),
          static_cast<unsigned long long>(
            event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode));
        CSRWExclusiveLock guard(connection.lock);
        connection.closing = true;
        return QUIC_STATUS_SUCCESS;
      }

      case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
      {
        if (event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode)
          DEBUG_WARN("Network client %u closed the connection: error 0x%llx",
            connection.client, static_cast<unsigned long long>(
              event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode));
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
      {
        std::unique_ptr<Send> send(
          static_cast<Send *>(event->SEND_COMPLETE.ClientContext));
        {
          CSRWExclusiveLock guard(connection.lock);
          ReleaseSendLocked(connection, *send);
        }
        owner.NotifyAudioSendReady(connection, false);
        owner.NotifyVideoSendReady(connection, Delivery::RELIABLE);
        owner.NotifyClipboardSendReady(connection, Delivery::RELIABLE);
        return QUIC_STATUS_SUCCESS;
      }

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

  static QUIC_STATUS QUIC_API BulkStreamCallback(HQUIC handle,
    void * context, QUIC_STREAM_EVENT * event)
  {
    BulkStream& stream     = *static_cast<BulkStream *>(context);
    Connection& connection = stream.connection;
    Impl&       owner      = connection.owner;
    CallbackGuard callback(owner);

    switch (event->Type)
    {
      case QUIC_STREAM_EVENT_START_COMPLETE:
      {
        if (QUIC_FAILED(event->START_COMPLETE.Status))
        {
          bool deferFree;
          {
            CSRWExclusiveLock guard(connection.lock);
            DetachBulkStreamLocked(stream);
            deferFree               = stream.outbound &&
              !stream.openingReturned;
            stream.shutdownComplete = true;
          }
          owner.api->StreamClose(handle);
          if (!deferFree)
            delete &stream;
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
          return QUIC_STATUS_SUCCESS;
        }
        if (!stream.outbound || !event->START_COMPLETE.ID ||
            (event->START_COMPLETE.ID & 3U) != 3U)
        {
          owner.api->StreamShutdown(handle,
            QUIC_STREAM_SHUTDOWN_FLAG_ABORT |
              QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE,
            NETWORK_ERROR_PROTOCOL);
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
          return QUIC_STATUS_SUCCESS;
        }

        uint64_t sessionEpoch;
        {
          CSRWSharedLock guard(connection.lock);
          const uint16_t serviceMask = UINT16_C(1) << stream.service;
          if (connection.closing ||
              connection.handshake != HandshakeState::READY ||
              connection.outboundBulk[stream.service] != &stream ||
              !(connection.selectedServices & serviceMask) ||
              !(connection.bulkServices & serviceMask))
          {
            owner.api->StreamShutdown(handle,
              QUIC_STREAM_SHUTDOWN_FLAG_ABORT |
                QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE,
              NETWORK_ERROR_PROTOCOL);
            return QUIC_STATUS_SUCCESS;
          }
          sessionEpoch = connection.sessionEpoch;
        }

        LGNetStreamBinding binding;
        lgNetStreamBindingInit(&binding, stream.service, LG_NET_ROLE_SERVER,
          event->START_COMPLETE.ID, sessionEpoch,
          LG_NET_STREAM_BINDING_UNIDIRECTIONAL |
            LG_NET_STREAM_BINDING_BULK);
        std::unique_ptr<Send> send(new (std::nothrow) Send());
        if (!send)
        {
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
          return QUIC_STATUS_SUCCESS;
        }
        try
        {
          send->data.resize(LG_NET_STREAM_BINDING_WIRE_SIZE);
        }
        catch (...)
        {
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
          return QUIC_STATUS_SUCCESS;
        }
        if (!lgNetStreamBindingEncode(send->data.data(), send->data.size(),
              &binding))
        {
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
          return QUIC_STATUS_SUCCESS;
        }

        send->buffer.Buffer = send->data.data();
        send->buffer.Length = LG_NET_STREAM_BINDING_WIRE_SIZE;
        send->service       = stream.service;
        send->length        = LG_NET_STREAM_BINDING_WIRE_SIZE;
        send->delivery      = Delivery::BULK;
        send->accounted     = false;

        uint16_t    ready;
        QUIC_STATUS status;
        {
          CSRWExclusiveLock guard(connection.lock);
          const uint16_t serviceMask = UINT16_C(1) << stream.service;
          const bool     valid       = !connection.closing &&
            connection.handshake == HandshakeState::READY &&
            connection.sessionEpoch == sessionEpoch &&
            connection.outboundBulk[stream.service] == &stream &&
            (connection.selectedServices & serviceMask) &&
            (connection.bulkServices & serviceMask);
          status = valid ? owner.api->StreamSend(handle, &send->buffer, 1,
            QUIC_SEND_FLAG_NONE, send.get()) : QUIC_STATUS_ABORTED;
          ready  = 0;
          if (QUIC_SUCCEEDED(status))
          {
            stream.streamID = event->START_COMPLETE.ID;
            stream.bound    = true;
            if (stream.openingReturned)
            {
              ready = ReadyServicesLocked(connection, Delivery::BULK);
              connection.bulkSend.writableServices |= ready;
            }
          }
        }
        if (QUIC_FAILED(status))
        {
          owner.api->StreamShutdown(handle,
            QUIC_STREAM_SHUTDOWN_FLAG_ABORT |
              QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE,
            NETWORK_ERROR_PROTOCOL);
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
          return QUIC_STATUS_SUCCESS;
        }

        send.release();
        if (ready)
        {
          owner.NotifyVideoSendReady(connection, Delivery::BULK);
          owner.NotifyClipboardSendReady(connection, Delivery::BULK);
        }
        return QUIC_STATUS_SUCCESS;
      }

      case QUIC_STREAM_EVENT_RECEIVE:
        if (stream.outbound || !owner.ReceiveBulk(stream,
              event->RECEIVE.Buffers, event->RECEIVE.BufferCount))
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
        return QUIC_STATUS_SUCCESS;

      case QUIC_STREAM_EVENT_SEND_COMPLETE:
      {
        std::unique_ptr<Send> send(
          static_cast<Send *>(event->SEND_COMPLETE.ClientContext));
        {
          CSRWExclusiveLock guard(connection.lock);
          ReleaseSendLocked(connection, *send);
        }
        owner.NotifyVideoSendReady(connection, Delivery::BULK);
        owner.NotifyClipboardSendReady(connection, Delivery::BULK);
        return QUIC_STATUS_SUCCESS;
      }

      case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
      case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
      case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
      {
        bool closing;
        {
          CSRWSharedLock guard(connection.lock);
          closing = connection.closing;
        }
        if (!closing)
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
        return QUIC_STATUS_SUCCESS;
      }

      case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
      {
        bool deferFree;
        {
          CSRWExclusiveLock guard(connection.lock);
          DetachBulkStreamLocked(stream);
          deferFree               = stream.outbound &&
            !stream.openingReturned;
          stream.shutdownComplete = true;
        }
        owner.api->StreamClose(handle);
        if (!deferFree)
          delete &stream;
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

  void Process()
  {
    const uint64_t now = GetTickCount64();
    audio.Process(now);
    video.Process(now);
    clipboard.Process(now);
    CSRWSharedLock guard(lock);
    if (stopping || !api)
      return;

    for (Connection * connection : connections)
    {
      HQUIC    handle = nullptr;
      uint32_t client = 0;
      {
        CSRWExclusiveLock connectionGuard(connection->lock);
        if (!connection->closing && connection->handshakeDeadline &&
            now >= connection->handshakeDeadline)
        {
          connection->closing = true;
          handle              = connection->handle;
          client              = connection->client;
        }
      }

      if (!handle)
      {
        if (!PumpSession(*connection, now))
          Shutdown(*connection, NETWORK_ERROR_PROTOCOL);
        continue;
      }

      DEBUG_WARN("Closing network client %u after an incomplete handshake",
        client);
      api->ConnectionShutdown(handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
        NETWORK_ERROR_HANDSHAKE_TIMEOUT);
    }
  }

  bool Start()
  {
    {
      CSRWExclusiveLock guard(lock);
      if (running)
        return true;
      stopping = false;
    }

    if ((settings.auth != LG_NET_AUTH_NONE &&
          settings.auth != LG_NET_AUTH_PASSWORD) ||
        (settings.auth == LG_NET_AUTH_PASSWORD &&
          settings.password.empty()) ||
        (settings.auth == LG_NET_AUTH_NONE && !settings.password.empty()))
    {
      DEBUG_ERROR("The network authentication settings are inconsistent");
      Stop();
      return false;
    }

    if (settings.auth == LG_NET_AUTH_PASSWORD)
    {
      std::vector<uint8_t> password;

      const bool valid = PasswordUTF8(settings.password, password);
      NetworkAuthCrypto::SecureZero(password.data(), password.size());
      if (!valid)
      {
        DEBUG_ERROR("The configured network password is not valid UTF-16");
        Stop();
        return false;
      }
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

    if (!LoadCertificateContext(certificateStore, certificate))
    {
      DEBUG_ERROR("The persistent network identity is unavailable; run the "
        "Looking Glass IDD installer to provision it");
      Stop();
      return false;
    }

    QUIC_CREDENTIAL_CONFIG credential = {};
    credential.Type               = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
    credential.Flags              = QUIC_CREDENTIAL_FLAG_NONE;
    credential.CertificateContext = reinterpret_cast<QUIC_CERTIFICATE *>(
      const_cast<CERT_CONTEXT *>(certificate));
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
    if (certificate)
    {
      CertFreeCertificateContext(certificate);
      certificate = nullptr;
    }
    if (certificateStore)
    {
      CertCloseStore(certificateStore, 0);
      certificateStore = nullptr;
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
  CNetworkAudio& audio, CNetworkClipboard& clipboard,
  CNetworkVideo& video, const NetworkSettings& settings) :
  m_impl(new Impl(transport, audio, clipboard, video, settings))
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

void CNetworkServer::Process()
{
  if (m_impl)
    m_impl->Process();
}

bool CNetworkServer::AudioPeerInfo(const NetworkAudioPeer& peer,
  NetworkAudioPeerInfo& info)
{
  return m_impl && m_impl->AudioPeerInfo(peer, info);
}

NetworkAudioSendResult CNetworkServer::SendAudio(
  const NetworkAudioPeer& peer, bool datagram, LGNetService service,
  uint16_t messageType, LGNetEnvelopeFlags flags,
  uint64_t componentEpoch, uint64_t requestID,
  const void * payload, uint32_t payloadLength)
{
  return m_impl ?
    m_impl->SendAudio(peer, datagram, service, messageType, flags,
      componentEpoch, requestID, payload, payloadLength) :
    NetworkAudioSendResult::FAILED;
}

bool CNetworkServer::ClipboardPeerInfo(
  const NetworkClipboardPeer& peer, NetworkClipboardPeerInfo& info)
{
  return m_impl && m_impl->ClipboardPeerInfo(peer, info);
}

NetworkClipboardSendResult CNetworkServer::SendClipboard(
  const NetworkClipboardPeer& peer, bool bulk, LGNetService service,
  uint16_t messageType, LGNetEnvelopeFlags flags,
  uint64_t componentEpoch, uint64_t requestID,
  const void * payload, uint32_t payloadLength)
{
  return m_impl ?
    m_impl->SendClipboard(peer, bulk, service, messageType, flags,
      componentEpoch, requestID, payload, payloadLength) :
    NetworkClipboardSendResult::FAILED;
}

bool CNetworkServer::VideoPeerInfo(
  const NetworkVideoPeer& peer, NetworkVideoPeerInfo& info)
{
  return m_impl && m_impl->VideoPeerInfo(peer, info);
}

NetworkVideoSendResult CNetworkServer::SendVideo(
  const NetworkVideoPeer& peer, NetworkVideoDelivery delivery,
  uint16_t messageType, LGNetEnvelopeFlags flags,
  uint64_t componentEpoch, uint64_t requestID,
  const void * payload, uint32_t payloadLength)
{
  return m_impl ?
    m_impl->SendVideo(peer, delivery, messageType, flags,
      componentEpoch, requestID, payload, payloadLength) :
    NetworkVideoSendResult::FAILED;
}
