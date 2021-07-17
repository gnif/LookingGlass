/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include <windows.h>

typedef NTSTATUS (__stdcall *ZwSetTimerResolution_t)(ULONG RequestedResolution,
    BOOLEAN Set, PULONG ActualResolution);
extern ZwSetTimerResolution_t ZwSetTimerResolution;

typedef NTSTATUS (__stdcall *NtDelayExecution_t)(BOOL Alertable,
    PLARGE_INTEGER DelayInterval);
extern NtDelayExecution_t NtDelayExecution;

void delayInit(void);

// like sleep but more accurate
void delayExecution(float ms);
