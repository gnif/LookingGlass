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

#include "usbredir.h"

#include "common/debug.h"

#include <usbredirparser.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define USB_REDIR_CHANNEL_COUNT 256

struct LG_USBRedir
{
  const LG_USBRedirDeviceOps * deviceOps;
  void                       * deviceOpaque;
  LG_USBRedirStatusFn          status;
  void                       * statusOpaque;

  PSUSBRedirChannel * channels[USB_REDIR_CHANNEL_COUNT];
  PSUSBRedirChannel * channel;
  struct usbredirparser * parser;

  const uint8_t * readData;
  size_t          readSize;
  size_t          readOffset;

  atomic_bool desiredPlugged;
  atomic_bool available;
  bool        plugged;
  bool        disconnectPending;
};

static void setAvailable(LG_USBRedir * usbredir, bool available)
{
  const bool previous = atomic_exchange_explicit(
      &usbredir->available, available, memory_order_acq_rel);
  if (previous != available && usbredir->status)
    usbredir->status(usbredir->statusOpaque, available);
}

static int readUSBRedir(void * opaque, uint8_t * data, int count)
{
  LG_USBRedir * usbredir = opaque;
  const size_t remaining = usbredir->readSize - usbredir->readOffset;
  if (remaining < (size_t)count)
    count = (int)remaining;

  if (!count)
    return 0;

  memcpy(data, usbredir->readData + usbredir->readOffset, count);
  usbredir->readOffset += count;
  return count;
}

static int writeUSBRedir(void * opaque, uint8_t * data, int count)
{
  LG_USBRedir * usbredir = opaque;
  if (!usbredir->channel ||
      !purespice_usbRedirWrite(usbredir->channel, data, count))
    return -1;

  return count;
}

static void logUSBRedir(void * opaque, int level, const char * message)
{
  (void)opaque;
  switch (level)
  {
    case usbredirparser_error:
      DEBUG_ERROR("USB redirection: %s", message);
      break;

    case usbredirparser_warning:
      DEBUG_WARN("USB redirection: %s", message);
      break;

    case usbredirparser_info:
      DEBUG_INFO("USB redirection: %s", message);
      break;

    case usbredirparser_debug:
    case usbredirparser_debug_data:
      DEBUG_TRACE("USB redirection: %s", message);
      break;

    case usbredirparser_none:
      break;
  }
}

static void helloUSBRedir(void * opaque,
    struct usb_redir_hello_header * hello)
{
  (void)hello;
  LG_USBRedir * usbredir = opaque;
  setAvailable(usbredir, true);
}

static void deviceDisconnectAck(void * opaque)
{
  LG_USBRedir * usbredir = opaque;
  usbredir->disconnectPending = false;
}

static void unplugDevice(LG_USBRedir * usbredir)
{
  if (!usbredir->plugged)
    return;

  usbredir->deviceOps->unplug(usbredir->deviceOpaque);
  usbredir->plugged = false;
}

static void destroyParser(LG_USBRedir * usbredir)
{
  setAvailable(usbredir, false);
  unplugDevice(usbredir);
  usbredir->disconnectPending = false;

  if (!usbredir->parser)
    return;

  usbredirparser_destroy(usbredir->parser);
  usbredir->parser = NULL;
}

static bool flushUSBRedir(LG_USBRedir * usbredir)
{
  return !usbredir->parser ||
    usbredirparser_do_write(usbredir->parser) == 0;
}

static bool connectChannel(LG_USBRedir * usbredir,
    PSUSBRedirChannel * channel)
{
  usbredir->channel = channel;
  if (purespice_usbRedirConnect(channel))
    return true;

  DEBUG_WARN("Failed to connect USB redirection channel %u",
      purespice_usbRedirChannelId(channel));
  usbredir->channel = NULL;
  return false;
}

static void selectChannel(LG_USBRedir * usbredir)
{
  if (usbredir->channel)
    return;

  for (unsigned int i = 0; i < USB_REDIR_CHANNEL_COUNT; ++i)
  {
    PSUSBRedirChannel * channel = usbredir->channels[i];
    if (!channel || !purespice_usbRedirAvailable(channel))
      continue;

    if (connectChannel(usbredir, channel))
      return;
  }
}

static bool createParser(LG_USBRedir * usbredir)
{
  if (usbredir->parser)
    return true;

  usbredir->parser = usbredirparser_create();
  if (!usbredir->parser)
  {
    DEBUG_ERROR("Failed to create the USB redirection parser");
    return false;
  }

  usbredir->deviceOps->setup(usbredir->deviceOpaque, usbredir->parser);
  usbredir->parser->priv                       = usbredir;
  usbredir->parser->log_func                   = logUSBRedir;
  usbredir->parser->read_func                  = readUSBRedir;
  usbredir->parser->write_func                 = writeUSBRedir;
  usbredir->parser->hello_func                 = helloUSBRedir;
  usbredir->parser->device_disconnect_ack_func = deviceDisconnectAck;

  uint32_t caps[USB_REDIR_CAPS_SIZE] = { 0 };
  usbredirparser_caps_set_cap(caps,
      usb_redir_cap_device_disconnect_ack);
  usbredirparser_caps_set_cap(caps,
      usb_redir_cap_connect_device_version);
  usbredirparser_caps_set_cap(caps,
      usb_redir_cap_ep_info_max_packet_size);
  usbredirparser_caps_set_cap(caps, usb_redir_cap_64bits_ids);
  usbredirparser_caps_set_cap(caps, usb_redir_cap_32bits_bulk_length);
  usbredirparser_init(usbredir->parser, "Looking Glass", caps,
      USB_REDIR_CAPS_SIZE, usbredirparser_fl_usb_host);

  if (flushUSBRedir(usbredir))
    return true;

  DEBUG_ERROR("Failed to send the USB redirection hello packet");
  destroyParser(usbredir);
  return false;
}

