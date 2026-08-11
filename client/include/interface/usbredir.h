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

#ifndef _H_LG_CLIENT_USBREDIR_INTERFACE_
#define _H_LG_CLIENT_USBREDIR_INTERFACE_

struct usbredirparser;

typedef struct LG_USBRedirDeviceOps
{
  /* Install the device packet callbacks on a newly-created parser. */
  void (*setup)(void * opaque, struct usbredirparser * parser);

  /* Queue the device descriptors and connection announcement. */
  void (*plug)(void * opaque, struct usbredirparser * parser);

  /* Queue time-sensitive device data immediately before parser output is
   * flushed. */
  void (*process)(void * opaque);

  /* Stop all device activity. The bridge sends the disconnect packet. */
  void (*unplug)(void * opaque);
}
LG_USBRedirDeviceOps;

/* Parser callbacks receive the bridge as their opaque value. */
void * lgUsbRedir_device(void * parserOpaque);

#endif
