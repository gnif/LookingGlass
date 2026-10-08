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
#include "InputPipeProtocol.h"
#include "platform/CPlatformInfo.h"
#include "VersionInfo.h"
#include <LGProtocol/NetworkAuth.h>
#include <LGProtocol/NetworkClipboard.h>
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
  static constexpr uint32_t NETWORK_STREAM_RECV_BUFFER       =
    1U * 1024U * 1024U;
  static constexpr uint32_t NETWORK_STREAM_RECV_WINDOW       =
    128U * 1024U * 1024U;
  static constexpr uint32_t NETWORK_CONN_RECV_WINDOW         =
    256U * 1024U * 1024U;
  static constexpr uint32_t NETWORK_MAX_CONTROL_PAYLOAD      =
    LG_NET_CURSOR_SHAPE_HEADER_WIRE_SIZE +
      LG_NET_CURSOR_MAX_SHAPE_LENGTH;
  static constexpr size_t   NETWORK_MAX_CONTROL_BUFFER       =
    LG_NET_ENVELOPE_WIRE_SIZE + NETWORK_MAX_CONTROL_PAYLOAD;
  static constexpr uint32_t NETWORK_MAX_NEGOTIATED_PAYLOAD   =
    LG_NET_MAX_PAYLOAD_LENGTH;
  static constexpr uint32_t NETWORK_MAX_DATAGRAM_PAYLOAD     = 1136;
  static constexpr uint32_t NETWORK_INPUT_MAX_PAYLOAD        =
    LG_NET_INPUT_RELATIVE_WIRE_SIZE > LG_NET_INPUT_ABSOLUTE_WIRE_SIZE ?
      LG_NET_INPUT_RELATIVE_WIRE_SIZE : LG_NET_INPUT_ABSOLUTE_WIRE_SIZE;
  static constexpr uint32_t NETWORK_INPUT_LEASE_MS           = 500;
  static constexpr uint32_t NETWORK_INPUT_RECOVERY_LIMIT     = 4;
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
  static constexpr uint32_t NETWORK_VIDEO_BULK_COUNT_MAX         = 3;
  static constexpr uint64_t NETWORK_VIDEO_BULK_BYTES_MAX         =
    static_cast<uint64_t>(NETWORK_VIDEO_BULK_COUNT_MAX) *
      (LG_NET_ENVELOPE_WIRE_SIZE + LG_NET_MAX_PAYLOAD_LENGTH);
  static constexpr uint32_t NETWORK_SEND_RESUME_NUMERATOR        = 3;
  static constexpr uint32_t NETWORK_SEND_RESUME_DENOMINATOR      = 4;
  static constexpr uint32_t NETWORK_INPUT_SEND_COUNT_MAX         = 128;
  static constexpr uint64_t NETWORK_INPUT_SEND_BYTES_MAX         =
    UINT64_C(1) * 1024U * 1024U;

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

  size_t UTF8PrefixLength(const uint8_t * value, size_t length,
    size_t maximum)
  {
    size_t prefix = (std::min)(length, maximum);
    while (prefix && prefix < length &&
        (value[prefix] & UINT8_C(0xC0)) == UINT8_C(0x80))
      --prefix;

    return prefix;
  }

  size_t ConsumeUTF8Field(const uint8_t * value, size_t length,
    size_t fieldMaximum, size_t& remaining)
  {
    const size_t maximum = (std::min)(fieldMaximum, remaining);
    const size_t result  = UTF8PrefixLength(value, length, maximum);
    remaining -= result;
    return result;
  }

  uint8_t TopologyCount(int count)
  {
    if (count <= 0)
      return 0;

    return static_cast<uint8_t>((std::min)(count,
      static_cast<int>(UINT8_MAX)));
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
    using SharedData = std::shared_ptr<const std::vector<uint8_t>>;

    QUIC_BUFFER          buffers [2]                         = {};
    uint8_t              envelope[LG_NET_ENVELOPE_WIRE_SIZE] = {};
    std::vector<uint8_t> data;
    SharedData           sharedData;

    uint32_t             bufferCount = 1;
    LGNetService         service     = LG_NET_SERVICE_CORE;
    uint32_t             length      = 0;
    Delivery             delivery    = Delivery::RELIABLE;
    bool                 accounted   = true;
  };

  struct SendLimits
  {
    uint32_t count;
    uint64_t bytes;
  };

  struct InputPointerMotion
  {
    uint64_t            claimEpoch      = 0;
    uint64_t            sequence        = 0;
    int64_t             previousX       = 0;
    int64_t             previousY       = 0;
    int64_t             cumulativeX     = 0;
    int64_t             cumulativeY     = 0;
    uint16_t            absoluteX       = 0;
    uint16_t            absoluteY       = 0;
    LGNetPointerButtons previousButtons = 0;
    bool                apply           = false;
    bool                useAbsolute     = false;
  };

  struct InputSend
  {
    static constexpr size_t CAPACITY =
      LG_NET_ENVELOPE_WIRE_SIZE + NETWORK_INPUT_MAX_PAYLOAD;

    QUIC_BUFFER buffer         = {};
    uint8_t     data[CAPACITY] = {};
    uint32_t    length         = 0;
    bool        active         = false;
    bool        inputAck       = false;
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

  enum class InputPrepareResult
  {
    READY,
    STALE,
    INVALID,
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
    CSRWLock                         inputLock;
    CSRWLock                         inputSendLock;
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
    uint32_t                         pins                         = 0;
    bool                             detached                     = false;
    bool                             cleanupComplete              = false;
    HandshakeState                   handshake                    =
      HandshakeState::WAITING_HELLO;

    HQUIC                            inputStream                  = nullptr;
    uint64_t                         inputStreamID                = 0;
    uint64_t                         inputStreamSessionEpoch      = 0;
    uint64_t                         inputStreamSendSequence      = 0;
    uint64_t                         inputStreamReceiveSequence   = 0;
    uint32_t                         inputStreamMaxPayload        = 0;
    uint32_t                         inputDatagramLimit           = 0;
    uint32_t                         inputStreamSendCount         = 0;
    uint64_t                         inputStreamSendBytes         = 0;
    uint32_t                         inputSendNext                = 0;
    uint16_t                         inputStreamServiceVersion    = 0;
    bool                             inputStreamBound             = false;
    bool                             inputClosing                 = false;

    uint8_t                          inputBindingData[LG_NET_STREAM_BINDING_WIRE_SIZE] = {};
    size_t                           inputBindingLength                                = 0;
    LGNetStreamParser                inputReceiveParser                                = {};

    std::vector<uint8_t>             inputReceiveData;
    std::vector<uint8_t>             inputReceivePayload;
    std::shared_ptr<CNetworkSession> inputSession;
    InputSend                        inputSends[NETWORK_INPUT_SEND_COUNT_MAX] = {};

    LGNetSequenceTracker             inputDatagramReceive         = {};
    uint64_t                         inputClaimantID              = 0;
    uint64_t                         inputClaimEpoch              = 0;
    uint64_t                         inputReliableSequence        = 0;
    uint64_t                         inputDatagramSequence        = 0;
    uint64_t                         inputMotionSequence          = 0;
    int64_t                          inputCumulativeX             = 0;
    int64_t                          inputCumulativeY             = 0;
    uint16_t                         inputAbsoluteX               = 0;
    uint16_t                         inputAbsoluteY               = 0;
    uint64_t                         inputLEDSequence             = 0;
    uint64_t                         inputLEDCheckGeneration      = 0;
    uint64_t                         inputAckClaimEpoch           = 0;
    uint64_t                         inputAckRequestID            = 0;
    LGNetInputClaimFlags             inputFlags                   = 0;
    LGNetInputStatusCode             inputAckStatus               =
      LG_NET_INPUT_STATUS_ERROR;
    LGNetPointerButtons              inputPointerButtons          = 0;
    LGNetKeyboardLEDs                inputLEDState                = 0;
    uint8_t                          keyboardModifiers            = 0;
    bool                             inputClaimPending            = false;
    bool                             inputLEDCheckPending         = false;
    bool                             inputLEDWaitForSendReady     = false;
    bool                             inputMotionAbsolute          = false;
    bool                             inputMotionBaseline          = false;
    bool                             inputLEDStateKnown           = false;
    bool                             inputAckPending              = false;
    bool                             inputAckSending              = false;
    bool                             inputAckInFlight             = false;
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
  unsigned                  activePins         = 0;
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
      drained = stopping && connections.empty() && !activeCallbacks &&
        !activePins;
    }
    if (drained && drainedEvent)
      SetEvent(drainedEvent);
  }

  void ReleaseConnectionPin(Connection& connection)
  {
    Connection * dispose = nullptr;
    bool         drained = false;
    {
      CSRWExclusiveLock guard(lock);
      if (!connection.pins || !activePins)
      {
        DEBUG_ERROR("Network client %u connection pin underflow",
          connection.client);
        return;
      }
      --connection.pins;
      --activePins;
      if (!connection.pins && connection.detached &&
          connection.cleanupComplete)
        dispose = &connection;
      drained = stopping && connections.empty() && !activeCallbacks &&
        !activePins;
    }
    delete dispose;
    if (drained && drainedEvent)
      SetEvent(drainedEvent);
  }

  struct ConnectionPin
  {
    ConnectionPin() = default;

    ConnectionPin(Impl& owner, Connection& connection) :
      owner(&owner), connection(&connection)
    {
    }

    ConnectionPin(const ConnectionPin&) = delete;
    ConnectionPin& operator=(const ConnectionPin&) = delete;

    ConnectionPin(ConnectionPin&& other) noexcept :
      owner(other.owner), connection(other.connection)
    {
      other.owner      = nullptr;
      other.connection = nullptr;
    }

    ConnectionPin& operator=(ConnectionPin&& other) noexcept
    {
      if (this == &other)
        return *this;
      Reset();
      owner            = other.owner;
      connection       = other.connection;
      other.owner      = nullptr;
      other.connection = nullptr;
      return *this;
    }

    ~ConnectionPin()
    {
      Reset();
    }

    explicit operator bool() const
    {
      return connection != nullptr;
    }

    Connection * operator->() const
    {
      return connection;
    }

    Connection& operator*() const
    {
      return *connection;
    }

    void Reset()
    {
      if (owner && connection)
        owner->ReleaseConnectionPin(*connection);
      owner      = nullptr;
      connection = nullptr;
    }

    Impl       * owner      = nullptr;
    Connection * connection = nullptr;
  };

  template<typename Peer>
  ConnectionPin PinConnection(const Peer& peer)
  {
    CSRWExclusiveLock guard(lock);
    if (stopping || !api)
      return {};
    for (Connection * connection : connections)
    {
      if (connection->client != peer.client ||
          connection->generation != peer.generation ||
          connection->detached)
        continue;

      ++connection->pins;
      ++activePins;
      return ConnectionPin(*this, *connection);
    }
    return {};
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
    ConnectionPin connection = PinConnection(peer);
    if (!connection)
      return false;

    CSRWSharedLock connectionGuard(connection->lock);
    const uint16_t serviceMask = UINT16_C(1) << LG_NET_SERVICE_AUDIO;
    if (connection->closing ||
        !PeerInfoAvailableLocked(*connection) ||
        !(connection->selectedServices & serviceMask))
      return false;

    const LGNetCapability& capability =
      connection->selectedCapabilities[LG_NET_SERVICE_AUDIO];
    info.serviceVersion     = capability.versionMin;
    info.capabilityFlags    = capability.flags;
    info.maxReliablePayload = capability.maxPayloadLength;
    info.maxDatagramPayload = capability.maxDatagramPayload;
    return true;
  }

  NetworkAudioSendResult SendAudio(
    const NetworkAudioPeer& peer, bool datagram, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
  {
    if (service != LG_NET_SERVICE_AUDIO)
      return NetworkAudioSendResult::FAILED;

    ConnectionPin connection = PinConnection(peer);
    if (!connection)
      return NetworkAudioSendResult::FAILED;

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
    return NetworkAudioSendResult::FAILED;
  }

  std::shared_ptr<CNetworkSession> ClipboardInputSession(
    const NetworkClipboardPeer& peer)
  {
    ConnectionPin connection = PinConnection(peer);
    if (!connection)
      return {};

    std::shared_ptr<CNetworkSession> session;
    {
      CSRWSharedLock connectionGuard(connection->lock);
      const uint16_t inputMask = UINT16_C(1) << LG_NET_SERVICE_INPUT;
      if (connection->closing ||
          connection->handshake != HandshakeState::READY ||
          !(connection->selectedServices & inputMask))
        return {};
      session = connection->session;
    }
    {
      CSRWSharedLock inputGuard(connection->inputLock);
      if (connection->inputClosing || !connection->inputStreamBound ||
          !connection->inputClaimantID || !connection->inputClaimEpoch ||
          connection->inputSession != session)
        return {};
    }

    return session;
  }

  bool ClipboardPeerOwnsInput(const NetworkClipboardPeer& peer)
  {
    const std::shared_ptr<CNetworkSession> session =
      ClipboardInputSession(peer);
    return session && session->InputState().owned;
  }

  bool ClipboardWithInputAuthority(const NetworkClipboardPeer& peer,
    NetworkClipboardInputAction action, void * opaque)
  {
    if (!action)
      return false;

    const std::shared_ptr<CNetworkSession> session =
      ClipboardInputSession(peer);
    if (!session)
      return false;

    struct AuthorityContext
    {
      Impl                      * owner   = nullptr;
      CNetworkSession           * session = nullptr;
      NetworkClipboardPeer        peer    = {};
      NetworkClipboardInputAction action  = nullptr;
      void                      * opaque  = nullptr;
      bool                        invoked = false;
    } context;
    context.owner   = this;
    context.session = session.get();
    context.peer    = peer;
    context.action  = action;
    context.opaque  = opaque;

    const auto invoke = [](void * value)
    {
      AuthorityContext& authorityContext =
        *static_cast<AuthorityContext *>(value);
      ConnectionPin connection = authorityContext.owner->PinConnection(
        authorityContext.peer);
      if (!connection)
        return;

      {
        CSRWSharedLock connectionGuard(connection->lock);
        const uint16_t inputMask = UINT16_C(1) << LG_NET_SERVICE_INPUT;
        if (connection->closing ||
            connection->handshake != HandshakeState::READY ||
            !(connection->selectedServices & inputMask) ||
            connection->session.get() != authorityContext.session)
          return;
      }
      {
        CSRWSharedLock inputGuard(connection->inputLock);
        if (connection->inputClosing || !connection->inputStreamBound ||
            !connection->inputClaimantID ||
            !connection->inputClaimEpoch ||
            connection->inputSession.get() != authorityContext.session)
          return;
      }

      authorityContext.action(authorityContext.opaque);
      authorityContext.invoked = true;
    };
    return session->WithInputAuthority(invoke, &context) && context.invoked;
  }

  bool ClipboardPeerInfo(const NetworkClipboardPeer& peer,
    NetworkClipboardPeerInfo& info)
  {
    ConnectionPin connection = PinConnection(peer);
    if (!connection)
      return false;

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
    const LGNetCapability& fileCapability      =
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

  NetworkClipboardSendResult SendClipboardToPeer(
    const NetworkClipboardPeer& peer, bool bulk, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
  {
    ConnectionPin connection = PinConnection(peer);
    if (!connection)
      return NetworkClipboardSendResult::FAILED;

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
    return NetworkClipboardSendResult::FAILED;
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

    const bool claimResult =
      service == LG_NET_SERVICE_CLIPBOARD &&
      messageType == LG_NET_CLIPBOARD_MESSAGE_CLAIM_RESULT;
    if (claimResult)
      return SendClipboardToPeer(peer, bulk, service, messageType, flags,
        componentEpoch, requestID, payload, payloadLength);

    struct SendContext
    {
      Impl                       * owner          = nullptr;
      NetworkClipboardPeer         peer           = {};
      bool                         bulk           = false;
      LGNetService                 service        = 0;
      uint16_t                     messageType    = 0;
      LGNetEnvelopeFlags           flags          = 0;
      uint64_t                     componentEpoch = 0;
      uint64_t                     requestID      = 0;
      const void                 * payload        = nullptr;
      uint32_t                     payloadLength  = 0;
      NetworkClipboardSendResult   result         =
        NetworkClipboardSendResult::FAILED;
    } context;
    context.owner          = this;
    context.peer           = peer;
    context.bulk           = bulk;
    context.service        = service;
    context.messageType    = messageType;
    context.flags          = flags;
    context.componentEpoch = componentEpoch;
    context.requestID      = requestID;
    context.payload        = payload;
    context.payloadLength  = payloadLength;

    const auto send = [](void * value)
    {
      SendContext& sendContext = *static_cast<SendContext *>(value);
      sendContext.result       = sendContext.owner->SendClipboardToPeer(
        sendContext.peer, sendContext.bulk, sendContext.service,
        sendContext.messageType, sendContext.flags,
        sendContext.componentEpoch, sendContext.requestID,
        sendContext.payload, sendContext.payloadLength);
    };
    return ClipboardWithInputAuthority(peer, send, &context) ?
      context.result : NetworkClipboardSendResult::UNAUTHORIZED;
  }

  bool VideoPeerInfo(const NetworkVideoPeer& peer,
    NetworkVideoPeerInfo& info)
  {
    ConnectionPin connection = PinConnection(peer);
    if (!connection)
      return false;

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
    info.maxReliablePayload = (std::min)(
      capability.maxPayloadLength, NETWORK_MAX_CONTROL_PAYLOAD);
    info.maxDatagramPayload = capability.maxDatagramPayload;
    info.maxBulkPayload     =
      capability.flags & LG_NET_CAPABILITY_BULK ?
        capability.maxPayloadLength : 0;
    return true;
  }

  NetworkVideoSendResult SendVideo(
    const NetworkVideoPeer& peer, NetworkVideoDelivery delivery,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID,
    std::shared_ptr<const std::vector<uint8_t>> payload)
  {
    if (payload && payload->size() > UINT32_MAX)
      return NetworkVideoSendResult::FAILED;

    const auto payloadLength = static_cast<uint32_t>(
      payload ? payload->size() : 0U);
    const auto payloadData   = payloadLength ?
      static_cast<const void *>(payload->data()) : nullptr;

    ConnectionPin connection = PinConnection(peer);
    if (!connection)
      return NetworkVideoSendResult::FAILED;

    SendResult result = SendResult::FAILED;
    switch (delivery)
    {
      case NetworkVideoDelivery::RELIABLE:
        result = TrySendPacket(*connection, LG_NET_SERVICE_VIDEO,
          messageType, flags, componentEpoch, requestID,
          payloadData, payloadLength);
        break;

      case NetworkVideoDelivery::DATAGRAM:
        result = TrySendDatagram(*connection, LG_NET_SERVICE_VIDEO,
          messageType, flags, componentEpoch, requestID,
          payloadData, payloadLength, true, std::move(payload));
        break;

      case NetworkVideoDelivery::BULK:
        result = TrySendBulk(*connection, LG_NET_SERVICE_VIDEO,
          messageType, flags, componentEpoch, requestID,
          payloadData, payloadLength, std::move(payload));
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
    {
      CSRWExclusiveLock guard(connection.inputLock);
      connection.inputClosing = true;
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
          return
          {
            NETWORK_VIDEO_BULK_COUNT_MAX,
            NETWORK_VIDEO_BULK_BYTES_MAX,
          };

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

  static InputSend * AcquireInputSendLocked(Connection& connection)
  {
    for (uint32_t offset = 0; offset < NETWORK_INPUT_SEND_COUNT_MAX;
        ++offset)
    {
      const uint32_t index =
        (connection.inputSendNext + offset) % NETWORK_INPUT_SEND_COUNT_MAX;
      InputSend&     send  = connection.inputSends[index];
      if (send.active)
        continue;

      send.active              = true;
      connection.inputSendNext =
        (index + 1U) % NETWORK_INPUT_SEND_COUNT_MAX;
      return &send;
    }

    return nullptr;
  }

  static void ReleaseInputSendLocked(Connection& connection,
    InputSend& send)
  {
    if (!send.active || !connection.inputStreamSendCount ||
        connection.inputStreamSendBytes < send.length)
    {
      DEBUG_ERROR("Network input send accounting underflow for client %u",
        connection.client);
    }
    else
    {
      --connection.inputStreamSendCount;
      connection.inputStreamSendBytes -= send.length;
    }
    send.buffer   = {};
    send.length   = 0;
    send.active   = false;
    send.inputAck = false;
  }

  SendResult TrySendInputPacket(Connection& connection,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID, const void * payload,
    uint32_t payloadLength, bool inputAck)
  {
    const LGNetMessageRoute * route =
      lgNetMessageRoute(LG_NET_SERVICE_INPUT, messageType);
    if (!lgNetMessageRouteAllows(route, LG_NET_ROLE_SERVER,
          LG_NET_ROUTE_DELIVERY_RELIABLE) ||
        (payloadLength && !payload) ||
        payloadLength > NETWORK_INPUT_MAX_PAYLOAD ||
        flags & (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM |
          LG_NET_ENVELOPE_EARLY_DATA))
      return SendResult::FAILED;

    const size_t packetSize =
      LG_NET_ENVELOPE_WIRE_SIZE + payloadLength;
    if (packetSize > InputSend::CAPACITY)
      return SendResult::FAILED;

    InputSend   * send   = nullptr;
    QUIC_STATUS   status = QUIC_STATUS_SUCCESS;
    {
      LGNetPacketParameters parameters = {};
      HQUIC                 stream     = nullptr;
      CSRWExclusiveLock     sendGuard(connection.inputSendLock);
      {
        CSRWExclusiveLock guard(connection.inputLock);
        if (connection.inputClosing || !connection.inputStreamBound ||
            !connection.inputStream ||
            payloadLength > connection.inputStreamMaxPayload ||
            connection.inputStreamServiceVersion <
              route->serviceVersionMin ||
            connection.inputStreamSendSequence == UINT64_MAX)
          return SendResult::FAILED;
        if (connection.inputStreamSendCount >= NETWORK_INPUT_SEND_COUNT_MAX ||
            connection.inputStreamSendBytes >
              NETWORK_INPUT_SEND_BYTES_MAX - packetSize)
          return SendResult::BLOCKED;

        send = AcquireInputSendLocked(connection);
        if (!send)
          return SendResult::BLOCKED;

        send->length   = static_cast<uint32_t>(packetSize);
        send->inputAck = inputAck;
        ++connection.inputStreamSendCount;
        connection.inputStreamSendBytes += packetSize;

        lgNetPacketParametersInit(&parameters, LG_NET_SERVICE_INPUT,
          messageType, connection.inputStreamServiceVersion,
          route->messageVersion);
        parameters.flags          = flags | LG_NET_ENVELOPE_RELIABLE;
        parameters.sessionEpoch   = connection.inputStreamSessionEpoch;
        parameters.componentEpoch = componentEpoch;
        parameters.sequence       = ++connection.inputStreamSendSequence;
        parameters.requestID      = requestID;
        stream                    = connection.inputStream;
      }

      size_t wireSize = 0;
      if (!lgNetPacketConstruct(send->data, sizeof(send->data),
            &wireSize, &parameters, payload, payloadLength) ||
          wireSize != packetSize)
      {
        CSRWExclusiveLock guard(connection.inputLock);
        if (connection.inputStreamSendSequence == parameters.sequence)
          --connection.inputStreamSendSequence;
        ReleaseInputSendLocked(connection, *send);
        return SendResult::FAILED;
      }

      send->buffer.Buffer = send->data;
      send->buffer.Length = static_cast<uint32_t>(wireSize);
      status              = api->StreamSend(stream,
        &send->buffer, 1, QUIC_SEND_FLAG_PRIORITY_WORK, send);
      if (QUIC_FAILED(status))
      {
        CSRWExclusiveLock guard(connection.inputLock);
        if (connection.inputStreamSendSequence == parameters.sequence)
          --connection.inputStreamSendSequence;
        ReleaseInputSendLocked(connection, *send);
      }
    }

    if (QUIC_FAILED(status))
      return SendResult::FAILED;
    return SendResult::SENT;
  }

  SendResult TrySendPacket(Connection& connection, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID, const void * payload,
    uint32_t payloadLength, bool activateSession = false)
  {
    if (service == LG_NET_SERVICE_INPUT)
    {
      if (activateSession)
        return SendResult::FAILED;
      return TrySendInputPacket(connection, messageType, flags,
        componentEpoch, requestID, payload, payloadLength, false);
    }

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

      send->buffers[0].Buffer = send->data.data();
      send->buffers[0].Length = static_cast<uint32_t>(wireSize);
      status                  = api->StreamSend(
        connection.controlStream, send->buffers,
        send->bufferCount, QUIC_SEND_FLAG_NONE, send.get());
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
    uint32_t payloadLength,
    std::shared_ptr<const std::vector<uint8_t>> sharedPayload = {})
  {
    if (sharedPayload)
    {
      if (service != LG_NET_SERVICE_VIDEO ||
          sharedPayload->size() != static_cast<size_t>(payloadLength))
        return SendResult::FAILED;
      payload = payloadLength ? sharedPayload->data() : nullptr;
    }

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

    const size_t     packetSize     =
      LG_NET_ENVELOPE_WIRE_SIZE + payloadLength;
    const SendLimits limits         =
      GetSendLimits(service, Delivery::BULK);
    const bool       scatterPayload = static_cast<bool>(sharedPayload);
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
      if (!scatterPayload)
      {
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

      size_t wireSize = 0;
      if (scatterPayload)
      {
        LGNetEnvelope envelope;
        lgNetEnvelopeInit(&envelope, parameters.service,
          parameters.messageType, parameters.serviceVersion,
          parameters.messageVersion);
        envelope.flags          = parameters.flags;
        envelope.sessionEpoch   = parameters.sessionEpoch;
        envelope.componentEpoch = parameters.componentEpoch;
        envelope.sequence       = parameters.sequence;
        envelope.requestID      = parameters.requestID;
        envelope.payloadLength  = payloadLength;
        if (lgNetEnvelopeEncode(send->envelope, sizeof(send->envelope),
              &envelope))
          wireSize = packetSize;
      }
      else if (!lgNetPacketConstruct(send->data.data(), send->data.size(),
            &wireSize, &parameters, payload, payloadLength))
        wireSize = 0;

      if (wireSize != packetSize)
      {
        --stream->sendSequence;
        ReleaseSendLocked(connection, *send);
        return SendResult::FAILED;
      }

      if (scatterPayload)
      {
        send->buffers[0].Buffer = send->envelope;
        send->buffers[0].Length = LG_NET_ENVELOPE_WIRE_SIZE;
        send->sharedData        = std::move(sharedPayload);
        if (payloadLength)
        {
          send->buffers[1].Buffer = const_cast<uint8_t *>(
            send->sharedData->data());
          send->buffers[1].Length = payloadLength;
          send->bufferCount       = 2;
        }
      }
      else
      {
        send->buffers[0].Buffer = send->data.data();
        send->buffers[0].Length = static_cast<uint32_t>(wireSize);
      }
      status = api->StreamSend(stream->handle, send->buffers,
        send->bufferCount, QUIC_SEND_FLAG_NONE, send.get());
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
    uint32_t payloadLength, bool discardIfBlocked,
    std::shared_ptr<const std::vector<uint8_t>> sharedPayload = {})
  {
    if (sharedPayload)
    {
      if (service != LG_NET_SERVICE_VIDEO ||
          sharedPayload->size() != static_cast<size_t>(payloadLength))
        return SendResult::FAILED;
      payload = payloadLength ? sharedPayload->data() : nullptr;
    }

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

    const size_t          packetSize     =
      LG_NET_ENVELOPE_WIRE_SIZE + payloadLength;
    const bool            scatterPayload = static_cast<bool>(sharedPayload);
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
      if (!scatterPayload)
      {
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

      size_t wireSize = 0;
      if (scatterPayload)
      {
        LGNetEnvelope envelope;
        lgNetEnvelopeInit(&envelope, parameters.service,
          parameters.messageType, parameters.serviceVersion,
          parameters.messageVersion);
        envelope.flags          = parameters.flags;
        envelope.sessionEpoch   = parameters.sessionEpoch;
        envelope.componentEpoch = parameters.componentEpoch;
        envelope.sequence       = parameters.sequence;
        envelope.requestID      = parameters.requestID;
        envelope.payloadLength  = payloadLength;
        if (lgNetEnvelopeEncode(send->envelope, sizeof(send->envelope),
              &envelope))
          wireSize = packetSize;
      }
      else if (!lgNetPacketConstruct(send->data.data(), send->data.size(),
            &wireSize, &parameters, payload, payloadLength))
        wireSize = 0;

      if (wireSize != packetSize)
      {
        --connection.datagramSendSequence;
        ReleaseSendLocked(connection, *send);
        return SendResult::FAILED;
      }

      if (scatterPayload)
      {
        send->buffers[0].Buffer = send->envelope;
        send->buffers[0].Length = LG_NET_ENVELOPE_WIRE_SIZE;
        send->sharedData        = std::move(sharedPayload);
        if (payloadLength)
        {
          send->buffers[1].Buffer = const_cast<uint8_t *>(
            send->sharedData->data());
          send->buffers[1].Length = payloadLength;
          send->bufferCount       = 2;
        }
      }
      else
      {
        send->buffers[0].Buffer = send->data.data();
        send->buffers[0].Length = static_cast<uint32_t>(wireSize);
      }
      QUIC_SEND_FLAGS sendFlags = QUIC_SEND_FLAG_PRIORITY_WORK;
      if (discardIfBlocked)
        sendFlags |= QUIC_SEND_FLAG_CANCEL_ON_BLOCKED;
      status = api->DatagramSend(connection.handle, send->buffers,
        send->bufferCount, sendFlags, send.get());
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
        NETWORK_MAX_NEGOTIATED_PAYLOAD ? hello.maxPayloadLength :
          NETWORK_MAX_NEGOTIATED_PAYLOAD;
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
        (capability.service == LG_NET_SERVICE_INPUT ?
          capability.maxBidirectionalStreams != 1 :
          capability.maxBidirectionalStreams != 0) ||
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
        connection.maxPayload < minimumPayload ||
        (offered.service == LG_NET_SERVICE_INPUT &&
          offered.maxBidirectionalStreams != 1))
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
    if (offered.service == LG_NET_SERVICE_INPUT &&
        selected.maxPayloadLength > NETWORK_INPUT_MAX_PAYLOAD)
      selected.maxPayloadLength = NETWORK_INPUT_MAX_PAYLOAD;
    selected.maxDatagramPayload       = 0;
    selected.maxBidirectionalStreams  = offered.service ==
      LG_NET_SERVICE_INPUT ? 1 : 0;
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
      if (selected.maxDatagramPayload > selected.maxPayloadLength)
        selected.maxDatagramPayload = selected.maxPayloadLength;
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

  bool SendGuestInformation(Connection& connection, uint64_t requestID)
  {
    static const char CAPTURE[] = "Looking Glass IDD Driver";

    uint32_t maxPayload;
    {
      CSRWSharedLock guard(connection.lock);
      if (connection.closing)
        return false;
      maxPayload = connection.maxPayload;
    }
    if (maxPayload < LG_NET_CORE_GUEST_INFO_HEADER_WIRE_SIZE)
      return false;

    const std::string& osName   = CPlatformInfo::GetProductName();
    const std::string& cpuModel = CPlatformInfo::GetCPUModel();
    LGNetCoreGuestInfo info     = {};
    memcpy(info.uuid, CPlatformInfo::GetUUID(), sizeof(info.uuid));
    info.os         = LG_NET_CORE_GUEST_OS_WINDOWS;
    info.processors = TopologyCount(CPlatformInfo::GetProcCount());
    info.cores      = TopologyCount(CPlatformInfo::GetCoreCount());
    info.sockets    = TopologyCount(CPlatformInfo::GetSocketCount());
    info.version    = reinterpret_cast<const uint8_t *>(LG_VERSION_STR);
    info.osName     = reinterpret_cast<const uint8_t *>(osName.data());
    info.capture    = reinterpret_cast<const uint8_t *>(CAPTURE);
    info.cpuModel   = reinterpret_cast<const uint8_t *>(cpuModel.data());

    size_t remaining      = maxPayload -
      LG_NET_CORE_GUEST_INFO_HEADER_WIRE_SIZE;
    info.versionLength    = static_cast<uint16_t>(ConsumeUTF8Field(
      info.version, strlen(LG_VERSION_STR),
      LG_NET_CORE_GUEST_MAX_VERSION_LENGTH, remaining));
    info.osNameLength     = static_cast<uint16_t>(ConsumeUTF8Field(
      info.osName, osName.size(), LG_NET_CORE_GUEST_MAX_OS_NAME_LENGTH,
      remaining));
    info.captureLength    = static_cast<uint16_t>(ConsumeUTF8Field(
      info.capture, strlen(CAPTURE), LG_NET_CORE_GUEST_MAX_CAPTURE_LENGTH,
      remaining));
    info.cpuModelLength   = static_cast<uint16_t>(ConsumeUTF8Field(
      info.cpuModel, cpuModel.size(),
      LG_NET_CORE_GUEST_MAX_CPU_MODEL_LENGTH, remaining));

    const size_t payloadSize = lgNetCoreGuestInfoSize(&info);
    if (!payloadSize || payloadSize > maxPayload)
      return false;

    std::vector<uint8_t> payload;
    try
    {
      payload.resize(payloadSize);
    }
    catch (...)
    {
      return false;
    }

    return lgNetCoreGuestInfoEncode(
        payload.data(), payload.size(), &info) &&
      SendPacket(connection, LG_NET_SERVICE_CORE,
        LG_NET_CORE_MESSAGE_GUEST_INFO,
        LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
        0, requestID, payload.data(),
        static_cast<uint32_t>(payload.size()));
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
    if (!SendGuestInformation(connection, requestID))
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

  static void ClearPendingInputAckLocked(Connection& connection)
  {
    connection.inputAckClaimEpoch = 0;
    connection.inputAckRequestID  = 0;
    connection.inputAckStatus     = LG_NET_INPUT_STATUS_ERROR;
    connection.inputAckPending    = false;
  }

  static void ClearInputStateLocked(Connection& connection,
    bool clearClaim)
  {
    if (clearClaim)
    {
      connection.inputClaimantID          = 0;
      connection.inputClaimEpoch          = 0;
      connection.inputFlags               = 0;
      connection.inputLEDState            = 0;
      connection.inputClaimPending        = false;
      connection.inputLEDCheckPending     = false;
      connection.inputLEDWaitForSendReady = false;
      connection.inputLEDStateKnown       = false;
      ClearPendingInputAckLocked(connection);
    }
    connection.inputPointerButtons = 0;
    connection.inputMotionSequence = 0;
    connection.inputCumulativeX    = 0;
    connection.inputCumulativeY    = 0;
    connection.inputAbsoluteX      = 0;
    connection.inputAbsoluteY      = 0;
    connection.inputMotionAbsolute = false;
    connection.inputMotionBaseline = false;
    connection.keyboardModifiers   = 0;
    memset(connection.keyboardKeys, 0, sizeof(connection.keyboardKeys));
  }

  void InvalidateInputClaim(Connection& connection,
    uint64_t claimEpoch)
  {
    {
      CSRWExclusiveLock guard(connection.inputLock);
      if (connection.inputClaimEpoch == claimEpoch)
        ClearInputStateLocked(connection, true);
    }
    clipboard.Process(GetTickCount64());
  }

  static void RequestInputLEDCheckLocked(Connection& connection)
  {
    connection.inputLEDCheckPending = true;
    ++connection.inputLEDCheckGeneration;
    if (!connection.inputLEDCheckGeneration)
      ++connection.inputLEDCheckGeneration;
  }

  static void CompleteInputLEDCheck(Connection& connection,
    uint64_t claimEpoch, uint64_t checkGeneration)
  {
    CSRWExclusiveLock guard(connection.inputLock);
    if (connection.inputClaimEpoch == claimEpoch &&
        connection.inputLEDCheckGeneration == checkGeneration)
      connection.inputLEDCheckPending = false;
  }

  SendResult TrySendInputStatus(Connection& connection,
    uint16_t messageType,
    LGNetInputStatusCode code, uint64_t claimEpoch, uint64_t requestID,
    LGNetEnvelopeFlags flags, bool inputAck)
  {
    LGNetInputStatus status = {};
    {
      CSRWSharedLock guard(connection.inputLock);
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
    if (!lgNetInputStatusEncode(payload, sizeof(payload), &status))
      return SendResult::FAILED;
    return TrySendInputPacket(connection, messageType,
      flags | LG_NET_ENVELOPE_FINAL, claimEpoch, requestID,
      payload, sizeof(payload), inputAck);
  }

  bool SendInputStatus(Connection& connection, uint16_t messageType,
    LGNetInputStatusCode code, uint64_t claimEpoch, uint64_t requestID,
    LGNetEnvelopeFlags flags)
  {
    return TrySendInputStatus(connection, messageType, code,
      claimEpoch, requestID, flags, false) == SendResult::SENT;
  }

  SendResult TrySendPendingInputAck(Connection& connection)
  {
    uint64_t             claimEpoch = 0;
    uint64_t             requestID  = 0;
    LGNetInputStatusCode code       = LG_NET_INPUT_STATUS_ERROR;
    {
      CSRWExclusiveLock guard(connection.inputLock);
      if (!connection.inputAckPending || connection.inputAckSending ||
          connection.inputAckInFlight)
        return SendResult::SENT;

      claimEpoch                 = connection.inputAckClaimEpoch;
      requestID                  = connection.inputAckRequestID;
      code                       = connection.inputAckStatus;

      connection.inputAckSending  = true;
      connection.inputAckInFlight = true;
    }

    const SendResult result = TrySendInputStatus(connection,
      LG_NET_INPUT_MESSAGE_STATUS, code, claimEpoch, requestID,
      LG_NET_ENVELOPE_RESPONSE, true);
    {
      CSRWExclusiveLock guard(connection.inputLock);
      connection.inputAckSending = false;
      if (result != SendResult::SENT)
        connection.inputAckInFlight = false;
      if (result == SendResult::SENT && connection.inputAckPending &&
          connection.inputAckClaimEpoch == claimEpoch &&
          connection.inputAckRequestID == requestID &&
          connection.inputAckStatus == code)
        ClearPendingInputAckLocked(connection);
    }
    return result;
  }

  bool QueueInputAck(Connection& connection, LGNetInputStatusCode code,
    uint64_t claimEpoch, uint64_t requestID)
  {
    {
      CSRWExclusiveLock guard(connection.inputLock);
      connection.inputAckClaimEpoch = claimEpoch;
      connection.inputAckRequestID  = requestID;
      connection.inputAckStatus     = code;
      connection.inputAckPending    = true;
    }
    return TrySendPendingInputAck(connection) != SendResult::FAILED;
  }

  SendResult TrySendInputLEDs(Connection& connection)
  {
    std::shared_ptr<CNetworkSession> session         = {};
    uint64_t                         claimEpoch      = 0;
    uint64_t                         checkGeneration = 0;
    {
      CSRWSharedLock guard(connection.inputLock);
      if (!connection.inputLEDCheckPending ||
          connection.inputLEDWaitForSendReady)
        return SendResult::SENT;
      session         = connection.inputSession;
      claimEpoch      = connection.inputClaimEpoch;
      checkGeneration = connection.inputLEDCheckGeneration;
      if (connection.inputClaimPending)
        return SendResult::SENT;
    }
    if (!session || !claimEpoch)
    {
      CompleteInputLEDCheck(connection, claimEpoch, checkGeneration);
      return SendResult::SENT;
    }

    const InputTargetState state = session->InputState();
    if (!state.owned || !state.keyboardLEDsValid)
    {
      CompleteInputLEDCheck(connection, claimEpoch, checkGeneration);
      return SendResult::SENT;
    }

    LGNetInputLEDs leds  = {};
    leds.state           = state.keyboardLEDs & NETWORK_KEYBOARD_LED_MASK;
    leds.validMask       = NETWORK_KEYBOARD_LED_MASK;
    {
      CSRWExclusiveLock guard(connection.inputLock);
      if (connection.inputClaimEpoch != claimEpoch ||
          connection.inputLEDCheckGeneration != checkGeneration ||
          !connection.inputLEDCheckPending)
        return SendResult::SENT;
      if (connection.inputLEDStateKnown &&
          connection.inputLEDState == leds.state)
      {
        connection.inputLEDCheckPending = false;
        return SendResult::SENT;
      }
      if (connection.inputLEDSequence == UINT64_MAX)
        return SendResult::FAILED;
      leds.sequence                 = ++connection.inputLEDSequence;
      connection.inputLEDState      = leds.state;
      connection.inputLEDStateKnown = true;
    }

    uint8_t    payload[LG_NET_INPUT_LEDS_WIRE_SIZE];
    SendResult result = SendResult::FAILED;
    if (lgNetInputLEDsEncode(payload, sizeof(payload), &leds))
      result = TrySendInputPacket(connection,
        LG_NET_INPUT_MESSAGE_KEYBOARD_LEDS, LG_NET_ENVELOPE_FINAL,
        claimEpoch, 0, payload, sizeof(payload), false);
    {
      CSRWExclusiveLock guard(connection.inputLock);
      if (connection.inputClaimEpoch == claimEpoch &&
          connection.inputLEDSequence == leds.sequence &&
          connection.inputLEDState == leds.state)
      {
        if (result == SendResult::SENT)
        {
          connection.inputLEDWaitForSendReady = false;
          if (connection.inputLEDCheckGeneration == checkGeneration)
            connection.inputLEDCheckPending = false;
        }
        else
        {
          --connection.inputLEDSequence;
          connection.inputLEDStateKnown       = false;
          connection.inputLEDWaitForSendReady =
            result == SendResult::BLOCKED &&
            (connection.inputStreamSendCount ||
              connection.inputStreamSendBytes);
        }
      }
    }
    return result;
  }

  bool HandleInputClaim(Connection& connection,
    const LGNetPacketView& packet)
  {
    LGNetInputClaim claim = {};
    if (lgNetInputClaimDecode(&claim, packet.payload,
          packet.payloadLength) != LG_NET_PARSE_OK ||
        packet.envelope.componentEpoch != claim.claimEpoch ||
        packet.envelope.requestID != claim.claimEpoch)
      return false;

    std::shared_ptr<CNetworkSession> session;
    {
      CSRWSharedLock guard(connection.inputLock);
      session = connection.inputSession;
    }
    if (!session)
      return false;

    bool hadClaim = false;
    {
      CSRWSharedLock guard(connection.inputLock);
      hadClaim = connection.inputClaimantID != 0;
    }

    InputResult result = session->ClaimInput();
    if (result == InputResult::ACCEPTED && hadClaim)
      result = session->ResetInput();
    {
      CSRWExclusiveLock guard(connection.inputLock);
      const bool accepted = result == InputResult::ACCEPTED;
      ClearPendingInputAckLocked(connection);
      connection.inputClaimantID          = accepted ? claim.claimantID : 0;
      connection.inputClaimEpoch          = accepted ? claim.claimEpoch : 0;
      connection.inputReliableSequence    = 0;
      connection.inputDatagramSequence    = 0;
      connection.inputFlags               = accepted ? claim.flags : 0;
      connection.inputLEDState            = 0;
      connection.inputClaimPending        = accepted;
      connection.inputLEDWaitForSendReady = false;
      connection.inputLEDStateKnown       = false;
      ClearInputStateLocked(connection, false);
      if (accepted)
        RequestInputLEDCheckLocked(connection);
      else
        connection.inputLEDCheckPending = false;
    }

    const LGNetInputStatusCode code = InputStatusCode(result);
    const bool                 sent = SendInputStatus(connection,
        LG_NET_INPUT_MESSAGE_CLAIM_RESULT,
        code, claim.claimEpoch, packet.envelope.requestID,
        LG_NET_ENVELOPE_RESPONSE);
    {
      CSRWExclusiveLock guard(connection.inputLock);
      if (sent && connection.inputClaimEpoch == claim.claimEpoch)
        connection.inputClaimPending = false;
    }
    clipboard.Process(GetTickCount64());
    return sent;
  }

  bool ValidateInputOwner(Connection& connection,
    uint64_t claimEpoch, uint64_t sequence, bool datagram,
    uint64_t claimantID, LGNetInputClaimFlags requiredFlags)
  {
    CSRWExclusiveLock guard(connection.inputLock);
    uint64_t&         lastSequence = datagram ?
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

  static int32_t InputCumulativeStep(int64_t current, int64_t previous,
    int64_t& next)
  {
    if (current >= previous)
    {
      const uint64_t delta = static_cast<uint64_t>(current) -
        static_cast<uint64_t>(previous);
      if (delta <= static_cast<uint64_t>(LG_INPUT_MOUSE_DELTA_MAX))
      {
        next = current;
        return static_cast<int32_t>(delta);
      }
      next = previous + static_cast<int64_t>(LG_INPUT_MOUSE_DELTA_MAX);
      return LG_INPUT_MOUSE_DELTA_MAX;
    }

    const uint64_t delta = static_cast<uint64_t>(previous) -
      static_cast<uint64_t>(current);
    const uint64_t limit = static_cast<uint64_t>(
      -static_cast<int64_t>(LG_INPUT_MOUSE_DELTA_MIN));
    if (delta <= limit)
    {
      next = current;
      return -static_cast<int32_t>(delta);
    }
    next = previous + static_cast<int64_t>(LG_INPUT_MOUSE_DELTA_MIN);
    return LG_INPUT_MOUSE_DELTA_MIN;
  }

  InputPrepareResult PrepareInputPointer(Connection& connection,
    const LGNetPacketView& packet, uint64_t sequence, bool datagram,
    bool relative, int32_t wheel, LGNetPointerButtons changedButtons,
    int64_t cumulativeX, int64_t cumulativeY,
    LGNetPointerButtons& buttons, InputPointerMotion& motion,
    std::shared_ptr<CNetworkSession>& session)
  {
    const bool acknowledge =
      (packet.envelope.flags & LG_NET_ENVELOPE_ACK_REQUIRED) != 0;
    if (!sequence)
      return InputPrepareResult::INVALID;
    if (datagram)
    {
      if (wheel || changedButtons || acknowledge ||
          packet.envelope.requestID)
        return InputPrepareResult::INVALID;
    }
    else if (acknowledge ? packet.envelope.requestID != sequence :
        packet.envelope.requestID != 0)
      return InputPrepareResult::INVALID;

    CSRWExclusiveLock guard(connection.inputLock);
    uint64_t&         lastSequence = datagram ?
      connection.inputDatagramSequence :
      connection.inputReliableSequence;
    session = connection.inputSession;
    if (!session)
      return InputPrepareResult::INVALID;
    if (!connection.inputClaimantID ||
        connection.inputClaimEpoch != packet.envelope.componentEpoch ||
        !(connection.inputFlags & LG_NET_INPUT_CLAIM_POINTER) ||
        sequence <= lastSequence)
      return InputPrepareResult::STALE;

    const bool motionApplies  =
      sequence > connection.inputMotionSequence;
    const bool baseline       = connection.inputMotionBaseline;
    const bool modeChange     = motionApplies && baseline &&
      connection.inputMotionAbsolute == relative;
    const bool buttonChange   = buttons != connection.inputPointerButtons;
    if (datagram)
    {
      if (motionApplies && (!baseline || modeChange))
        return InputPrepareResult::INVALID;
    }
    else
    {
      const bool transition = wheel || changedButtons || buttonChange ||
        (motionApplies && (!baseline || modeChange));
      if (transition && !acknowledge)
        return InputPrepareResult::INVALID;
    }

    lastSequence = sequence;
    if (!datagram)
      connection.inputMotionBaseline = true;
    motion.claimEpoch      = packet.envelope.componentEpoch;
    motion.sequence        = sequence;
    motion.previousX       = connection.inputCumulativeX;
    motion.previousY       = connection.inputCumulativeY;
    motion.previousButtons = connection.inputPointerButtons;
    if (datagram)
      buttons = connection.inputPointerButtons;
    else
      connection.inputPointerButtons = buttons;

    motion.apply = motionApplies;
    if (motion.apply)
    {
      connection.inputMotionSequence = sequence;
      if (relative)
      {
        motion.cumulativeX = cumulativeX;
        motion.cumulativeY = cumulativeY;
      }
      else
      {
        connection.inputAbsoluteX   = motion.absoluteX;
        connection.inputAbsoluteY   = motion.absoluteY;
        connection.inputCumulativeX = cumulativeX;
        connection.inputCumulativeY = cumulativeY;
      }
      connection.inputMotionAbsolute = !relative;
    }

    motion.useAbsolute = connection.inputMotionAbsolute;
    if (motion.useAbsolute)
    {
      motion.absoluteX = connection.inputAbsoluteX;
      motion.absoluteY = connection.inputAbsoluteY;
    }
    return InputPrepareResult::READY;
  }

  InputResult SendInputRelativeMotion(Connection& connection,
    const std::shared_ptr<CNetworkSession>& session,
    const InputPointerMotion& motion, int32_t wheel,
    LGNetPointerButtons buttons)
  {
    int64_t cumulativeX = motion.previousX;
    int64_t cumulativeY = motion.previousY;

    InputResult result = InputResult::STALE;
    for (uint32_t chunk = 0; chunk < NETWORK_INPUT_RECOVERY_LIMIT; ++chunk)
    {
      int64_t       nextX   = cumulativeX;
      int64_t       nextY   = cumulativeY;
      const int32_t deltaX  = InputCumulativeStep(
        motion.cumulativeX, cumulativeX, nextX);
      const int32_t deltaY  = InputCumulativeStep(
        motion.cumulativeY, cumulativeY, nextY);
      const bool    reached =
        nextX == motion.cumulativeX && nextY == motion.cumulativeY;
      const bool    final   =
        reached || chunk + 1U == NETWORK_INPUT_RECOVERY_LIMIT;

      result = session->SendMouseRelative(deltaX, deltaY,
        final ? wheel : 0, final ? buttons : motion.previousButtons);
      if (result != InputResult::ACCEPTED)
        break;

      cumulativeX = nextX;
      cumulativeY = nextY;
      if (final)
      {
        if (!reached)
        {
          cumulativeX = motion.cumulativeX;
          cumulativeY = motion.cumulativeY;
        }
        break;
      }
    }

    {
      CSRWExclusiveLock guard(connection.inputLock);
      if (connection.inputClaimEpoch == motion.claimEpoch &&
          connection.inputMotionSequence == motion.sequence &&
          !connection.inputMotionAbsolute)
      {
        connection.inputCumulativeX = cumulativeX;
        connection.inputCumulativeY = cumulativeY;
      }
    }
    return result;
  }

  bool HandleInputControl(Connection& connection,
    const LGNetPacketView& packet, bool datagram)
  {
    LGNetInputControl control = {};
    if (lgNetInputControlDecode(&control, packet.payload,
          packet.payloadLength) != LG_NET_PARSE_OK ||
        packet.envelope.componentEpoch != control.claimEpoch ||
        packet.envelope.requestID ||
        (datagram && packet.envelope.messageType !=
          LG_NET_INPUT_MESSAGE_KEEPALIVE))
      return false;

    if (!ValidateInputOwner(connection, control.claimEpoch,
          control.sequence, datagram, control.claimantID, 0))
      return true;

    std::shared_ptr<CNetworkSession> session;
    {
      CSRWSharedLock guard(connection.inputLock);
      session = connection.inputSession;
    }
    if (!session)
      return false;

    InputResult result = InputResult::STALE;
    switch (packet.envelope.messageType)
    {
      case LG_NET_INPUT_MESSAGE_KEEPALIVE:
        result = session->TouchInput();
        if (result == InputResult::ACCEPTED)
        {
          CSRWExclusiveLock guard(connection.inputLock);
          if (connection.inputClaimEpoch == control.claimEpoch)
            RequestInputLEDCheckLocked(connection);
        }
        break;

      case LG_NET_INPUT_MESSAGE_RESET:
        result = session->ResetInput();
        {
          CSRWExclusiveLock guard(connection.inputLock);
          connection.inputPointerButtons = 0;
          connection.keyboardModifiers   = 0;
          memset(connection.keyboardKeys, 0,
            sizeof(connection.keyboardKeys));
          if (connection.inputMotionSequence <= control.sequence)
          {
            connection.inputMotionSequence = control.sequence;
            connection.inputCumulativeX    = 0;
            connection.inputCumulativeY    = 0;
            connection.inputAbsoluteX      = 0;
            connection.inputAbsoluteY      = 0;
            connection.inputMotionAbsolute = false;
            connection.inputMotionBaseline = false;
          }
        }
        break;

      case LG_NET_INPUT_MESSAGE_RELEASE:
        result = session->ReleaseInput(false);
        {
          CSRWExclusiveLock guard(connection.inputLock);
          ClearInputStateLocked(connection, true);
        }
        break;

      default:
        return false;
    }

    if (result == InputResult::ACCEPTED &&
        packet.envelope.messageType == LG_NET_INPUT_MESSAGE_KEEPALIVE)
      return true;
    const LGNetInputStatusCode code =
      packet.envelope.messageType == LG_NET_INPUT_MESSAGE_RELEASE &&
        result == InputResult::ACCEPTED ?
          static_cast<LGNetInputStatusCode>(LG_NET_INPUT_STATUS_RELEASED) :
          InputStatusCode(result);
    if (result != InputResult::ACCEPTED)
      InvalidateInputClaim(connection, control.claimEpoch);
    const bool sent = SendInputStatus(connection, LG_NET_INPUT_MESSAGE_STATUS,
      code, control.claimEpoch, 0, 0);
    if (result == InputResult::ACCEPTED &&
        packet.envelope.messageType == LG_NET_INPUT_MESSAGE_RELEASE)
      clipboard.Process(GetTickCount64());
    return sent;
  }

  bool HandleInputKeyboard(Connection& connection,
    const LGNetPacketView& packet)
  {
    LGNetInputKeyboard keyboard = {};
    if (lgNetInputKeyboardDecode(&keyboard, packet.payload,
        packet.payloadLength) != LG_NET_PARSE_OK ||
        keyboard.usagePage != NETWORK_KEYBOARD_USAGE_PAGE ||
        !keyboard.usage || keyboard.usage >= 256 ||
        packet.envelope.componentEpoch == 0 ||
        packet.envelope.requestID)
      return false;

    uint8_t                          keys[NETWORK_KEYBOARD_KEY_COUNT] = {};
    std::shared_ptr<CNetworkSession> session                         = {};
    uint8_t                          modifiers                       = 0;
    {
      CSRWExclusiveLock guard(connection.inputLock);
      if (!connection.inputClaimantID ||
          connection.inputClaimEpoch !=
            packet.envelope.componentEpoch ||
          !(connection.inputFlags & LG_NET_INPUT_CLAIM_KEYBOARD) ||
          keyboard.sequence <= connection.inputReliableSequence)
        return true;

      connection.inputReliableSequence = keyboard.sequence;
      connection.keyboardModifiers     =
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
      session   = connection.inputSession;
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
    return true;
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
    InputResult                      result  = InputResult::STALE;
    std::shared_ptr<CNetworkSession> session = {};

    if (packet.envelope.messageType ==
        LG_NET_INPUT_MESSAGE_MOUSE_RELATIVE)
    {
      LGNetInputRelative relative = {};
      if (lgNetInputRelativeDecode(&relative, packet.payload,
            packet.payloadLength) != LG_NET_PARSE_OK || relative.wheelX)
        return false;
      InputPointerMotion       motion   = {};
      const InputPrepareResult prepared = PrepareInputPointer(connection,
        packet, relative.sequence, datagram, true, relative.wheelY,
        relative.changedButtons, relative.cumulativeX,
        relative.cumulativeY, relative.buttons, motion, session);
      if (prepared == InputPrepareResult::INVALID)
        return false;
      if (prepared == InputPrepareResult::STALE)
        return true;
      if (datagram && !motion.apply)
        return true;
      if (motion.useAbsolute)
        result = session->SendMouseAbsolute(
          motion.absoluteX, motion.absoluteY,
          relative.wheelY, relative.buttons);
      else if (motion.apply)
        result = SendInputRelativeMotion(connection, session, motion,
          relative.wheelY, relative.buttons);
      else
        result = session->SendMouseRelative(0, 0,
          relative.wheelY, relative.buttons);
    }
    else
    {
      LGNetInputAbsolute absolute = {};
      if (lgNetInputAbsoluteDecode(&absolute, packet.payload,
            packet.payloadLength) != LG_NET_PARSE_OK || absolute.wheelX)
        return false;
      InputPointerMotion       motion   = {};
      motion.absoluteX = NormalizeAbsolute(absolute.x, absolute.width);
      motion.absoluteY = NormalizeAbsolute(absolute.y, absolute.height);
      const InputPrepareResult prepared = PrepareInputPointer(connection,
        packet, absolute.sequence, datagram, false, absolute.wheelY,
        absolute.changedButtons, absolute.cumulativeX,
        absolute.cumulativeY, absolute.buttons, motion, session);
      if (prepared == InputPrepareResult::INVALID)
        return false;
      if (prepared == InputPrepareResult::STALE)
        return true;
      if (datagram && !motion.apply)
        return true;
      if (motion.useAbsolute)
        result = session->SendMouseAbsolute(
          motion.absoluteX, motion.absoluteY,
          absolute.wheelY, absolute.buttons);
      else
        result = session->SendMouseRelative(
          0, 0, absolute.wheelY, absolute.buttons);
    }

    const bool acknowledge =
      (packet.envelope.flags & LG_NET_ENVELOPE_ACK_REQUIRED) != 0;
    if (result == InputResult::ACCEPTED && !acknowledge)
      return true;
    if (result != InputResult::ACCEPTED)
      InvalidateInputClaim(connection, packet.envelope.componentEpoch);
    if (acknowledge)
      return QueueInputAck(connection, InputStatusCode(result),
        packet.envelope.componentEpoch, packet.envelope.requestID);
    return SendInputStatus(connection, LG_NET_INPUT_MESSAGE_STATUS,
      InputStatusCode(result), packet.envelope.componentEpoch, 0, 0);
  }

  bool HandleInput(Connection& connection, const LGNetPacketView& packet,
    bool datagram)
  {
    if (datagram)
    {
      if (packet.envelope.flags & LG_NET_ENVELOPE_ACK_REQUIRED)
        return false;
      if (packet.envelope.messageType != LG_NET_INPUT_MESSAGE_KEEPALIVE &&
          packet.envelope.messageType !=
            LG_NET_INPUT_MESSAGE_MOUSE_RELATIVE &&
          packet.envelope.messageType !=
            LG_NET_INPUT_MESSAGE_MOUSE_ABSOLUTE)
        return false;
    }

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

    if (TrySendPendingInputAck(connection) == SendResult::FAILED ||
        TrySendInputLEDs(connection) == SendResult::FAILED)
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
    const bool         recoveryRequest  =
      packet.envelope.service == LG_NET_SERVICE_RECOVERY &&
      (packet.envelope.messageType == LG_NET_RECOVERY_MESSAGE_GET_INFO ||
       packet.envelope.messageType == LG_NET_RECOVERY_MESSAGE_REQUEST);
    const bool         controlRequest   =
      packet.envelope.service == LG_NET_SERVICE_CONTROL &&
      (packet.envelope.messageType ==
         LG_NET_CONTROL_MESSAGE_CURSOR_POSITION ||
       packet.envelope.messageType == LG_NET_CONTROL_MESSAGE_DISPLAY_SIZE ||
       packet.envelope.messageType == LG_NET_CONTROL_MESSAGE_FRAME_SCHEDULE);
    const bool         audioResponse    =
      packet.envelope.service == LG_NET_SERVICE_AUDIO &&
      packet.envelope.messageType == LG_NET_AUDIO_MESSAGE_STATE_ACK;
    const bool         clipboardPacket  =
      packet.envelope.service == LG_NET_SERVICE_CLIPBOARD ||
      packet.envelope.service == LG_NET_SERVICE_FILE;
    LGNetEnvelopeFlags expectedFlags    =
      LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_FINAL;
    if (audioSubscribe || videoSubscribe || recoveryRequest || controlRequest)
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
    bool           closing;
    {
      CSRWSharedLock guard(connection.lock);
      handshake    = connection.handshake;
      sessionEpoch = connection.sessionEpoch;
      maxPayload   = connection.maxPayload;
      closing      = connection.closing;
    }

    if (closing)
      return true;

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

  bool HandleInputStreamPacket(Connection& connection,
    const LGNetPacketView& packet)
  {
    LGNetEnvelopeFlags expectedFlags =
      LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_FINAL;
    if (packet.envelope.messageType == LG_NET_INPUT_MESSAGE_CLAIM ||
        ((packet.envelope.messageType ==
            LG_NET_INPUT_MESSAGE_MOUSE_RELATIVE ||
          packet.envelope.messageType ==
            LG_NET_INPUT_MESSAGE_MOUSE_ABSOLUTE) &&
          (packet.envelope.flags & LG_NET_ENVELOPE_ACK_REQUIRED)))
      expectedFlags |= LG_NET_ENVELOPE_ACK_REQUIRED;
    if (packet.envelope.headerSize != LG_NET_ENVELOPE_WIRE_SIZE ||
        packet.envelope.service != LG_NET_SERVICE_INPUT ||
        packet.envelope.flags != expectedFlags ||
        !lgNetEnvelopeRouteValid(&packet.envelope, LG_NET_ROLE_CLIENT,
          LG_NET_ROUTE_DELIVERY_RELIABLE))
      return false;

    {
      CSRWSharedLock guard(connection.inputLock);
      if (connection.inputClosing || !connection.inputStreamBound ||
          packet.envelope.sessionEpoch !=
            connection.inputStreamSessionEpoch ||
          packet.envelope.serviceVersion !=
            connection.inputStreamServiceVersion ||
          packet.envelope.payloadLength > connection.inputStreamMaxPayload)
        return false;
    }
    return HandleInput(connection, packet, false);
  }

  bool DrainInputReceive(Connection& connection)
  {
    for (;;)
    {
      LGNetStreamPacket       streamPacket = {};
      const LGNetStreamResult result       = lgNetStreamParserNext(
        &connection.inputReceiveParser, &streamPacket, nullptr);
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
        if (streamPacket.payloadLength >
            connection.inputReceivePayload.size())
          return false;
        memcpy(connection.inputReceivePayload.data(),
          streamPacket.payload[0].data, streamPacket.payload[0].size);
        memcpy(connection.inputReceivePayload.data() +
            streamPacket.payload[0].size, streamPacket.payload[1].data,
          streamPacket.payload[1].size);
        packet.payload = connection.inputReceivePayload.data();
      }

      {
        CSRWExclusiveLock guard(connection.inputLock);
        if (!connection.inputStreamBound || !packet.envelope.sequence ||
            connection.inputStreamReceiveSequence == UINT64_MAX ||
            packet.envelope.sequence !=
              connection.inputStreamReceiveSequence + 1)
          return false;
        connection.inputStreamReceiveSequence = packet.envelope.sequence;
      }
      if (!HandleInputStreamPacket(connection, packet) ||
          !lgNetStreamParserConsume(&connection.inputReceiveParser))
        return false;

      if (!lgNetStreamParserBuffered(&connection.inputReceiveParser))
        lgNetStreamParserReset(&connection.inputReceiveParser);
    }
  }

  bool ActivateInputReceive(Connection& connection)
  {
    LGNetStreamBinding binding  = {};
    uint64_t           streamID = 0;
    {
      CSRWSharedLock guard(connection.inputLock);
      if (connection.inputBindingLength !=
            sizeof(connection.inputBindingData) ||
          connection.inputStreamBound)
        return false;
      streamID = connection.inputStreamID;
      if (lgNetStreamBindingDecode(&binding, connection.inputBindingData,
            sizeof(connection.inputBindingData)) != LG_NET_PARSE_OK)
        return false;
    }
    if (binding.service != LG_NET_SERVICE_INPUT ||
        binding.originRole != LG_NET_ROLE_CLIENT || binding.flags ||
        binding.streamID != streamID)
      return false;

    std::shared_ptr<CNetworkSession> session    = {};
    LGNetCapability                  capability = {};
    {
      CSRWSharedLock guard(connection.lock);
      const uint16_t inputMask = UINT16_C(1) << LG_NET_SERVICE_INPUT;
      if (connection.closing ||
          connection.handshake != HandshakeState::READY ||
          binding.sessionEpoch != connection.sessionEpoch ||
          !(connection.selectedServices & inputMask) ||
          !(connection.reliableServices & inputMask) ||
          connection.selectedCapabilities
            [LG_NET_SERVICE_INPUT].maxBidirectionalStreams != 1 ||
          !connection.session)
        return false;
      capability =
        connection.selectedCapabilities[LG_NET_SERVICE_INPUT];
      session    = connection.session;
    }

    LGNetSequenceTracker datagramReceive = {};
    if (!lgNetSequenceTrackerInit(&datagramReceive, binding.sessionEpoch,
          NETWORK_SEQUENCE_COMPONENT, NETWORK_DATAGRAM_WINDOW, false))
      return false;

    CSRWExclusiveLock guard(connection.inputLock);
    if (connection.inputClosing || !connection.inputStream ||
        connection.inputStreamID != streamID ||
        connection.inputBindingLength !=
          sizeof(connection.inputBindingData) ||
        connection.inputStreamBound)
      return false;
    connection.inputStreamSessionEpoch       = binding.sessionEpoch;
    connection.inputStreamServiceVersion     = capability.versionMin;
    connection.inputStreamMaxPayload         = capability.maxPayloadLength;
    connection.inputDatagramLimit            =
      capability.maxDatagramPayload;
    connection.inputSession                  = std::move(session);
    connection.inputDatagramReceive          = datagramReceive;
    connection.inputStreamBound              = true;
    return true;
  }

  bool ReceiveInput(Connection& connection, const QUIC_BUFFER * buffers,
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
        bool bound = false;
        {
          CSRWSharedLock guard(connection.inputLock);
          bound = connection.inputStreamBound;
        }
        if (!bound)
        {
          bool activate = false;
          {
            CSRWExclusiveLock guard(connection.inputLock);
            const size_t remaining = buffers[index].Length - offset;
            size_t       length    = sizeof(connection.inputBindingData) -
              connection.inputBindingLength;
            if (length > remaining)
              length = remaining;
            memcpy(connection.inputBindingData +
                connection.inputBindingLength,
              buffers[index].Buffer + offset, length);
            connection.inputBindingLength += length;
            offset                        += length;
            activate = connection.inputBindingLength ==
              sizeof(connection.inputBindingData);
          }
          if (activate && !ActivateInputReceive(connection))
            return false;
          continue;
        }

        size_t accepted = 0;
        if (!lgNetStreamParserAppend(&connection.inputReceiveParser,
              buffers[index].Buffer + offset,
              buffers[index].Length - offset, &accepted))
          return false;
        offset += accepted;
        if (!DrainInputReceive(connection) ||
            (!accepted &&
              !lgNetStreamParserWritable(&connection.inputReceiveParser)))
          return false;
      }
    }

    bool bound = false;
    {
      CSRWSharedLock guard(connection.inputLock);
      bound = connection.inputStreamBound;
    }
    return !bound || DrainInputReceive(connection);
  }

  bool AcceptInputStream(Connection& connection, HQUIC handle,
    QUIC_STREAM_OPEN_FLAGS flags)
  {
    if (flags != QUIC_STREAM_OPEN_FLAG_NONE)
      return false;

    QUIC_UINT62 streamID     = 0;
    uint32_t    streamIDSize = sizeof(streamID);
    if (QUIC_FAILED(api->GetParam(handle, QUIC_PARAM_STREAM_ID,
          &streamIDSize, &streamID)) ||
        streamIDSize != sizeof(streamID) || (streamID & 3U) != 0U)
      return false;

    uint32_t maxPayload = 0;
    {
      CSRWSharedLock guard(connection.lock);
      const uint16_t inputMask = UINT16_C(1) << LG_NET_SERVICE_INPUT;
      if (connection.closing ||
          connection.handshake != HandshakeState::READY ||
          !(connection.selectedServices & inputMask) ||
          !(connection.reliableServices & inputMask) ||
          connection.selectedCapabilities
            [LG_NET_SERVICE_INPUT].maxBidirectionalStreams != 1)
        return false;
      maxPayload = connection.selectedCapabilities
        [LG_NET_SERVICE_INPUT].maxPayloadLength;
    }
    if (!maxPayload || maxPayload > NETWORK_INPUT_MAX_PAYLOAD)
      return false;

    const uint16_t priority = UINT16_MAX;
    if (QUIC_FAILED(api->SetParam(handle, QUIC_PARAM_STREAM_PRIORITY,
          sizeof(priority), &priority)))
      return false;

    std::vector<uint8_t> receiveData;
    std::vector<uint8_t> receivePayload;
    try
    {
      receiveData.resize(LG_NET_ENVELOPE_WIRE_SIZE + maxPayload);
      receivePayload.resize(maxPayload);
    }
    catch (...)
    {
      return false;
    }

    {
      CSRWExclusiveLock guard(connection.inputLock);
      if (connection.inputClosing || connection.inputStream)
        return false;
      connection.inputReceiveData.swap(receiveData);
      connection.inputReceivePayload.swap(receivePayload);
      if (!lgNetStreamParserInit(&connection.inputReceiveParser,
            connection.inputReceiveData.data(),
            connection.inputReceiveData.size(), maxPayload))
      {
        connection.inputReceiveData.clear();
        connection.inputReceivePayload.clear();
        return false;
      }
      connection.inputStream        = handle;
      connection.inputStreamID      = streamID;
      connection.inputBindingLength = 0;
      memset(connection.inputBindingData, 0,
        sizeof(connection.inputBindingData));
    }

    api->SetCallbackHandler(handle,
      reinterpret_cast<void *>(InputStreamCallback), &connection);
    return true;
  }

  bool HandleInputDatagram(Connection& connection,
    const LGNetPacketView& packet)
  {
    {
      CSRWExclusiveLock guard(connection.inputLock);
      if (connection.inputClosing || !connection.inputStreamBound ||
          packet.envelope.sessionEpoch !=
            connection.inputStreamSessionEpoch ||
          packet.envelope.serviceVersion !=
            connection.inputStreamServiceVersion ||
          !connection.inputDatagramLimit ||
          packet.envelope.payloadLength >
            connection.inputDatagramLimit)
        return false;

      const LGNetSequenceResult sequenceResult = lgNetSequenceTrackerCheck(
        &connection.inputDatagramReceive, packet.envelope.sessionEpoch,
        NETWORK_SEQUENCE_COMPONENT, packet.envelope.sequence);
      if (sequenceResult == LG_NET_SEQUENCE_DUPLICATE ||
          sequenceResult == LG_NET_SEQUENCE_TOO_OLD)
        return true;
      if (!lgNetSequenceAccepted(sequenceResult))
        return false;
    }
    return HandleInput(connection, packet, true);
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

    if (packet.envelope.service == LG_NET_SERVICE_INPUT)
      return HandleInputDatagram(connection, packet);

    const uint16_t serviceMask = UINT16_C(1) << packet.envelope.service;
    {
      CSRWExclusiveLock guard(connection.lock);
      if (connection.closing)
        return true;
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
      connection.detached = true;
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
    {
      CSRWExclusiveLock guard(connection.inputLock);
      if (connection.inputStreamSendCount ||
          connection.inputStreamSendBytes)
        DEBUG_ERROR("Network client %u closed with input sends in flight",
          connection.client);
      connection.inputSession.reset();
    }
    video.PeerDisconnected(VideoPeer(connection));
    clipboard.PeerDisconnected(ClipboardPeer(connection));
    audio.PeerDisconnected(AudioPeer(connection));
    if (session)
      transport.RemoveSession(client, generation);
    session.reset();

    bool dispose = false;
    {
      CSRWExclusiveLock guard(lock);
      connection.cleanupComplete = true;
      dispose                    = connection.detached && !connection.pins;
    }
    if (dispose)
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

        bool controlAccepted = false;
        {
          CSRWExclusiveLock guard(connection.lock);
          if (flags == QUIC_STREAM_OPEN_FLAG_NONE &&
              !connection.closing &&
              !connection.controlStream)
          {
            connection.controlStream = event->PEER_STREAM_STARTED.Stream;
            controlAccepted          = true;
          }
        }

        if (controlAccepted)
          owner.api->SetCallbackHandler(event->PEER_STREAM_STARTED.Stream,
            reinterpret_cast<void *>(StreamCallback), &connection);
        else if (owner.AcceptInputStream(connection,
              event->PEER_STREAM_STARTED.Stream, flags))
          return QUIC_STATUS_SUCCESS;
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
        {
          CSRWExclusiveLock guard(connection.lock);
          connection.closing = true;
        }
        {
          CSRWExclusiveLock guard(connection.inputLock);
          connection.inputClosing = true;
        }
        return QUIC_STATUS_SUCCESS;
      }

      case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
      {
        if (event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode)
          DEBUG_WARN("Network client %u closed the connection: error 0x%llx",
            connection.client, static_cast<unsigned long long>(
              event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode));
        {
          CSRWExclusiveLock guard(connection.lock);
          connection.closing = true;
        }
        {
          CSRWExclusiveLock guard(connection.inputLock);
          connection.inputClosing = true;
        }
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

  static QUIC_STATUS QUIC_API InputStreamCallback(HQUIC handle,
    void * context, QUIC_STREAM_EVENT * event)
  {
    Connection& connection = *static_cast<Connection *>(context);
    Impl&       owner      = connection.owner;
    CallbackGuard callback(owner);

    switch (event->Type)
    {
      case QUIC_STREAM_EVENT_RECEIVE:
        if (!owner.ReceiveInput(connection, event->RECEIVE.Buffers,
              event->RECEIVE.BufferCount))
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
        return QUIC_STATUS_SUCCESS;

      case QUIC_STREAM_EVENT_SEND_COMPLETE:
      {
        InputSend& send     = *static_cast<InputSend *>(
          event->SEND_COMPLETE.ClientContext);
        bool       inputAck = false;
        {
          CSRWExclusiveLock guard(connection.inputLock);
          inputAck = send.inputAck;
          ReleaseInputSendLocked(connection, send);
          connection.inputLEDWaitForSendReady = false;
          if (inputAck)
            connection.inputAckInFlight = false;
        }
        if (owner.TrySendPendingInputAck(connection) == SendResult::FAILED ||
            owner.TrySendInputLEDs(connection) == SendResult::FAILED)
          owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
        return QUIC_STATUS_SUCCESS;
      }

      case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
      case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
      case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        owner.Shutdown(connection, NETWORK_ERROR_PROTOCOL);
        return QUIC_STATUS_SUCCESS;

      case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
      {
        CSRWExclusiveLock sendGuard(connection.inputSendLock);
        {
          CSRWExclusiveLock guard(connection.inputLock);
          if (connection.inputStream == handle)
          {
            connection.inputStream        = nullptr;
            connection.inputStreamBound   = false;
            connection.inputClosing       = true;
            connection.inputAckSending    = false;
            connection.inputAckInFlight   = false;
            ClearPendingInputAckLocked(connection);
            connection.inputSession.reset();
          }
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

        send->buffers[0].Buffer = send->data.data();
        send->buffers[0].Length = LG_NET_STREAM_BINDING_WIRE_SIZE;
        send->service           = stream.service;
        send->length            = LG_NET_STREAM_BINDING_WIRE_SIZE;
        send->delivery          = Delivery::BULK;
        send->accounted         = false;

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
          status = valid ? owner.api->StreamSend(handle, send->buffers,
            send->bufferCount, QUIC_SEND_FLAG_NONE, send.get()) :
            QUIC_STATUS_ABORTED;
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
    std::vector<Connection *> pinned;
    {
      CSRWExclusiveLock guard(lock);
      if (stopping || !api)
        return;
      try
      {
        pinned = connections;
      }
      catch (...)
      {
        return;
      }
      for (Connection * connection : pinned)
      {
        ++connection->pins;
        ++activePins;
      }
    }

    for (Connection * connection : pinned)
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
        ReleaseConnectionPin(*connection);
        continue;
      }

      {
        CSRWExclusiveLock inputGuard(connection->inputLock);
        connection->inputClosing = true;
      }

      DEBUG_WARN("Closing network client %u after an incomplete handshake",
        client);
      api->ConnectionShutdown(handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
        NETWORK_ERROR_HANDSHAKE_TIMEOUT);

      ReleaseConnectionPin(*connection);
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
    registrationConfig.ExecutionProfile =
      QUIC_EXECUTION_PROFILE_TYPE_REAL_TIME;
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
    quicSettings.IsSet.HandshakeIdleTimeoutMs         = TRUE;
    quicSettings.IsSet.IdleTimeoutMs                  = TRUE;
    quicSettings.IsSet.StreamRecvBufferDefault        = TRUE;
    quicSettings.IsSet.ConnFlowControlWindow          = TRUE;
    quicSettings.IsSet.InitialRttMs                   = TRUE;
    quicSettings.IsSet.MaxAckDelayMs                  = TRUE;
    quicSettings.IsSet.KeepAliveIntervalMs            = TRUE;
    quicSettings.IsSet.PeerBidiStreamCount            = TRUE;
    quicSettings.IsSet.PeerUnidiStreamCount           = TRUE;
    quicSettings.IsSet.SendBufferingEnabled           = TRUE;
    quicSettings.IsSet.DatagramReceiveEnabled         = TRUE;
    quicSettings.IsSet.ServerResumptionLevel          = TRUE;
    quicSettings.IsSet.StreamRecvWindowUnidiDefault   = TRUE;
    quicSettings.HandshakeIdleTimeoutMs       = NETWORK_HANDSHAKE_TIMEOUT_MS;
    quicSettings.IdleTimeoutMs                = NETWORK_IDLE_TIMEOUT_MS;
    quicSettings.StreamRecvBufferDefault      =
      NETWORK_STREAM_RECV_BUFFER;
    quicSettings.ConnFlowControlWindow        =
      NETWORK_CONN_RECV_WINDOW;
    quicSettings.InitialRttMs                 = NETWORK_INITIAL_RTT_MS;
    quicSettings.MaxAckDelayMs                = 1;
    quicSettings.KeepAliveIntervalMs          =
      NETWORK_KEEPALIVE_INTERVAL_MS;
    quicSettings.PeerBidiStreamCount          = NETWORK_PEER_BIDI_STREAMS;
    quicSettings.PeerUnidiStreamCount         = NETWORK_PEER_UNIDI_STREAMS;
    quicSettings.SendBufferingEnabled         = FALSE;
    quicSettings.DatagramReceiveEnabled       = TRUE;
    quicSettings.ServerResumptionLevel        = QUIC_SERVER_NO_RESUME;
    quicSettings.StreamRecvWindowUnidiDefault =
      NETWORK_STREAM_RECV_WINDOW;

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
      if (connections.empty() && !activeCallbacks && !activePins &&
          drainedEvent)
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

bool CNetworkServer::ClipboardPeerOwnsInput(
  const NetworkClipboardPeer& peer)
{
  return m_impl && m_impl->ClipboardPeerOwnsInput(peer);
}

bool CNetworkServer::ClipboardWithInputAuthority(
  const NetworkClipboardPeer& peer, NetworkClipboardInputAction action,
  void * opaque)
{
  return m_impl &&
    m_impl->ClipboardWithInputAuthority(peer, action, opaque);
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
  std::shared_ptr<const std::vector<uint8_t>> payload)
{
  return m_impl ?
    m_impl->SendVideo(peer, delivery, messageType, flags,
      componentEpoch, requestID, std::move(payload)) :
    NetworkVideoSendResult::FAILED;
}
