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

#include "common/crash.h"
#include "common/debug.h"
#include "common/version.h"

#if defined(ENABLE_BACKTRACE)

#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <elfutils/libdwfl.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#include <inttypes.h>
#define CRASH_MAX_FRAMES 64

struct CrashRequest
{
  int       signal;
  uintptr_t faultAddress;
  unsigned  frameCount;
  uintptr_t frames[CRASH_MAX_FRAMES];
};

struct CrashState
{
  pid_t reporter;
  int   requestFd;
  int   responseFd;
};

static struct CrashState crash =
{
  .reporter   = -1,
  .requestFd  = -1,
  .responseFd = -1
};

static bool writeAll(int fd, const void * buffer, size_t size)
{
  const uint8_t * pos = buffer;
  while (size)
  {
    ssize_t written = write(fd, pos, size);
    if (written < 0)
    {
      if (errno == EINTR)
        continue;
      return false;
    }

    pos  += written;
    size -= written;
  }

  return true;
}

static bool readAll(int fd, void * buffer, size_t size)
{
  uint8_t * pos = buffer;
  while (size)
  {
    ssize_t received = read(fd, pos, size);
    if (received == 0)
      return false;

    if (received < 0)
    {
      if (errno == EINTR)
        continue;
      return false;
    }

    pos  += received;
    size -= received;
  }

  return true;
}

static void reportFrame(Dwfl * dwfl, unsigned index, uintptr_t pc)
{
  // Except for the faulting instruction, an IP is a return address. Looking
  // up the preceding byte attributes it to the call instruction.
  Dwarf_Addr address = pc - (index != 0 && pc != 0);
  Dwfl_Module * module = dwfl_addrmodule(dwfl, address);
  if (!module)
  {
    DEBUG_ERROR("[trace]: (%u) 0x%" PRIxPTR, index, pc);
    return;
  }

  GElf_Off functionOffset = 0;
  GElf_Sym symbol;
  const char * function = dwfl_module_addrinfo(module, address,
    &functionOffset, &symbol, NULL, NULL, NULL);
  Dwfl_Line * source = dwfl_module_getsrc(module, address);
  if (source)
  {
    int line = 0;
    int column = 0;
    const char * filename =
      dwfl_lineinfo(source, NULL, &line, &column, NULL, NULL);
    if (filename)
    {
      DEBUG_ERROR("[trace]: (%u) %s:%d:%d (%s)", index, filename, line,
        column, function ? function : "unknown");
      return;
    }
  }

  Dwarf_Addr moduleStart = 0;
  const char * moduleName = dwfl_module_info(module, NULL, &moduleStart,
    NULL, NULL, NULL, NULL, NULL);
  if (!moduleName)
    moduleName = "unknown";

  if (function)
    DEBUG_ERROR("[trace]: (%u) %s+0x%" PRIx64 " (%s)", index, function,
      (uint64_t)functionOffset, moduleName);
  else
    DEBUG_ERROR("[trace]: (%u) %s+0x%" PRIx64 " [0x%" PRIxPTR "]", index,
      moduleName, (uint64_t)(address - moduleStart), pc);
}

static void reportCrash(pid_t parent, const struct CrashRequest * request)
{
  if (request->signal)
  {
    DEBUG_ERROR("==== FATAL CRASH (%s) ====", BUILD_VERSION);
    DEBUG_ERROR("signal %d (%s), address is 0x%" PRIxPTR, request->signal,
      strsignal(request->signal), request->faultAddress);
  }

  static const Dwfl_Callbacks callbacks =
  {
    .find_elf       = dwfl_linux_proc_find_elf,
    .find_debuginfo = dwfl_standard_find_debuginfo
  };

  Dwfl * dwfl = dwfl_begin(&callbacks);
  if (!dwfl)
  {
    DEBUG_ERROR("Failed to initialize stack trace symbolizer: %s",
      dwfl_errmsg(-1));
    goto raw;
  }

  if (dwfl_linux_proc_report(dwfl, parent) != 0 ||
      dwfl_report_end(dwfl, NULL, NULL) != 0)
  {
    DEBUG_ERROR("Failed to inspect the crashed process: %s",
      dwfl_errmsg(-1));
    dwfl_end(dwfl);
    goto raw;
  }

  for (unsigned i = 0; i < request->frameCount; ++i)
    reportFrame(dwfl, i, request->frames[i]);

  dwfl_end(dwfl);
  return;

raw:
  for (unsigned i = 0; i < request->frameCount; ++i)
    DEBUG_ERROR("[trace]: (%u) 0x%" PRIxPTR, i, request->frames[i]);
}

static void reporterLoop(pid_t parent, int requestFd, int responseFd)
{
  // Do not leave an orphaned reporter if the client exits unexpectedly before
  // it closes the request pipe.
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  if (getppid() != parent)
    return;

  const uint8_t ready = 1;
  if (!writeAll(responseFd, &ready, sizeof(ready)))
    return;

  struct CrashRequest request;
  while (readAll(requestFd, &request, sizeof(request)))
  {
    reportCrash(parent, &request);

    const uint8_t complete = 1;
    if (!writeAll(responseFd, &complete, sizeof(complete)))
      return;
  }
}

