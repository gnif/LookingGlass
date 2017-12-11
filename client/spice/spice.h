/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
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

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>

bool spice_connect(const char * host, const short port, const char * password);
void spice_disconnect();
bool spice_process();
bool spice_ready();

bool spice_key_down      (uint32_t code);
bool spice_key_up        (uint32_t code);
bool spice_mouse_mode    (bool     server);
bool spice_mouse_position(uint32_t x, uint32_t y);
bool spice_mouse_motion  ( int32_t x,  int32_t y);
bool spice_mouse_press   (uint32_t button);
bool spice_mouse_release (uint32_t button);