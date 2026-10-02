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

#ifndef _H_LG_MSQUIC_MINGW_COMPAT_
#define _H_LG_MSQUIC_MINGW_COMPAT_

#ifndef FORCEINLINE
#define FORCEINLINE __forceinline
#endif

#ifndef EXTERN_C_START
#ifdef __cplusplus
#define EXTERN_C_START extern "C" {
#define EXTERN_C_END   }
#else
#define EXTERN_C_START
#define EXTERN_C_END
#endif
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mswsock.h>
#include <mstcpip.h>

#include <string.h>

#ifndef __cplusplus
#ifndef static_assert
#define static_assert _Static_assert
#endif

#define inline        static __inline__
#endif

#include <msquic_winuser.h>

#ifndef __cplusplus
#undef inline
#endif

#undef QUIC_INLINE
#define QUIC_INLINE static inline

#undef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(parameter) ((void)(parameter))

#undef DBG_UNREFERENCED_LOCAL_VARIABLE
#define DBG_UNREFERENCED_LOCAL_VARIABLE(variable) ((void)(variable))

#ifndef _Interlocked_operand_
#define _Interlocked_operand_
#endif

#ifndef DECLSPEC_ALLOCATOR
#define DECLSPEC_ALLOCATOR
#endif

#ifndef FAST_FAIL_INVALID_REFERENCE_COUNT
#define FAST_FAIL_INVALID_REFERENCE_COUNT 14
#endif

#ifndef PASSIVE_LEVEL
#define PASSIVE_LEVEL 0
#endif

#ifndef IFI_UNSPECIFIED
#define IFI_UNSPECIFIED 0
#endif

#ifndef __kernel_entry
#define __kernel_entry
#endif

#ifndef __noop
#define __noop(expression) ((void)(0 && (expression)))
#endif

#ifndef __annotation
#define __annotation(...) ((void)0)
#endif

#ifndef __analysis_assert
#define __analysis_assert(expression) ((void)(0 && (expression)))
#endif

#undef InterlockedAnd8
#define InterlockedAnd8(destination, value) \
  __atomic_fetch_and((destination), (value), __ATOMIC_SEQ_CST)

#undef InterlockedOr8
#define InterlockedOr8(destination, value)  \
  __atomic_fetch_or((destination), (value), __ATOMIC_SEQ_CST)

#ifndef InterlockedIncrementNoFence64
#define InterlockedIncrementNoFence64 _InterlockedIncrement64
#endif

#ifndef InterlockedDecrementRelease64
#define InterlockedDecrementRelease64 _InterlockedDecrement64
#endif

#ifndef InterlockedCompareExchangeNoFence64
#define InterlockedCompareExchangeNoFence64 _InterlockedCompareExchange64
#endif

#ifndef ReadNoFence64
#define ReadNoFence64(target) (*(volatile __int64 *)(target))
#endif

#ifndef ReadPointerNoFence
#define ReadPointerNoFence(target) (*(void * volatile *)(target))
#endif

#ifndef ReadUInt32Acquire
#define ReadUInt32Acquire(source) \
  __atomic_load_n((source), __ATOMIC_ACQUIRE)
#endif

#ifndef WriteUInt32Release
#define WriteUInt32Release(destination, value) \
  __atomic_store_n((destination), (value), __ATOMIC_RELEASE)
#endif

#ifndef WSA_FLAG_REGISTERED_IO
#define WSA_FLAG_REGISTERED_IO 0x100
#endif

#ifndef SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER
#define SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER _WSAIORW(IOC_WS2, 36)
#endif

#ifndef SIO_QUERY_RSS_PROCESSOR_INFO
#define SIO_QUERY_RSS_PROCESSOR_INFO _WSAIOR(IOC_WS2, 37)

typedef struct _SOCKET_PROCESSOR_AFFINITY
{
  PROCESSOR_NUMBER Processor;
  USHORT           NumaNodeId;
  USHORT           Reserved;
}
SOCKET_PROCESSOR_AFFINITY, *PSOCKET_PROCESSOR_AFFINITY;
#endif

#ifndef SIO_QUERY_RSS_SCALABILITY_INFO
#define SIO_QUERY_RSS_SCALABILITY_INFO _WSAIOR(IOC_VENDOR, 210)

