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

#include "usb_audio.h"

#include "common/debug.h"

#include <usbredirparser.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum
{
  /* Reserved prototype identity for the initial evaluation device. */
  USB_AUDIO_VENDOR_ID       = 0xffff,
  USB_AUDIO_PRODUCT_ID      = 0x0001,
  USB_AUDIO_DEVICE_VERSION  = 0x0100,
  USB_AUDIO_CONFIGURATION   = 1,
  USB_AUDIO_CONTROL_IFACE   = 0,
  USB_AUDIO_STREAM_IFACE    = 1,
  USB_AUDIO_STREAM_ALT      = 1,
  USB_AUDIO_CLOCK_ID        = 1,
  USB_AUDIO_STREAM_ENDPOINT = 0x01,
  USB_AUDIO_MAX_PACKET_SIZE = 392,
  USB_AUDIO_FRAME_SIZE      = 8,
  USB_AUDIO_EP_INTERVAL     = 8,
  USB_AUDIO_FS_INTERVAL_POS = 118,
};

enum
{
  USB_REQUEST_GET_STATUS     = 0x00,
  USB_REQUEST_GET_DESCRIPTOR = 0x06,
  USB_REQUEST_CUR            = 0x01,
  USB_REQUEST_RANGE          = 0x02,
};

enum
{
  USB_REQUEST_TYPE_IN_STANDARD_DEVICE    = 0x80,
  USB_REQUEST_TYPE_IN_STANDARD_INTERFACE = 0x81,
  USB_REQUEST_TYPE_IN_STANDARD_ENDPOINT  = 0x82,
  USB_REQUEST_TYPE_IN_CLASS_INTERFACE    = 0xa1,
};

enum
{
  USB_DESCRIPTOR_DEVICE           = 0x01,
  USB_DESCRIPTOR_CONFIGURATION    = 0x02,
  USB_DESCRIPTOR_STRING           = 0x03,
  USB_DESCRIPTOR_DEVICE_QUALIFIER = 0x06,
  USB_DESCRIPTOR_OTHER_SPEED      = 0x07,
};

enum
{
  USB_AUDIO_CONTROL_FREQUENCY = 0x01,
  USB_AUDIO_CONTROL_VALIDITY  = 0x02,
};

static const uint8_t l_deviceDescriptor[] =
{
  0x12, 0x01, 0x00, 0x02, 0xef, 0x02, 0x01, 0x40,
  0xff, 0xff, 0x01, 0x00, 0x00, 0x01, 0x01, 0x02,
  0x03, 0x01,
};

