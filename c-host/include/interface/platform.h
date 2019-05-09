/*
Looking Glass - KVM FrameRelay (KVMFR) Client
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

#pragma once

#include <stdbool.h>

int  app_main(int argc, char * argv[]);
bool app_init();
void app_quit();

// these must be implemented for each OS
const char * os_getExecutable();

unsigned int os_shmemSize();
bool         os_shmemMmap(void **ptr);
void         os_shmemUnmap();

// os specific thread functions

typedef struct osThreadHandle osThreadHandle;
typedef int (*osThreadFunction)(void * opaque);

bool         os_createThread(const char * name, osThreadFunction function, void * opaque, osThreadHandle ** handle);
bool         os_joinThread  (osThreadHandle * handle, int * resultCode);

// os specific event functions

#define TIMEOUT_INFINITE ((unsigned int)~0)

typedef struct osEventHandle osEventHandle;

osEventHandle * os_createEvent(bool autoReset);
void            os_freeEvent  (osEventHandle * handle);
bool            os_waitEvent  (osEventHandle * handle, unsigned int timeout);
bool            os_waitEvents (osEventHandle * handles[], int count, bool waitAll, unsigned int timeout);
bool            os_signalEvent(osEventHandle * handle);
bool            os_resetEvent (osEventHandle * handle);