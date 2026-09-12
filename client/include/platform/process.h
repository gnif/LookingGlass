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

#ifndef _H_LG_PLATFORM_PROCESS_
#define _H_LG_PLATFORM_PROCESS_

#include <stdbool.h>

enum LG_ProcessPrivilege
{
  LG_PROCESS_PRIVILEGE_NORMAL,
  LG_PROCESS_PRIVILEGE_ROOT,
  LG_PROCESS_PRIVILEGE_SETUID
};

/* Return true when the request was handled. Returning false asks the platform
 * to continue with its default forced-termination behavior. */
typedef bool (*LG_ProcessStopHandler)(void * opaque);

enum LG_ProcessPrivilege lgProcessGetPrivilege(void);
char * lgProcessGetExecutablePath(const char * argv0);

bool lgProcessInstallStopHandler(LG_ProcessStopHandler handler, void * opaque);
void lgProcessRemoveStopHandler(void);

int lgClientMain(int argc, char * argv[]);

#endif