static const uint8_t l_configurationDescriptor[] =
{
  /* Configuration */
  0x09, 0x02, 0x7f, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,

  /* Audio function interface association */
  0x08, 0x0b, 0x00, 0x02, 0x01, 0x00, 0x20, 0x00,

  /* Audio control interface */
  0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x20, 0x00,
  0x09, 0x24, 0x01, 0x00, 0x02, 0x01, 0x2e, 0x00, 0x00,
  0x08, 0x24, 0x0a, 0x01, 0x01, 0x05, 0x00, 0x00,
  0x11, 0x24, 0x02, 0x02, 0x01, 0x01, 0x00, 0x01, 0x02,
  0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x0c, 0x24, 0x03, 0x03, 0x01, 0x03, 0x00, 0x02, 0x01,
  0x00, 0x00, 0x00,

  /* Audio streaming interface, idle and active alternate settings */
  0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x20, 0x00,
  0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x20, 0x00,
  0x10, 0x24, 0x01, 0x02, 0x00, 0x01, 0x04, 0x00, 0x00,
  0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00,
  0x06, 0x24, 0x02, 0x01, 0x04, 0x20,

  /* High-speed 1 ms adaptive isochronous output endpoint */
  0x07, 0x05, 0x01, 0x09, 0x88, 0x01, 0x04,
  0x08, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t l_deviceQualifierDescriptor[] =
{
  0x0a, 0x06, 0x00, 0x02, 0xef, 0x02, 0x01, 0x40, 0x01, 0x00,
};

static const uint8_t l_clockFrequency[] =
{
  0x80, 0xbb, 0x00, 0x00,
};

static const uint8_t l_clockFrequencyRange[] =
{
  0x01, 0x00,
  0x80, 0xbb, 0x00, 0x00,
  0x80, 0xbb, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
};

static const uint8_t l_clockValid[] = { 0x01 };
static const uint8_t l_zeroStatus[] = { 0x00, 0x00 };

_Static_assert(sizeof(l_deviceDescriptor) == 18,
    "invalid USB device descriptor size");
_Static_assert(sizeof(l_configurationDescriptor) == 127,
    "invalid USB configuration descriptor size");
_Static_assert(sizeof(l_deviceQualifierDescriptor) == 10,
    "invalid USB device qualifier descriptor size");
_Static_assert(USB_AUDIO_FS_INTERVAL_POS <
    sizeof(l_configurationDescriptor), "invalid endpoint interval offset");

struct LG_USBAudio
{
  const LG_USBAudioEventOps * events;
  void                      * eventOpaque;
  struct usbredirparser     * parser;

  uint8_t configuration;
  uint8_t streamAlt;
  bool    streaming;
};

static LG_USBAudio * getAudio(void * opaque)
{
  return lgUsbRedir_device(opaque);
}

static void stopPlayback(LG_USBAudio * audio)
{
  if (!audio->streaming)
    return;

  audio->streaming = false;
  if (audio->events && audio->events->stop)
    audio->events->stop(audio->eventOpaque);
}

static void resetDevice(LG_USBAudio * audio)
{
  stopPlayback(audio);
  audio->configuration = 0;
  audio->streamAlt     = 0;
}

static void sendInterfaceInfo(LG_USBAudio * audio)
{
  struct usb_redir_interface_info_header info =
  {
    .interface_count    = 2,
    .interface          = { USB_AUDIO_CONTROL_IFACE, USB_AUDIO_STREAM_IFACE },
    .interface_class    = { 0x01, 0x01 },
    .interface_subclass = { 0x01, 0x02 },
    .interface_protocol = { 0x20, 0x20 },
  };
  usbredirparser_send_interface_info(audio->parser, &info);
}

static void sendEndpointInfo(LG_USBAudio * audio)
{
  struct usb_redir_ep_info_header info = { 0 };
  memset(info.type, usb_redir_type_invalid, sizeof(info.type));

  info.type[0]             = usb_redir_type_control;
  info.type[16]            = usb_redir_type_control;
  info.max_packet_size[0]  = 64;
  info.max_packet_size[16] = 64;

  if (audio->configuration == USB_AUDIO_CONFIGURATION &&
      audio->streamAlt == USB_AUDIO_STREAM_ALT)
  {
    info.type[USB_AUDIO_STREAM_ENDPOINT]            = usb_redir_type_iso;
    info.interval[USB_AUDIO_STREAM_ENDPOINT]        = USB_AUDIO_EP_INTERVAL;
    info.interface[USB_AUDIO_STREAM_ENDPOINT]       = USB_AUDIO_STREAM_IFACE;
    info.max_packet_size[USB_AUDIO_STREAM_ENDPOINT] =
      USB_AUDIO_MAX_PACKET_SIZE;
  }

  usbredirparser_send_ep_info(audio->parser, &info);
}

static void sendDeviceInfo(LG_USBAudio * audio)
{
  sendEndpointInfo(audio);
  sendInterfaceInfo(audio);
}

static void resetUSB(void * opaque)
{
  resetDevice(getAudio(opaque));
}

static void setConfiguration(void * opaque, uint64_t id,
    struct usb_redir_set_configuration_header * request)
{
  LG_USBAudio * audio = getAudio(opaque);
  struct usb_redir_configuration_status_header status =
  {
    .status        = usb_redir_stall,
    .configuration = audio->configuration,
  };

  if (request->configuration == 0 ||
      request->configuration == USB_AUDIO_CONFIGURATION)
  {
    resetDevice(audio);
    audio->configuration = request->configuration;
    status.status        = usb_redir_success;
    status.configuration = audio->configuration;
    sendDeviceInfo(audio);
  }

  usbredirparser_send_configuration_status(audio->parser, id, &status);
}

static void getConfiguration(void * opaque, uint64_t id)
{
  LG_USBAudio * audio = getAudio(opaque);
  struct usb_redir_configuration_status_header status =
  {
    .status        = usb_redir_success,
    .configuration = audio->configuration,
  };
  usbredirparser_send_configuration_status(audio->parser, id, &status);
}

static void setAltSetting(void * opaque, uint64_t id,
    struct usb_redir_set_alt_setting_header * request)
{
  LG_USBAudio * audio = getAudio(opaque);
  struct usb_redir_alt_setting_status_header status =
  {
    .status    = usb_redir_stall,
    .interface = request->interface,
    .alt       = request->alt,
  };

  if (audio->configuration == USB_AUDIO_CONFIGURATION &&
      request->interface == USB_AUDIO_CONTROL_IFACE && request->alt == 0)
    status.status = usb_redir_success;
  else if (audio->configuration == USB_AUDIO_CONFIGURATION &&
      request->interface == USB_AUDIO_STREAM_IFACE &&
      (request->alt == 0 || request->alt == USB_AUDIO_STREAM_ALT))
  {
    stopPlayback(audio);
    audio->streamAlt = request->alt;
    status.status    = usb_redir_success;
  }

  if (status.status == usb_redir_success)
    sendDeviceInfo(audio);
  usbredirparser_send_alt_setting_status(audio->parser, id, &status);
}

static void getAltSetting(void * opaque, uint64_t id,
    struct usb_redir_get_alt_setting_header * request)
{
  LG_USBAudio * audio = getAudio(opaque);
  struct usb_redir_alt_setting_status_header status =
  {
    .status    = usb_redir_stall,
    .interface = request->interface,
    .alt       = 0,
  };

  if (audio->configuration == USB_AUDIO_CONFIGURATION &&
      request->interface == USB_AUDIO_CONTROL_IFACE)
    status.status = usb_redir_success;
  else if (audio->configuration == USB_AUDIO_CONFIGURATION &&
      request->interface == USB_AUDIO_STREAM_IFACE)
  {
    status.status = usb_redir_success;
    status.alt    = audio->streamAlt;
  }

  usbredirparser_send_alt_setting_status(audio->parser, id, &status);
}

static void sendISOStatus(LG_USBAudio * audio, uint64_t id,
    uint8_t endpoint, uint8_t result)
{
  struct usb_redir_iso_stream_status_header status =
  {
    .status   = result,
    .endpoint = endpoint,
  };
  usbredirparser_send_iso_stream_status(audio->parser, id, &status);
}

static void startISOStream(void * opaque, uint64_t id,
    struct usb_redir_start_iso_stream_header * request)
{
  LG_USBAudio * audio = getAudio(opaque);
  uint8_t result = usb_redir_stall;

  if (request->endpoint == USB_AUDIO_STREAM_ENDPOINT &&
      audio->configuration == USB_AUDIO_CONFIGURATION &&
      audio->streamAlt == USB_AUDIO_STREAM_ALT)
  {
    result = usb_redir_success;
    if (!audio->streaming)
    {
      audio->streaming = true;
      if (audio->events && audio->events->start)
        audio->events->start(audio->eventOpaque);
    }
  }

  sendISOStatus(audio, id, request->endpoint, result);
}

static void stopISOStream(void * opaque, uint64_t id,
    struct usb_redir_stop_iso_stream_header * request)
{
  LG_USBAudio * audio = getAudio(opaque);
  const uint8_t result = request->endpoint == USB_AUDIO_STREAM_ENDPOINT ?
    usb_redir_success : usb_redir_stall;
  if (result == usb_redir_success)
    stopPlayback(audio);
  sendISOStatus(audio, id, request->endpoint, result);
}

static void cancelDataPacket(void * opaque, uint64_t id)
{
  (void)opaque;
  (void)id;
}

static size_t stringDescriptor(uint8_t index, uint8_t * buffer,
    size_t bufferSize)
{
  if (index == 0)
  {
    static const uint8_t language[] = { 0x04, 0x03, 0x09, 0x04 };
    memcpy(buffer, language, sizeof(language));
    return sizeof(language);
  }

  const char * text;
  switch (index)
  {
    case 1: text = "Looking Glass"          ; break;
    case 2: text = "Looking Glass USB Audio"; break;
    case 3: text = "LG-UAC2-0001"           ; break;
    default: return 0;
  }

  const size_t chars = strlen(text);
  const size_t size  = 2 + chars * 2;
  if (size > bufferSize)
    return 0;

  buffer[0] = (uint8_t)size;
  buffer[1] = USB_DESCRIPTOR_STRING;
  for (size_t i = 0; i < chars; ++i)
  {
    buffer[2 + i * 2] = text[i];
    buffer[3 + i * 2] = 0;
  }
  return size;
}

static void sendControlResponse(LG_USBAudio * audio, uint64_t id,
    const struct usb_redir_control_packet_header * request, uint8_t status,
    const uint8_t * data, size_t size)
{
  struct usb_redir_control_packet_header response = *request;
  response.status = status;

  if (status != usb_redir_success)
  {
    response.length = 0;
    data             = NULL;
    size             = 0;
  }
  else if (request->requesttype & 0x80)
  {
    if (size > request->length)
      size = request->length;
    response.length = (uint16_t)size;
  }
  else
  {
    response.length = request->length;
    data             = NULL;
    size             = 0;
  }

  usbredirparser_send_control_packet(audio->parser, id, &response,
      (uint8_t *)data, (int)size);
}

static void controlPacket(void * opaque, uint64_t id,
    struct usb_redir_control_packet_header * request,
    uint8_t * data, int dataLength)
{
  LG_USBAudio * audio = getAudio(opaque);
  uint8_t buffer[sizeof(l_configurationDescriptor)];
  const uint8_t * response = NULL;
  size_t responseSize = 0;
  uint8_t status = usb_redir_stall;
  (void)dataLength;

  if (request->requesttype == USB_REQUEST_TYPE_IN_STANDARD_DEVICE &&
      request->request == USB_REQUEST_GET_DESCRIPTOR)
  {
    const uint8_t type  = request->value >> 8;
    const uint8_t index = request->value;

    switch (type)
    {
      case USB_DESCRIPTOR_DEVICE:
        if (index == 0)
        {
          response     = l_deviceDescriptor;
          responseSize = sizeof(l_deviceDescriptor);
        }
        break;

      case USB_DESCRIPTOR_CONFIGURATION:
        if (index == 0)
        {
          response     = l_configurationDescriptor;
          responseSize = sizeof(l_configurationDescriptor);
        }
        break;

      case USB_DESCRIPTOR_STRING:
        responseSize = stringDescriptor(index, buffer, sizeof(buffer));
        if (responseSize)
          response = buffer;
        break;

      case USB_DESCRIPTOR_DEVICE_QUALIFIER:
        if (index == 0)
        {
          response     = l_deviceQualifierDescriptor;
          responseSize = sizeof(l_deviceQualifierDescriptor);
        }
        break;

      case USB_DESCRIPTOR_OTHER_SPEED:
        if (index == 0)
        {
          memcpy(buffer, l_configurationDescriptor,
              sizeof(l_configurationDescriptor));
          buffer[1]                         = USB_DESCRIPTOR_OTHER_SPEED;
          buffer[USB_AUDIO_FS_INTERVAL_POS] = 1;
          response                          = buffer;
          responseSize                      =
            sizeof(l_configurationDescriptor);
        }
        break;
    }

    if (response)
      status = usb_redir_success;
  }
  else if ((request->requesttype == USB_REQUEST_TYPE_IN_STANDARD_DEVICE ||
        request->requesttype == USB_REQUEST_TYPE_IN_STANDARD_INTERFACE ||
        request->requesttype == USB_REQUEST_TYPE_IN_STANDARD_ENDPOINT) &&
      request->request == USB_REQUEST_GET_STATUS && request->value == 0)
  {
    response     = l_zeroStatus;
    responseSize = sizeof(l_zeroStatus);
    status       = usb_redir_success;
  }
  else if (request->requesttype == USB_REQUEST_TYPE_IN_CLASS_INTERFACE &&
      request->index == (USB_AUDIO_CLOCK_ID << 8) &&
      (uint8_t)request->value == 0)
  {
    const uint8_t control = request->value >> 8;
    if (request->request == USB_REQUEST_CUR &&
        control == USB_AUDIO_CONTROL_FREQUENCY)
    {
      response     = l_clockFrequency;
      responseSize = sizeof(l_clockFrequency);
      status       = usb_redir_success;
    }
    else if (request->request == USB_REQUEST_RANGE &&
        control == USB_AUDIO_CONTROL_FREQUENCY)
    {
      response     = l_clockFrequencyRange;
      responseSize = sizeof(l_clockFrequencyRange);
      status       = usb_redir_success;
    }
    else if (request->request == USB_REQUEST_CUR &&
        control == USB_AUDIO_CONTROL_VALIDITY)
    {
      response     = l_clockValid;
      responseSize = sizeof(l_clockValid);
      status       = usb_redir_success;
    }
  }

  sendControlResponse(audio, id, request, status, response, responseSize);
  if (data)
    usbredirparser_free_packet_data(audio->parser, data);
}

static void stallStream(LG_USBAudio * audio)
{
  stopPlayback(audio);
  sendISOStatus(audio, 0, USB_AUDIO_STREAM_ENDPOINT, usb_redir_stall);
}

static void isoPacket(void * opaque, uint64_t id,
    struct usb_redir_iso_packet_header * packet,
    uint8_t * data, int dataLength)
{
  (void)id;
  LG_USBAudio * audio = getAudio(opaque);

  if (audio->streaming)
  {
    if (packet->endpoint != USB_AUDIO_STREAM_ENDPOINT ||
        packet->status != usb_redir_success || dataLength < 0 ||
        packet->length != dataLength ||
        dataLength > USB_AUDIO_MAX_PACKET_SIZE ||
        dataLength % USB_AUDIO_FRAME_SIZE != 0)
    {
      DEBUG_WARN("Invalid USB audio isochronous packet");
      stallStream(audio);
    }
    else if (dataLength && audio->events && audio->events->data)
      audio->events->data(audio->eventOpaque, data,
          dataLength / USB_AUDIO_FRAME_SIZE);
  }

  if (data)
    usbredirparser_free_packet_data(audio->parser, data);
}

static void setupDevice(void * opaque, struct usbredirparser * parser)
{
  LG_USBAudio * audio = opaque;
  audio->parser = parser;

  parser->reset_func              = resetUSB;
  parser->set_configuration_func  = setConfiguration;
  parser->get_configuration_func  = getConfiguration;
  parser->set_alt_setting_func    = setAltSetting;
  parser->get_alt_setting_func    = getAltSetting;
  parser->start_iso_stream_func   = startISOStream;
  parser->stop_iso_stream_func    = stopISOStream;
  parser->cancel_data_packet_func = cancelDataPacket;
  parser->control_packet_func     = controlPacket;
  parser->iso_packet_func         = isoPacket;
}

static void plugDevice(void * opaque, struct usbredirparser * parser)
{
  LG_USBAudio * audio = opaque;
  audio->parser = parser;
  resetDevice(audio);
  sendDeviceInfo(audio);

  struct usb_redir_device_connect_header device =
  {
    .speed              = usb_redir_speed_high,
    .device_class       = 0xef,
    .device_subclass    = 0x02,
    .device_protocol    = 0x01,
    .vendor_id          = USB_AUDIO_VENDOR_ID,
    .product_id         = USB_AUDIO_PRODUCT_ID,
    .device_version_bcd = USB_AUDIO_DEVICE_VERSION,
  };
  usbredirparser_send_device_connect(audio->parser, &device);
}

static void unplugDevice(void * opaque)
{
  resetDevice(opaque);
}

static const LG_USBRedirDeviceOps l_deviceOps =
{
  .setup  = setupDevice,
  .plug   = plugDevice,
  .unplug = unplugDevice,
};

LG_USBAudio * lgUsbAudio_create(
    const LG_USBAudioEventOps * events, void * eventOpaque)
{
  LG_USBAudio * audio = calloc(1, sizeof(*audio));
  if (!audio)
    return NULL;

  audio->events      = events;
  audio->eventOpaque = eventOpaque;
  return audio;
}

void lgUsbAudio_destroy(LG_USBAudio * audio)
{
  if (!audio)
    return;

  free(audio);
}

const LG_USBRedirDeviceOps * lgUsbAudio_deviceOps(void)
{
  return &l_deviceOps;
}