LG_USBRedir * lgUsbRedir_create(
    const LG_USBRedirDeviceOps * deviceOps, void * deviceOpaque,
    LG_USBRedirStatusFn status, void * statusOpaque)
{
  if (!deviceOps || !deviceOps->setup || !deviceOps->plug ||
      !deviceOps->unplug)
    return NULL;

  LG_USBRedir * usbredir = calloc(1, sizeof(*usbredir));
  if (!usbredir)
    return NULL;

  usbredir->deviceOps      = deviceOps;
  usbredir->deviceOpaque   = deviceOpaque;
  usbredir->status         = status;
  usbredir->statusOpaque   = statusOpaque;
  atomic_init(&usbredir->desiredPlugged, false);
  atomic_init(&usbredir->available, false);
  return usbredir;
}

void lgUsbRedir_destroy(LG_USBRedir * usbredir)
{
  if (!usbredir)
    return;

  destroyParser(usbredir);
  free(usbredir);
}

void lgUsbRedir_setPlugged(LG_USBRedir * usbredir, bool plugged)
{
  atomic_store_explicit(&usbredir->desiredPlugged, plugged,
      memory_order_release);
}

bool lgUsbRedir_available(const LG_USBRedir * usbredir)
{
  return atomic_load_explicit(&usbredir->available, memory_order_acquire);
}

bool lgUsbRedir_disconnectPending(const LG_USBRedir * usbredir)
{
  return usbredir->disconnectPending;
}

bool lgUsbRedir_process(LG_USBRedir * usbredir)
{
  if (!usbredir->parser ||
      !atomic_load_explicit(&usbredir->available, memory_order_acquire))
    return flushUSBRedir(usbredir);

  if (usbredir->disconnectPending)
    return flushUSBRedir(usbredir);

  const bool desired = atomic_load_explicit(&usbredir->desiredPlugged,
      memory_order_acquire);
  if (desired != usbredir->plugged)
  {
    if (desired)
    {
      usbredir->deviceOps->plug(
          usbredir->deviceOpaque, usbredir->parser);
      usbredir->plugged = true;
    }
    else
    {
      unplugDevice(usbredir);
      usbredir->disconnectPending = usbredirparser_peer_has_cap(
          usbredir->parser, usb_redir_cap_device_disconnect_ack);
      usbredirparser_send_device_disconnect(usbredir->parser);
    }
  }

  return flushUSBRedir(usbredir);
}

void lgUsbRedir_state(PSUSBRedirChannel * channel,
    PSUSBRedirState state, void * opaque)
{
  LG_USBRedir * usbredir = opaque;
  const uint8_t id = purespice_usbRedirChannelId(channel);

  switch (state)
  {
    case PS_USB_REDIR_AVAILABLE:
      usbredir->channels[id] = channel;
      selectChannel(usbredir);
      break;

    case PS_USB_REDIR_CONNECTED:
      if (channel != usbredir->channel)
      {
        purespice_usbRedirDisconnect(channel);
        break;
      }

      if (!createParser(usbredir))
        purespice_usbRedirDisconnect(channel);
      break;

    case PS_USB_REDIR_DISCONNECTED:
      if (channel == usbredir->channel)
        destroyParser(usbredir);
      break;

    case PS_USB_REDIR_UNAVAILABLE:
      if (usbredir->channels[id] == channel)
        usbredir->channels[id] = NULL;

      if (channel == usbredir->channel)
      {
        destroyParser(usbredir);
        usbredir->channel = NULL;
        selectChannel(usbredir);
      }
      break;
  }
}

bool lgUsbRedir_data(PSUSBRedirChannel * channel,
    const uint8_t * data, size_t size, void * opaque)
{
  LG_USBRedir * usbredir = opaque;
  if (channel != usbredir->channel || !usbredir->parser ||
      usbredir->readData)
    return false;

  usbredir->readData   = data;
  usbredir->readSize   = size;
  usbredir->readOffset = 0;

  const int result = usbredirparser_do_read(usbredir->parser);
  const bool consumed = usbredir->readOffset == usbredir->readSize;
  usbredir->readData   = NULL;
  usbredir->readSize   = 0;
  usbredir->readOffset = 0;

  if (result == usbredirparser_read_parse_error)
  {
    DEBUG_ERROR("Invalid USB redirection packet");
    return false;
  }

  if (!consumed)
  {
    DEBUG_ERROR("USB redirection parser did not consume its input");
    return false;
  }

  return lgUsbRedir_process(usbredir);
}

void * lgUsbRedir_device(void * parserOpaque)
{
  LG_USBRedir * usbredir = parserOpaque;
  return usbredir->deviceOpaque;
}