static bool startReporter(void)
{
  int requestPipe[2];
  int responsePipe[2];
  if (pipe(requestPipe) != 0)
  {
    DEBUG_ERROR("Failed to create crash reporter request pipe");
    return false;
  }

  if (pipe(responsePipe) != 0)
  {
    DEBUG_ERROR("Failed to create crash reporter response pipe");
    close(requestPipe[0]);
    close(requestPipe[1]);
    return false;
  }

  for (unsigned i = 0; i < 2; ++i)
  {
    fcntl(requestPipe[i], F_SETFD, FD_CLOEXEC);
    fcntl(responsePipe[i], F_SETFD, FD_CLOEXEC);
  }

  const pid_t parent = getpid();
  const pid_t child = fork();
  if (child < 0)
  {
    DEBUG_ERROR("Failed to start the crash reporter");
    close(requestPipe [0]);
    close(requestPipe [1]);
    close(responsePipe[0]);
    close(responsePipe[1]);
    return false;
  }

  if (child == 0)
  {
    close(requestPipe [1]);
    close(responsePipe[0]);
    reporterLoop(parent, requestPipe[0], responsePipe[1]);
    close(requestPipe [0]);
    close(responsePipe[1]);
    _exit(0);
  }

  close(requestPipe [0]);
  close(responsePipe[1]);

  crash.reporter   = child;
  crash.requestFd  = requestPipe [1];
  crash.responseFd = responsePipe[0];

  uint8_t ready;
  if (!readAll(crash.responseFd, &ready, sizeof(ready)))
  {
    DEBUG_ERROR("Crash reporter failed to initialize");
    cleanupCrashHandler();
    return false;
  }

  return true;
}

void cleanupCrashHandler(void)
{
  if (crash.requestFd >= 0)
  {
    close(crash.requestFd);
    crash.requestFd = -1;
  }

  if (crash.responseFd >= 0)
  {
    close(crash.responseFd);
    crash.responseFd = -1;
  }

  if (crash.reporter > 0)
  {
    while (waitpid(crash.reporter, NULL, 0) < 0 && errno == EINTR)
      continue;
    crash.reporter = -1;
  }
}

void printBacktrace(void)
{
  struct CrashRequest request =
  {
    .signal = 0
  };

  unw_context_t context;
  unw_cursor_t cursor;
  if (unw_getcontext(&context) < 0 ||
      unw_init_local(&cursor, &context) < 0)
    return;

  // Omit unw_getcontext and printBacktrace itself.
  if (unw_step(&cursor) <= 0 || unw_step(&cursor) <= 0)
    return;

  do
  {
    unw_word_t pc;
    if (unw_get_reg(&cursor, UNW_REG_IP, &pc) < 0 || !pc)
      break;

    request.frames[request.frameCount++] = (uintptr_t)pc;
  }
  while (request.frameCount < CRASH_MAX_FRAMES && unw_step(&cursor) > 0);

  if (crash.requestFd >= 0 && crash.responseFd >= 0 &&
      writeAll(crash.requestFd, &request, sizeof(request)))
  {
    uint8_t complete;
    readAll(crash.responseFd, &complete, sizeof(complete));
  }
}

static void crit_err_hdlr(int sig_num, siginfo_t * info, void * ucontext)
{
  struct CrashRequest request;
  request.signal       = sig_num;
  request.faultAddress = (uintptr_t)info->si_addr;
  request.frameCount   = 0;

  unw_cursor_t cursor;
  if (unw_init_local2(&cursor, (unw_context_t *)ucontext,
        UNW_INIT_SIGNAL_FRAME) >= 0)
  {
    do
    {
      unw_word_t pc;
      if (unw_get_reg(&cursor, UNW_REG_IP, &pc) < 0 || !pc)
        break;

      request.frames[request.frameCount++] = (uintptr_t)pc;
    }
    while (request.frameCount < CRASH_MAX_FRAMES &&
      unw_step(&cursor) > 0);
  }

  if (crash.requestFd >= 0 && crash.responseFd >= 0 &&
      writeAll(crash.requestFd, &request, sizeof(request)))
  {
    uint8_t complete;
    readAll(crash.responseFd, &complete, sizeof(complete));
  }

  // SA_RESETHAND has restored the default disposition and SA_NODEFER leaves
  // this signal unblocked, so this preserves the original fatal signal and
  // still permits the operating system to produce a core dump.
  kill(getpid(), sig_num);
  _exit(128 + sig_num);
}

bool installCrashHandler(const char * exe)
{
  (void)exe;

  if (!startReporter())
    return false;

  struct sigaction sigact = { 0 };
  sigact.sa_sigaction = crit_err_hdlr;
  sigact.sa_flags =
    SA_RESTART | SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
  sigemptyset(&sigact.sa_mask);

  if (sigaction(SIGSEGV, &sigact, NULL) != 0)
  {
    DEBUG_ERROR("Error setting SIGSEGV handler");
    cleanupCrashHandler();
    return false;
  }

  return true;
}

#else //ENABLE_BACKTRACE

bool installCrashHandler(const char * exe)
{
  return true;
}

void cleanupCrashHandler(void)
{
}

void printBacktrace(void)
{
}

#endif
