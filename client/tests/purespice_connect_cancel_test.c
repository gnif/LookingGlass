/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "test.h"

#include <purespice.h>
#include "ps.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define ACCEPT_TIMEOUT_MS 2000U
#define CANCEL_BOUND_MS    500U

struct StallPeer
{
  int         listenFd;
  atomic_bool accepted;
  atomic_bool stop;
};

struct ConnectTask
{
  PSConfig    config;
  atomic_bool done;
  bool        result;
};

struct ChannelConnectTask
{
  atomic_bool done;
  bool        result;
};

struct PublishGate
{
  atomic_bool entered;
  atomic_bool release;
};

static void quietLog(const char * file, unsigned int line,
    const char * function, const char * format, ...)
{
  (void)file;
  (void)line;
  (void)function;
  (void)format;
}

static uint64_t monotonicMs(void)
{
  struct timespec time;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
  return (uint64_t)time.tv_sec * UINT64_C(1000) +
    (uint64_t)time.tv_nsec / UINT64_C(1000000);
}

static void * stallPeerThread(void * opaque)
{
  struct StallPeer * peer = opaque;
  const int client = accept(peer->listenFd, NULL, NULL);
  if (client < 0)
    return NULL;

  atomic_store_explicit(&peer->accepted, true, memory_order_release);
  while (!atomic_load_explicit(&peer->stop, memory_order_acquire))
    usleep(1000);

  close(client);
  return NULL;
}

static void * connectThread(void * opaque)
{
  struct ConnectTask * task = opaque;
  task->result = purespice_connect(&task->config);
  atomic_store_explicit(&task->done, true, memory_order_release);
  return NULL;
}

static void * channelConnectThread(void * opaque)
{
  struct ChannelConnectTask * task = opaque;
  task->result = purespice_connectChannel(PS_CHANNEL_DISPLAY);
  atomic_store_explicit(&task->done, true, memory_order_release);
  return NULL;
}

static void connectPublished(void * opaque)
{
  struct PublishGate * gate = opaque;
  atomic_store_explicit(&gate->entered, true, memory_order_release);
  while (!atomic_load_explicit(&gate->release, memory_order_acquire))
    usleep(1000);
}

static void waitFor(atomic_bool * value, unsigned int timeoutMs)
{
  const uint64_t deadline = monotonicMs() + timeoutMs;
  while (!atomic_load_explicit(value, memory_order_acquire) &&
      monotonicMs() < deadline)
    usleep(1000);
  CHECK(atomic_load_explicit(value, memory_order_acquire));
}

static void testStalledHandshake(void)
{
  const PSInit init =
  {
    .log =
    {
      .info  = quietLog,
      .warn  = quietLog,
      .error = quietLog,
    },
  };
  purespice_init(&init);

  const int listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(listenFd >= 0);

  struct sockaddr_un address =
  {
    .sun_family = AF_UNIX,
  };
  const int pathLength = snprintf(address.sun_path,
      sizeof(address.sun_path), "/tmp/lg-ps-cancel-%ld", (long)getpid());
  CHECK(pathLength > 0 && (size_t)pathLength < sizeof(address.sun_path));
  (void)unlink(address.sun_path);
  const socklen_t addressSize = sizeof(address);
  CHECK(bind(listenFd, (const struct sockaddr *)&address,
        addressSize) == 0);
  CHECK(listen(listenFd, 1) == 0);

  struct StallPeer peer =
  {
    .listenFd = listenFd,
  };
  atomic_init(&peer.accepted, false);
  atomic_init(&peer.stop, false);

  pthread_t peerThread;
  CHECK(pthread_create(&peerThread, NULL, stallPeerThread, &peer) == 0);

  struct ConnectTask connect =
  {
    .config =
    {
      .host     = address.sun_path,
      .port     = 0,
      .password = "",
    },
  };
  atomic_init(&connect.done, false);

  purespice_beginConnect();
  pthread_t connectThreadId;
  CHECK(pthread_create(
        &connectThreadId, NULL, connectThread, &connect) == 0);
  waitFor(&peer.accepted, ACCEPT_TIMEOUT_MS);

  const uint64_t start = monotonicMs();
  purespice_disconnect();
  const uint64_t disconnectElapsed = monotonicMs() - start;

  waitFor(&connect.done, CANCEL_BOUND_MS);
  const uint64_t elapsed = monotonicMs() - start;
  CHECK(pthread_join(connectThreadId, NULL) == 0);
  CHECK(!connect.result);
  CHECK(disconnectElapsed < CANCEL_BOUND_MS);
  CHECK(elapsed < CANCEL_BOUND_MS);

  atomic_store_explicit(&peer.stop, true, memory_order_release);
  CHECK(pthread_join(peerThread, NULL) == 0);
  close(listenFd);
  CHECK(unlink(address.sun_path) == 0);
}

