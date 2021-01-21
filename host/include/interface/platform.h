/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
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

#pragma once

#include <stdbool.h>

// exit code for user opted to exit looking-glass-host
#define LG_HOST_EXIT_USER    0x10
// exit code for capture errors that should result in a restart, e.g. UAC
#define LG_HOST_EXIT_CAPTURE 0x20
// exit code for terminated
#define LG_HOST_EXIT_KILLED  0x30
// exit code for failed to start
#define LG_HOST_EXIT_FAILED  0x40

int  app_main(int argc, char * argv[]);
bool app_init();
void app_shutdown();
void app_quit();

// these must be implemented for each OS
const char * os_getExecutable();
const char * os_getDataPath();