typedef struct _RSS_SCALABILITY_INFO
{
  BOOLEAN RssEnabled;
}
RSS_SCALABILITY_INFO, *PRSS_SCALABILITY_INFO;
#endif

#ifndef GAA_FLAG_SKIP_DNS_INFO
#define GAA_FLAG_SKIP_DNS_INFO 0x0800
#endif

#ifndef WSAID_MULTIPLE_RIO
#define WSAID_MULTIPLE_RIO \
  { 0x8509e081, 0x96dd, 0x4005, \
    { 0xb1, 0x65, 0x9e, 0x2e, 0xe8, 0xc7, 0x9e, 0x3f } }
#endif

#ifndef RIO_INVALID_CQ
typedef struct RIO_BUFFERID_t *RIO_BUFFERID, **PRIO_BUFFERID;
typedef struct RIO_CQ_t       *RIO_CQ,       **PRIO_CQ;
typedef struct RIO_RQ_t       *RIO_RQ,       **PRIO_RQ;

typedef struct _RIORESULT
{
  LONG      Status;
  ULONG     BytesTransferred;
  ULONGLONG SocketContext;
  ULONGLONG RequestContext;
}
RIORESULT, *PRIORESULT;

typedef struct _RIO_BUF
{
  RIO_BUFFERID BufferId;
  ULONG        Offset;
  ULONG        Length;
}
RIO_BUF, *PRIO_BUF;

#define RIO_MSG_DONT_NOTIFY 0x00000001
#define RIO_MSG_DEFER       0x00000002
#define RIO_MSG_WAITALL     0x00000004
#define RIO_MSG_COMMIT_ONLY 0x00000008

#define RIO_INVALID_BUFFERID ((RIO_BUFFERID)(ULONG_PTR)0xFFFFFFFF)
#define RIO_INVALID_CQ       ((RIO_CQ)0)
#define RIO_INVALID_RQ       ((RIO_RQ)0)
#define RIO_MAX_CQ_SIZE      0x08000000
#define RIO_CORRUPT_CQ       0xFFFFFFFF

typedef struct _RIO_CMSG_BUFFER
{
  ULONG TotalLength;
}
RIO_CMSG_BUFFER, *PRIO_CMSG_BUFFER;

#define RIO_CMSG_BASE_SIZE WSA_CMSGHDR_ALIGN(sizeof(RIO_CMSG_BUFFER))

typedef BOOL         (WINAPI *LPFN_RIORECEIVE)(
  RIO_RQ, PRIO_BUF, ULONG, DWORD, PVOID);
typedef BOOL         (WINAPI *LPFN_RIORECEIVEEX)(
  RIO_RQ, PRIO_BUF, ULONG, PRIO_BUF, PRIO_BUF, PRIO_BUF, PRIO_BUF,
  DWORD, PVOID);
typedef BOOL         (WINAPI *LPFN_RIOSEND)(
  RIO_RQ, PRIO_BUF, ULONG, DWORD, PVOID);
typedef BOOL         (WINAPI *LPFN_RIOSENDEX)(
  RIO_RQ, PRIO_BUF, ULONG, PRIO_BUF, PRIO_BUF, PRIO_BUF, PRIO_BUF,
  DWORD, PVOID);
typedef VOID         (WINAPI *LPFN_RIOCLOSECOMPLETIONQUEUE)(RIO_CQ);

typedef enum _RIO_NOTIFICATION_COMPLETION_TYPE
{
  RIO_EVENT_COMPLETION = 1,
  RIO_IOCP_COMPLETION  = 2,
}
RIO_NOTIFICATION_COMPLETION_TYPE, *PRIO_NOTIFICATION_COMPLETION_TYPE;

typedef struct _RIO_NOTIFICATION_COMPLETION
{
  RIO_NOTIFICATION_COMPLETION_TYPE Type;
  union
  {
    struct
    {
      HANDLE EventHandle;
      BOOL   NotifyReset;
    }
    Event;
    struct
    {
      HANDLE IocpHandle;
      PVOID  CompletionKey;
      PVOID  Overlapped;
    }
    Iocp;
  };
}
RIO_NOTIFICATION_COMPLETION, *PRIO_NOTIFICATION_COMPLETION;

