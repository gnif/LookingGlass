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

#include "types.h"

typedef struct PortholeClient *PortholeClient;

/**
 * Memory map event callback.
 *
 * @param type The type ID provided by the guest
 * @param map  The new mapping
 *
 * @note this is called from the socket thread
 */
typedef void (*PortholeMapEvent)(uint32_t type, PortholeMap * map);

/**
 * Memory unmap event callback.
 *
 * @param id The id of the mapping that has been unmapped by the guest
 *
 * @note this is called from the socket thread
 */
typedef void (*PortholeUnmapEvent)(uint32_t id);

/**
 * Unexpected client disconnection event.
 *
 * When this occurs all mappings become invalid and should no longer be used.
 *
 * @note this is called from the socket thread
 */
typedef void (*PortholeDisconEvent)();

/**
 * Open the porthole device
 *
 * @param   handle       Returned handle if successful, otherwise undefined
 * @param   socket_path  Path to the unix socket of the porthole char device
 * @param   map_event    Callback for map events from the guest
 * @param   unmap_event  Callback for unmap events from the guest
 * @param   discon_event Callback for client socket disconnection
 * @returns true on success
 *
 * If successful the handle must be closed to free resources when finished.
 */
bool porthole_client_open(
  PortholeClient    * handle,
  const char        * socket_path,
  PortholeMapEvent    map_cb,
  PortholeUnmapEvent  unmap_cb,
  PortholeDisconEvent discon_cb);

/**
 * Close the porthole devce
 *
 * @param handle The porthole client handle obtained from porthole_client_open
 *
 * handle will be set to NULL and is no longer valid after calling this
 * function and all mappings will become invalid.
 */
void porthole_client_close(PortholeClient * handle);