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
#include <stdint.h>

typedef struct PortholeDev *PortholeDev;
typedef int    PortholeID;
typedef enum   PortholeState
{
  PH_STATE_NEW_SESSION,
  PH_STATE_CONNECTED,
  PH_STATE_DISCONNECTED
}
PortholeState;

/**
 * Open the porthole device
 *
 * @param  handle    The returned handle if successful, otherwise undefined
 * @param  vendor_id The subsystem vendor and device id to match
 * @return true on success
 *
 * If successful the handle must be closed to free resources when finished.
 */
bool porthole_dev_open(PortholeDev *handle, const uint32_t vendor_id);

/**
 * Close the porthole devce
 *
 * @param handle The porthole device handle obtained from porthole_dev_open
 *
 * handle will be set to NULL and is no longer valid after calling this function.
 */
void porthole_dev_close(PortholeDev *handle);

/**
 * Get the state of the porthole device
 *
 * @param  handle The porthole device handle obtained form porthole_dev_open
 * @return The current state of the connection
 *
 * This method will return the current state of the porthole device
 *
 *   * PH_STATE_NEW_SESSION  = The client has connected
 *   * PH_STATE_CONNECTED    = The client is still connected
 *   * PH_STATE_DISCONNECTED = There is no client connection
 */
PortholeState porthole_dev_get_state(PortholeDev handle);

/**
 * Wait for the specified state
 *
 * @param  handle  The porthole device handle obtained from porthole_dev_open
 * @param  state   The state to wait for
 * @param  timeout The maximum amount of time to wait in milliseconds for the state (0 = infinite)
 * @return true on success, false on timeout
 */
bool porthole_dev_wait_state(PortholeDev handle, const PortholeState state, const unsigned int timeout);

/**
 * Share the provided buffer over the porthole device
 *
 * @param  handle The porthole device
 * @param  type   The type
 * @param  buffer The buffer to share
 * @param  size   The size of the buffer
 * @return the porthole mapping ID, or -1 on failure
 *
 * This function locks the supplied buffer in RAM via the porthole device
 * driver and is then shared with the device for use outside the guest.
 *
 * The type parameter is application defined and is sent along with the buffer
 * to the client application for buffer type identification.
 *
 * If successful the byffer must be unlocked with `porthole_dev_unlock` before
 * the buffer can be freed.
 *
 * This is an expensive operation, the idea is that you allocate fixed buffers
 * and share them with the host at initialization.
 *
 * @note the device & driver are hard limited to 32 shares.
 */
PortholeID porthole_dev_map(PortholeDev handle, const uint32_t type, void *buffer, size_t size);

/**
 * Unmap a previously shared buffer
 *
 * @param  handle The porthole device
 * @param  id     The porthole map id returned by porthole_dev_share
 * @return true on success
 *
 * Unmaps a previously shared buffer. Once this has been done the buffer can
 * be freed or re-used. The client application should no longer attempt to
 * access this buffer as it may be paged out of RAM.
 *
 * Note that this is not strictly required as closing the device will cause
 * the driver to cleanup any prior locked buffers.
 *
 * The client application will be notified that the mapping is about to become
 * invalid and is expected to clean up and notify when it is done. If your
 * application hangs calling this method the issue is very likely with your
 * client application.
 */
bool porthole_dev_unmap(PortholeDev handle, PortholeID id);