typedef RIO_CQ       (WINAPI *LPFN_RIOCREATECOMPLETIONQUEUE)(
  DWORD, PRIO_NOTIFICATION_COMPLETION);
typedef RIO_RQ       (WINAPI *LPFN_RIOCREATEREQUESTQUEUE)(
  SOCKET, ULONG, ULONG, ULONG, ULONG, RIO_CQ, RIO_CQ, PVOID);
typedef ULONG        (WINAPI *LPFN_RIODEQUEUECOMPLETION)(
  RIO_CQ, PRIORESULT, ULONG);
typedef VOID         (WINAPI *LPFN_RIODEREGISTERBUFFER)(RIO_BUFFERID);
typedef INT          (WINAPI *LPFN_RIONOTIFY)(RIO_CQ);
typedef RIO_BUFFERID (WINAPI *LPFN_RIOREGISTERBUFFER)(PCHAR, DWORD);
typedef BOOL         (WINAPI *LPFN_RIORESIZECOMPLETIONQUEUE)(RIO_CQ, DWORD);
typedef BOOL         (WINAPI *LPFN_RIORESIZEREQUESTQUEUE)(RIO_RQ, DWORD, DWORD);

typedef struct _RIO_EXTENSION_FUNCTION_TABLE
{
  DWORD                         cbSize;
  LPFN_RIORECEIVE               RIOReceive;
  LPFN_RIORECEIVEEX             RIOReceiveEx;
  LPFN_RIOSEND                  RIOSend;
  LPFN_RIOSENDEX                RIOSendEx;
  LPFN_RIOCLOSECOMPLETIONQUEUE  RIOCloseCompletionQueue;
  LPFN_RIOCREATECOMPLETIONQUEUE RIOCreateCompletionQueue;
  LPFN_RIOCREATEREQUESTQUEUE    RIOCreateRequestQueue;
  LPFN_RIODEQUEUECOMPLETION     RIODequeueCompletion;
  LPFN_RIODEREGISTERBUFFER      RIODeregisterBuffer;
  LPFN_RIONOTIFY                RIONotify;
  LPFN_RIOREGISTERBUFFER        RIORegisterBuffer;
  LPFN_RIORESIZECOMPLETIONQUEUE RIOResizeCompletionQueue;
  LPFN_RIORESIZEREQUESTQUEUE    RIOResizeRequestQueue;
}
RIO_EXTENSION_FUNCTION_TABLE, *PRIO_EXTENSION_FUNCTION_TABLE;
#endif

#ifndef IN6ADDR_SETV4MAPPED
static inline void lgMsQuicSetV4MappedAddress(
  SOCKADDR_IN6  *address,
  const IN_ADDR *ipv4,
  SCOPE_ID       scope,
  USHORT         port)
{
  memset(address, 0, sizeof(*address));
  address->sin6_family            = AF_INET6;
  address->sin6_port              = port;
  address->sin6_scope_struct      = scope;
  address->sin6_addr.s6_bytes[10] = 0xff;
  address->sin6_addr.s6_bytes[11] = 0xff;
  memcpy(&address->sin6_addr.s6_bytes[12], ipv4, sizeof(*ipv4));
}

#define IN6ADDR_SETV4MAPPED(address, ipv4, scope, port) \
  lgMsQuicSetV4MappedAddress((address), (ipv4), (scope), (port))
#endif

#ifndef IN6_GET_ADDR_V4MAPPED
#define IN6_GET_ADDR_V4MAPPED(address) (&(address)->s6_bytes[12])
#endif

#ifndef IN6ADDR_ISV4MAPPED
#define IN6ADDR_ISV4MAPPED(address) \
  IN6_IS_ADDR_V4MAPPED(&(address)->sin6_addr)
#endif

#ifndef IN4_IS_ADDR_LOOPBACK
#define IN4_IS_ADDR_LOOPBACK(address) \
  (((const unsigned char *)&(address)->s_addr)[0] == 127)
#endif

#ifndef PIN_PKTINFO
typedef IN_PKTINFO *PIN_PKTINFO;
#endif

#ifndef PIN6_PKTINFO
typedef IN6_PKTINFO *PIN6_PKTINFO;
#endif

#endif
