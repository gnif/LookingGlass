/*
KVMGFX Client - A KVM Client for VGA Passthrough
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

#define _GNU_SOURCE
#include "common/crash.h"
#include "common/debug.h"

#if defined(ENABLE_BACKTRACE)

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <inttypes.h>
#include <limits.h>

#include <link.h>
#include "bfd.inc.h"

struct range
{
  intptr_t start, end;
};

struct crash
{
  char         * exe;
  struct range * ranges;
  int            rangeCount;
  bfd          * fd;
  asection     * section;
  asymbol     ** syms;
  long           symCount;
};

static struct crash crash = {0};

static void load_symbols()
{
  bfd_init();
  crash.fd = bfd_openr(crash.exe, NULL);
  if (!crash.fd)
  {
    DEBUG_ERROR("failed to open '%s'", crash.exe);
    return;
  }

  crash.fd->flags |= BFD_DECOMPRESS;

  char **matching;
  if (!bfd_check_format_matches(crash.fd, bfd_object, &matching))
  {
    DEBUG_ERROR("executable is not a bfd_object");
    return;
  }

  crash.section = bfd_get_section_by_name(crash.fd, ".text");
  if (!crash.section)
  {
    DEBUG_ERROR("failed to find .text section");
    return;
  }

  if ((bfd_get_file_flags(crash.fd) & HAS_SYMS) == 0)
  {
    DEBUG_ERROR("executable '%s' has no symbols", crash.exe);
    return;
  }

  long storage   = bfd_get_symtab_upper_bound(crash.fd);
  crash.syms     = (asymbol **)malloc(storage);
  crash.symCount = bfd_canonicalize_symtab(crash.fd, crash.syms);
  if (crash.symCount < 0)
  {
    DEBUG_ERROR("failed to get the symbol count");
    return;
  }
}

static bool lookup_address(bfd_vma pc, const char ** filename, const char ** function, unsigned int * line, unsigned int * discriminator)
{
  if ((bfd_get_section_flags(crash.fd, crash.section) & SEC_ALLOC) == 0)
    return false;

  bfd_size_type size = bfd_get_section_size(crash.section);
  if (pc >= size)
    return false;

  if (!bfd_find_nearest_line_discriminator(
    crash.fd,
    crash.section,
    crash.syms,
    pc,
    filename,
    function,
    line,
    discriminator
  ))
    return false;

  if (!*filename)
    return false;

  return true;
}

static void cleanup()
{
  if (crash.syms)
    free(crash.syms);

  if (crash.fd)
    bfd_close(crash.fd);

  if (crash.ranges)
    free(crash.ranges);

  if (crash.exe)
    free(crash.exe);
}

static int dl_iterate_phdr_callback(struct dl_phdr_info * info, size_t size, void * data)
{
  // we are not a module, and as such we don't have a name
  if (strlen(info->dlpi_name) != 0)
    return 0;

  size_t ttl = 0;
  for(int i = 0; i < info->dlpi_phnum; ++i)
  {
    const ElfW(Phdr) hdr = info->dlpi_phdr[i];
    if (hdr.p_type == PT_LOAD && (hdr.p_flags & PF_X) == PF_X)
      ttl += hdr.p_memsz;
  }

  crash.ranges = realloc(crash.ranges, sizeof(struct range) * (crash.rangeCount + 1));
  crash.ranges[crash.rangeCount].start = info->dlpi_addr;
  crash.ranges[crash.rangeCount].end   = info->dlpi_addr + ttl;
  ++crash.rangeCount;

  return 0;
}

static void crit_err_hdlr(int sig_num, siginfo_t * info, void * ucontext)
{
  void *             array[50];
  char **            messages;
  int                size, i;

  dl_iterate_phdr(dl_iterate_phdr_callback, NULL);
  load_symbols();

  DEBUG_ERROR("==== FATAL CRASH (" BUILD_VERSION ") ====");
  DEBUG_ERROR("signal %d (%s), address is %p", sig_num, strsignal(sig_num), info->si_addr);

  size     = backtrace(array, 50);
  messages = backtrace_symbols(array, size);

  for (i = 2; i < size && messages != NULL; ++i)
  {
    intptr_t base = -1;
    for(int c = 0; c < crash.rangeCount; ++c)
    {
      if ((intptr_t)array[i] >= crash.ranges[c].start && (intptr_t)array[i] < crash.ranges[c].end)
      {
        base = crash.ranges[c].start + crash.section->vma;
        break;
      }
    }

    if (base != -1)
    {
      const char * filename, * function;
      unsigned int line, discriminator;
      if (lookup_address((intptr_t)array[i] - base, &filename, &function, &line, &discriminator))
      {
        DEBUG_ERROR("[trace]: (%d) %s:%u (%s)", i - 2, filename, line, function);
        continue;
      }
    }

    DEBUG_ERROR("[trace]: (%d) %s", i - 2, messages[i]);
  }

  free(messages);
  cleanup();
  exit(EXIT_FAILURE);
}

bool installCrashHandler(const char * exe)
{
  struct sigaction sigact = { 0 };

  crash.exe = realpath(exe, NULL);
  sigact.sa_sigaction = crit_err_hdlr;
  sigact.sa_flags = SA_RESTART | SA_SIGINFO;

  if (sigaction(SIGSEGV, &sigact, (struct sigaction *)NULL) != 0)
  {
    DEBUG_ERROR("Error setting signal handler for %d (%s)", SIGSEGV, strsignal(SIGSEGV));
    return false;
  }

  return true;
}

#else //ENABLE_BACKTRACE

bool installCrashHandler(const char * exe)
{
  return true;
}

#endif