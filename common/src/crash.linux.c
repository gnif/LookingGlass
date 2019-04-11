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

#include "common/crash.h"
#include "common/debug.h"

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

/*
  Large portions of this code comes from @jschmier @ https://stackoverflow.com/a/1925461/637874
*/

/* This structure mirrors the one found in /usr/include/asm/ucontext.h */
typedef struct _sig_ucontext
{
  unsigned long     uc_flags;
  struct ucontext   *uc_link;
  stack_t           uc_stack;
  struct sigcontext uc_mcontext;
  sigset_t          uc_sigmask;
}
sig_ucontext_t;

static void crit_err_hdlr(int sig_num, siginfo_t * info, void * ucontext)
{
  void *             array[50];
  void *             caller_address;
  char **            messages;
  int                size, i;
  sig_ucontext_t *   uc;

  uc = (sig_ucontext_t *)ucontext;

  /* Get the address at the time the signal was raised */
  #if defined(__i386__) // gcc specific
   caller_address = (void *) uc->uc_mcontext.eip; // EIP: x86 specific
  #elif defined(__x86_64__) // gcc specific
   caller_address = (void *) uc->uc_mcontext.rip; // RIP: x86_64 specific
  #else
  #error Unsupported architecture. // TODO: Add support for other arch.
  #endif

  DEBUG_ERROR("signal %d (%s), address is %p from %p", sig_num, strsignal(sig_num), info->si_addr, (void *)caller_address);

  size = backtrace(array, 50);

  /* overwrite sigaction with caller's address */
  array[1] = caller_address;

  messages = backtrace_symbols(array, size);

  /* skip first stack frame (points here) */
  for (i = 1; i < size && messages != NULL; ++i)
    DEBUG_ERROR("[bt]: (%d) %s", i, messages[i]);

  free(messages);

  exit(EXIT_FAILURE);
}

bool installCrashHandler()
{
  struct sigaction sigact;

  sigact.sa_sigaction = crit_err_hdlr;
  sigact.sa_flags = SA_RESTART | SA_SIGINFO;

  if (sigaction(SIGSEGV, &sigact, (struct sigaction *)NULL) != 0)
  {
    DEBUG_ERROR("Error setting signal handler for %d (%s)", SIGSEGV, strsignal(SIGSEGV));
    return false;
  }

  return true;
}