static void testCancelBeforeConnect(void)
{
  struct PublishGate gate;
  atomic_init(&gate.entered, false);
  atomic_init(&gate.release, false);
  purespice_testSetConnectPublishedHook(connectPublished, &gate);

  struct ConnectTask connect =
  {
    .config =
    {
      .host     = "/tmp/lg-ps-unreachable",
      .port     = 0,
      .password = "",
    },
  };
  atomic_init(&connect.done, false);
  purespice_beginConnect();

  pthread_t connectThreadId;
  CHECK(pthread_create(
        &connectThreadId, NULL, connectThread, &connect) == 0);
  waitFor(&gate.entered, ACCEPT_TIMEOUT_MS);

  const uint64_t start = monotonicMs();
  purespice_cancelConnect();
  atomic_store_explicit(&gate.release, true, memory_order_release);
  waitFor(&connect.done, CANCEL_BOUND_MS);
  const uint64_t elapsed = monotonicMs() - start;

  CHECK(pthread_join(connectThreadId, NULL) == 0);
  CHECK(!connect.result);
  CHECK(elapsed < CANCEL_BOUND_MS);
  purespice_testSetConnectPublishedHook(NULL, NULL);
}

static void testStalledChannelHandshake(void)
{
  const int listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(listenFd >= 0);

  struct sockaddr_un address =
  {
    .sun_family = AF_UNIX,
  };
  const int pathLength = snprintf(address.sun_path,
      sizeof(address.sun_path), "/tmp/lg-ps-channel-%ld", (long)getpid());
  CHECK(pathLength > 0 && (size_t)pathLength < sizeof(address.sun_path));
  (void)unlink(address.sun_path);
  CHECK(bind(listenFd, (const struct sockaddr *)&address,
        sizeof(address)) == 0);
  CHECK(listen(listenFd, 1) == 0);

  struct StallPeer peer =
  {
    .listenFd = listenFd,
  };
  atomic_init(&peer.accepted, false);
  atomic_init(&peer.stop, false);
  pthread_t peerThread;
  CHECK(pthread_create(&peerThread, NULL, stallPeerThread, &peer) == 0);

  g_ps.family = AF_UNIX;
  memset(&g_ps.addr, 0, sizeof(g_ps.addr));
  g_ps.addr.un.sun_family = AF_UNIX;
  CHECK(snprintf(g_ps.addr.un.sun_path, sizeof(g_ps.addr.un.sun_path),
        "%s", address.sun_path) > 0);
  g_ps.epollfd = epoll_create1(0);
  CHECK(g_ps.epollfd >= 0);
  atomic_store_explicit(&g_ps.channels[PS_CHANNEL_DISPLAY].available,
      true, memory_order_release);

  struct ChannelConnectTask connect;
  atomic_init(&connect.done, false);
  connect.result = true;
  purespice_beginConnect();
  purespice_beginChannelConnect(PS_CHANNEL_DISPLAY);
  pthread_t connectThreadId;
  CHECK(pthread_create(&connectThreadId, NULL,
        channelConnectThread, &connect) == 0);
  waitFor(&peer.accepted, ACCEPT_TIMEOUT_MS);

  const uint64_t start = monotonicMs();
  purespice_cancelChannelConnect(PS_CHANNEL_DISPLAY);
  waitFor(&connect.done, CANCEL_BOUND_MS);
  const uint64_t elapsed = monotonicMs() - start;

  CHECK(pthread_join(connectThreadId, NULL) == 0);
  CHECK(!connect.result);
  CHECK(elapsed < CANCEL_BOUND_MS);

  atomic_store_explicit(&peer.stop, true, memory_order_release);
  CHECK(pthread_join(peerThread, NULL) == 0);
  close(listenFd);
  CHECK(unlink(address.sun_path) == 0);
  close(g_ps.epollfd);
  g_ps.epollfd = -1;
  atomic_store_explicit(&g_ps.channels[PS_CHANNEL_DISPLAY].available,
      false, memory_order_release);
}

int main(void)
{
  testStalledHandshake();
  testCancelBeforeConnect();
  testStalledChannelHandshake();
  puts("PureSpice cancellable connect test passed");
  return 0;
}
