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

#ifndef _H_LG_CLIENT_AUDIO_
#define _H_LG_CLIENT_AUDIO_

#include "interface/audio.h"

#if ENABLE_AUDIO

void lgAudio_init(void);
void lgAudio_free(void);

void lgAudio_setFallback(const LG_AudioOps * ops, void * opaque);
void lgAudio_setTransport(const LG_AudioOps * ops, void * opaque);
void lgAudio_dropTransport(void);

bool lgAudio_supportsPlayback(void);
bool lgAudio_supportsRecord(void);
void lgAudio_recordToggleKeybind(int sc, void * opaque);

#else

static inline void lgAudio_init(void) {}
static inline void lgAudio_free(void) {}
static inline void lgAudio_setFallback(
    const LG_AudioOps * ops, void * opaque) {}
static inline void lgAudio_setTransport(
    const LG_AudioOps * ops, void * opaque) {}
static inline void lgAudio_dropTransport(void) {}
static inline bool lgAudio_supportsPlayback(void) { return false; }
static inline bool lgAudio_supportsRecord(void) { return false; }
static inline void lgAudio_recordToggleKeybind(int sc, void * opaque) {}

#endif

#endif
