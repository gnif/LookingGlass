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
#include "common/locking.h"
#include "common/ringbuffer.h"
#include "common/time.h"

#include <usbredirparser.h>

#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum
{
  USB_AUDIO_VENDOR_ID                 = 0x043e,
  USB_AUDIO_PRODUCT_ID                = 0x0001,
  USB_AUDIO_DEVICE_VERSION            = 0x010d,
  USB_AUDIO_CONFIGURATION             = 1,
  USB_AUDIO_PLAYBACK_CONTROL_IFACE    = 0,
  USB_AUDIO_PLAYBACK_IFACE            = 1,
  USB_AUDIO_RECORD_CONTROL_IFACE      = 2,
  USB_AUDIO_RECORD_IFACE              = 3,
  USB_AUDIO_PLAYBACK_CLOCK_ID         = 1,
  USB_AUDIO_RECORD_CLOCK_ID           = 1,
  USB_AUDIO_PLAYBACK_DATA_ENDPOINT    = 0x01,
  USB_AUDIO_FEEDBACK_ENDPOINT         = 0x81,
  USB_AUDIO_RECORD_DATA_ENDPOINT      = 0x82,
  USB_AUDIO_PLAYBACK_DATA_EP_INDEX    = 1,
  USB_AUDIO_FEEDBACK_EP_INDEX         = 17,
  USB_AUDIO_RECORD_DATA_EP_INDEX      = 18,
  USB_AUDIO_PLAYBACK_DATA_INTERVAL    = 1,
  USB_AUDIO_FEEDBACK_INTERVAL         = 8,
  USB_AUDIO_RECORD_DATA_INTERVAL      = 4,
  USB_AUDIO_FEEDBACK_BINTERVAL        = 4,
  USB_AUDIO_RECORD_DATA_BINTERVAL     = 3,
  USB_AUDIO_FEEDBACK_SIZE             = 4,
  USB_AUDIO_FEEDBACK_MAX_QUEUE        = 512,
  USB_AUDIO_SAMPLE_SIZE               = 3,
  USB_AUDIO_PLAYBACK_HS_PACKET_FRAMES = 25,
  USB_AUDIO_FS_PACKET_FRAMES          = 97,
  USB_AUDIO_RECORD_PACKET_FRAMES      = 97,
  USB_AUDIO_RECORD_PACKETS_PER_SECOND = 2000,
  USB_AUDIO_RECORD_PACKET_INTERVAL_NS = 500000,
  USB_AUDIO_RECORD_RATE_SCALE         = 65536,
  USB_AUDIO_RECORD_RATE_SETTLE_NS     = 2000000000,
  USB_AUDIO_RECORD_RATE_GAP_NS        = 100000000,
  USB_AUDIO_RECORD_RATE_DENOMINATOR   =
    USB_AUDIO_RECORD_PACKETS_PER_SECOND * USB_AUDIO_RECORD_RATE_SCALE,
  USB_AUDIO_RECORD_MAX_BATCH          = 64,
  USB_AUDIO_RECORD_QUEUE_FRAMES       = 3840,
  USB_AUDIO_RECORD_FRAME_SIZE         =
    2 * USB_AUDIO_SAMPLE_SIZE,
  USB_AUDIO_RECORD_PACKET_SIZE        =
    USB_AUDIO_RECORD_PACKET_FRAMES * USB_AUDIO_RECORD_FRAME_SIZE,
  USB_AUDIO_CONFIGURATION_DESC_SIZE   = 9,
  USB_AUDIO_FUNCTION_CONTROL_SIZE     = 63,
  USB_AUDIO_PLAYBACK_STREAM_DESC_SIZE = 53,
  USB_AUDIO_RECORD_STREAM_DESC_SIZE   = 46,
};

enum
{
  USB_AUDIO_LAYOUT_STEREO  = 0x00000003,
  USB_AUDIO_LAYOUT_QUAD    = 0x00000033,
  USB_AUDIO_LAYOUT_5POINT1 = 0x0000003f,
  USB_AUDIO_LAYOUT_7POINT1 = 0x0000063f,
};

#define USB_AUDIO_LAYOUTS(X) \
  X(1, 8, USB_AUDIO_LAYOUT_7POINT1) \
  X(2, 6, USB_AUDIO_LAYOUT_5POINT1) \
  X(3, 4, USB_AUDIO_LAYOUT_QUAD   ) \
  X(4, 2, USB_AUDIO_LAYOUT_STEREO )

#define USB_AUDIO_COUNT_LAYOUT(alt, channels, mask) + 1
enum
{
  USB_AUDIO_LAYOUT_COUNT =
    0 USB_AUDIO_LAYOUTS(USB_AUDIO_COUNT_LAYOUT),
  USB_AUDIO_RECORD_FUNCTION_OFFSET =
    USB_AUDIO_CONFIGURATION_DESC_SIZE +
    USB_AUDIO_FUNCTION_CONTROL_SIZE + 9 +
    USB_AUDIO_LAYOUT_COUNT * USB_AUDIO_PLAYBACK_STREAM_DESC_SIZE,
  USB_AUDIO_CONFIGURATION_SIZE =
    USB_AUDIO_RECORD_FUNCTION_OFFSET +
    USB_AUDIO_FUNCTION_CONTROL_SIZE +
    9 + USB_AUDIO_RECORD_STREAM_DESC_SIZE,
  USB_AUDIO_OTHER_SPEED_SIZE =
    USB_AUDIO_CONFIGURATION_DESC_SIZE +
    USB_AUDIO_FUNCTION_CONTROL_SIZE +
    9 + USB_AUDIO_PLAYBACK_STREAM_DESC_SIZE +
    USB_AUDIO_FUNCTION_CONTROL_SIZE +
    9 + USB_AUDIO_RECORD_STREAM_DESC_SIZE,
};
#undef USB_AUDIO_COUNT_LAYOUT

#define USB_AUDIO_PACKET_SIZE(frames, channels) \
  ((frames) * (channels) * USB_AUDIO_SAMPLE_SIZE)

typedef struct USBAudioLayout
{
  uint32_t channelMask;
  uint8_t  channelCount;
}
USBAudioLayout;

#define USB_AUDIO_LAYOUT_ENTRY(alt, channels, mask) \
  [alt - 1] = { .channelMask = mask, .channelCount = channels },
static const USBAudioLayout l_channelLayouts[] =
{
  USB_AUDIO_LAYOUTS(USB_AUDIO_LAYOUT_ENTRY)
};
#undef USB_AUDIO_LAYOUT_ENTRY

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
  USB_REQUEST_TYPE_OUT_CLASS_INTERFACE   = 0x21,
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
  0x3e, 0x04, 0x01, 0x00,
  USB_AUDIO_DEVICE_VERSION       & 0xff,
  USB_AUDIO_DEVICE_VERSION >> 8  & 0xff,
  0x01, 0x02,
  0x03, 0x01,
};

#define USB_AUDIO_PLAYBACK_STREAM_DESCRIPTOR(alt, channels, mask, \
    packetFrames, feedbackSize, feedbackInterval) \
  0x09, 0x04, USB_AUDIO_PLAYBACK_IFACE, alt, \
    0x02, 0x01, 0x02, 0x20, 0x00, \
  0x10, 0x24, 0x01, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, \
    channels, \
    (mask)       & 0xff, \
    (mask) >>  8 & 0xff, \
    (mask) >> 16 & 0xff, \
    (mask) >> 24 & 0xff, \
    0x00, \
  0x06, 0x24, 0x02, 0x01, 0x03, 0x18, \
  0x07, 0x05, USB_AUDIO_PLAYBACK_DATA_ENDPOINT, 0x05, \
    USB_AUDIO_PACKET_SIZE(packetFrames, channels)      & 0xff, \
    USB_AUDIO_PACKET_SIZE(packetFrames, channels) >> 8 & 0xff, \
    USB_AUDIO_PLAYBACK_DATA_INTERVAL, \
  0x08, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, \
  0x07, 0x05, USB_AUDIO_FEEDBACK_ENDPOINT, 0x11, \
    feedbackSize, 0x00, feedbackInterval,

#define USB_AUDIO_PLAYBACK_HS_STREAM_DESCRIPTOR(alt, channels, mask) \
  USB_AUDIO_PLAYBACK_STREAM_DESCRIPTOR( \
      alt, channels, mask, USB_AUDIO_PLAYBACK_HS_PACKET_FRAMES, \
      USB_AUDIO_FEEDBACK_SIZE, USB_AUDIO_FEEDBACK_BINTERVAL)

#define USB_AUDIO_RECORD_STREAM_DESCRIPTOR(packetFrames, interval) \
  0x09, 0x04, USB_AUDIO_RECORD_IFACE, 0x01, \
    0x01, 0x01, 0x02, 0x20, 0x00, \
  0x10, 0x24, 0x01, 0x03, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, \
    0x02, 0x03, 0x00, 0x00, 0x00, 0x00, \
  0x06, 0x24, 0x02, 0x01, 0x03, 0x18, \
  0x07, 0x05, USB_AUDIO_RECORD_DATA_ENDPOINT, 0x05, \
    USB_AUDIO_PACKET_SIZE(packetFrames, 2)      & 0xff, \
    USB_AUDIO_PACKET_SIZE(packetFrames, 2) >> 8 & 0xff, \
    interval, \
  0x08, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,

static const uint8_t l_configurationDescriptor[] =
{
  /* Configuration */
  0x09, 0x02,
  USB_AUDIO_CONFIGURATION_SIZE       & 0xff,
  USB_AUDIO_CONFIGURATION_SIZE >> 8  & 0xff,
  0x04, 0x01, 0x00, 0x80, 0x32,

  /* Playback audio function */
  0x08, 0x0b, USB_AUDIO_PLAYBACK_CONTROL_IFACE, 0x02,
    0x01, 0x00, 0x20, 0x02,

  0x09, 0x04, USB_AUDIO_PLAYBACK_CONTROL_IFACE, 0x00,
    0x00, 0x01, 0x01, 0x20, 0x00,
  0x09, 0x24, 0x01, 0x00, 0x02, 0x02, 0x2e, 0x00, 0x00,
  0x08, 0x24, 0x0a, USB_AUDIO_PLAYBACK_CLOCK_ID,
    0x03, 0x07, 0x00, 0x00,
  0x11, 0x24, 0x02, 0x02, 0x01, 0x01, 0x00, 0x01, 0x08,
  0x3f, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x0c, 0x24, 0x03, 0x03, 0x01, 0x03, 0x00, 0x02, 0x01,
  0x00, 0x00, 0x00,

  /* Playback interface, idle alternate setting */
  0x09, 0x04, USB_AUDIO_PLAYBACK_IFACE, 0x00,
    0x00, 0x01, 0x02, 0x20, 0x00,

  /* Publish the maximum topology first for the Windows endpoint model. */
  USB_AUDIO_LAYOUTS(USB_AUDIO_PLAYBACK_HS_STREAM_DESCRIPTOR)

  /* Recording audio function */
  0x08, 0x0b, USB_AUDIO_RECORD_CONTROL_IFACE, 0x02,
    0x01, 0x00, 0x20, 0x04,

  0x09, 0x04, USB_AUDIO_RECORD_CONTROL_IFACE, 0x00,
    0x00, 0x01, 0x01, 0x20, 0x00,
  0x09, 0x24, 0x01, 0x00, 0x02, 0x03, 0x2e, 0x00, 0x00,
  0x08, 0x24, 0x0a, USB_AUDIO_RECORD_CLOCK_ID,
    0x03, 0x07, 0x00, 0x00,
  0x11, 0x24, 0x02, 0x02, 0x01, 0x02, 0x00, 0x01, 0x02,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
  0x0c, 0x24, 0x03, 0x03, 0x01, 0x01, 0x00, 0x02, 0x01,
    0x00, 0x00, 0x00,

  /* Recording interface, idle alternate setting */
  0x09, 0x04, USB_AUDIO_RECORD_IFACE, 0x00,
    0x00, 0x01, 0x02, 0x20, 0x00,
  USB_AUDIO_RECORD_STREAM_DESCRIPTOR(
      USB_AUDIO_RECORD_PACKET_FRAMES,
      USB_AUDIO_RECORD_DATA_BINTERVAL)
};
#undef USB_AUDIO_PLAYBACK_HS_STREAM_DESCRIPTOR
#undef USB_AUDIO_LAYOUTS

static const uint8_t l_fullSpeedPlaybackDescriptor[] =
{
  0x09, 0x04, USB_AUDIO_PLAYBACK_IFACE, 0x00,
    0x00, 0x01, 0x02, 0x20, 0x00,
  USB_AUDIO_PLAYBACK_STREAM_DESCRIPTOR(1, 2, USB_AUDIO_LAYOUT_STEREO,
      USB_AUDIO_FS_PACKET_FRAMES, 3, 1)
};

static const uint8_t l_fullSpeedRecordDescriptor[] =
{
  0x09, 0x04, USB_AUDIO_RECORD_IFACE, 0x00,
    0x00, 0x01, 0x02, 0x20, 0x00,
  USB_AUDIO_RECORD_STREAM_DESCRIPTOR(
      USB_AUDIO_FS_PACKET_FRAMES, 1)
};
#undef USB_AUDIO_RECORD_STREAM_DESCRIPTOR
#undef USB_AUDIO_PLAYBACK_STREAM_DESCRIPTOR
#undef USB_AUDIO_PACKET_SIZE

static const uint8_t l_deviceQualifierDescriptor[] =
{
  0x0a, 0x06, 0x00, 0x02, 0xef, 0x02, 0x01, 0x40, 0x01, 0x00,
};

static const uint32_t l_sampleRates[] =
{
   44100,
   48000,
   88200,
   96000,
  176400,
  192000,
};

static const uint8_t l_clockValid[] = { 0x01 };
static const uint8_t l_zeroStatus[] = { 0x00, 0x00 };

_Static_assert(sizeof(l_deviceDescriptor) == 18,
    "invalid USB device descriptor size");
_Static_assert(sizeof(l_channelLayouts) / sizeof(*l_channelLayouts) ==
    USB_AUDIO_LAYOUT_COUNT, "invalid USB audio layout count");
_Static_assert(sizeof(l_configurationDescriptor) ==
    USB_AUDIO_CONFIGURATION_SIZE,
    "invalid USB configuration descriptor size");
_Static_assert(sizeof(l_fullSpeedPlaybackDescriptor) ==
    9 + USB_AUDIO_PLAYBACK_STREAM_DESC_SIZE,
    "invalid full-speed playback descriptor size");
_Static_assert(sizeof(l_fullSpeedRecordDescriptor) ==
    9 + USB_AUDIO_RECORD_STREAM_DESC_SIZE,
    "invalid full-speed recording descriptor size");
_Static_assert(sizeof(l_deviceQualifierDescriptor) == 10,
    "invalid USB device qualifier descriptor size");
_Static_assert(USB_AUDIO_OTHER_SPEED_SIZE <=
    sizeof(l_configurationDescriptor), "invalid other-speed descriptor size");
_Static_assert(USB_AUDIO_FS_PACKET_FRAMES * 2 * USB_AUDIO_SAMPLE_SIZE <=
    1023, "full-speed stream packet is too large");
_Static_assert(USB_AUDIO_RECORD_PACKET_SIZE <= 1024,
    "high-speed recording packet is too large");

struct LG_USBAudio
{
  const LG_USBAudioEventOps * events;
  void                      * eventOpaque;
  struct usbredirparser     * parser;

  uint8_t     configuration;
  uint8_t     playbackAlt;
  uint8_t     recordAlt;
  uint32_t    playbackSampleRate;
  uint32_t    recordSampleRate;
  bool        playbackStreaming;
  bool        feedbackStreaming;
  uint64_t    feedbackPacketId;
  uint8_t     feedbackDataPackets;
  atomic_uint feedbackValue;

  atomic_bool recordStreaming;
  uint64_t    recordPacketId;
  uint64_t    recordPacketPhase;
  uint64_t    recordNextPacketTime;
  LG_Lock     recordLock;
  RingBuffer  recordBuffer;
  uint64_t    recordRateQ16;
  uint64_t    recordRateStartTime;
  uint64_t    recordRateLastTime;
  uint64_t    recordRateFrames;
};

static LG_USBAudio * getAudio(void * opaque)
{
  return lgUsbRedir_device(opaque);
}

static uint64_t recordTime(void)
{
  /* PipeWire timestamps use CLOCK_MONOTONIC. Keep USB capture scheduling in
   * that domain instead of introducing MONOTONIC_RAW clock drift. */
  return microtime() * UINT64_C(1000);
}

static uint32_t readLE32(const uint8_t * data)
{
  return
    (uint32_t)data[0]       |
    (uint32_t)data[1] <<  8 |
    (uint32_t)data[2] << 16 |
    (uint32_t)data[3] << 24;
}

static void writeLE16(uint8_t * data, uint16_t value)
{
  data[0] = value;
  data[1] = value >> 8;
}

static void writeLE32(uint8_t * data, uint32_t value)
{
  data[0] = value;
  data[1] = value >> 8;
  data[2] = value >> 16;
  data[3] = value >> 24;
}

static uint32_t encodeFeedbackRate(double sampleRate)
{
  return (uint32_t)(sampleRate * UINT32_C(65536) / 8000.0 + 0.5);
}

static void resetFeedbackRate(LG_USBAudio * audio)
{
  atomic_store_explicit(&audio->feedbackValue,
      encodeFeedbackRate(audio->playbackSampleRate), memory_order_release);
}

static uint32_t nearestSampleRate(uint32_t sampleRate)
{
  uint32_t selected  = l_sampleRates[0];
  uint64_t bestDelta = sampleRate > selected ?
    (uint64_t)sampleRate - selected : (uint64_t)selected - sampleRate;

  for (size_t i = 1;
      i < sizeof(l_sampleRates) / sizeof(*l_sampleRates); ++i)
  {
    const uint32_t candidate = l_sampleRates[i];
    const uint64_t delta     = sampleRate > candidate ?
      (uint64_t)sampleRate - candidate : (uint64_t)candidate - sampleRate;
    if (delta < bestDelta)
    {
      selected  = candidate;
      bestDelta = delta;
    }
  }

  return selected;
}

static const USBAudioLayout * getLayout(uint8_t alt)
{
  if (alt == 0 || alt > USB_AUDIO_LAYOUT_COUNT)
    return NULL;

  return &l_channelLayouts[alt - 1];
}

static uint16_t layoutPacketSize(const USBAudioLayout * layout)
{
  return USB_AUDIO_PLAYBACK_HS_PACKET_FRAMES * layout->channelCount *
    USB_AUDIO_SAMPLE_SIZE;
}

static size_t writeSampleRateRange(uint8_t * buffer)
{
  const size_t count = sizeof(l_sampleRates) / sizeof(*l_sampleRates);
  writeLE16(buffer, (uint16_t)count);

  uint8_t * range = buffer + 2;
  for (size_t i = 0; i < count; ++i, range += 12)
  {
    writeLE32(range     , l_sampleRates[i]);
    writeLE32(range +  4, l_sampleRates[i]);
    writeLE32(range +  8, 0);
  }

  return 2 + count * 12;
}

static void stopPlayback(LG_USBAudio * audio)
{
  if (!audio->playbackStreaming)
    return;

  audio->playbackStreaming = false;
  resetFeedbackRate(audio);
  if (audio->events && audio->events->playbackStop)
    audio->events->playbackStop(audio->eventOpaque);
}

static void stopFeedback(LG_USBAudio * audio)
{
  audio->feedbackStreaming   = false;
  audio->feedbackDataPackets = 0;
}

static void stopPlaybackStreams(LG_USBAudio * audio)
{
  stopPlayback(audio);
  stopFeedback(audio);
}

static void startPlayback(LG_USBAudio * audio)
{
  const USBAudioLayout * layout = getLayout(audio->playbackAlt);
  if (audio->playbackStreaming || !layout)
    return;

  resetFeedbackRate(audio);
  audio->playbackStreaming = true;
  if (audio->events && audio->events->playbackStart)
    audio->events->playbackStart(
        audio->eventOpaque, audio->playbackSampleRate,
        layout->channelMask);
}

static void stopRecord(LG_USBAudio * audio)
{
  if (!atomic_exchange_explicit(
        &audio->recordStreaming, false, memory_order_acq_rel))
    return;

  if (audio->events && audio->events->recordStop)
    audio->events->recordStop(audio->eventOpaque);

  LG_LOCK(audio->recordLock);
  ringbuffer_reset(audio->recordBuffer);
  LG_UNLOCK(audio->recordLock);
}

static void resetRecordData(LG_USBAudio * audio)
{
  LG_LOCK(audio->recordLock);
  ringbuffer_reset(audio->recordBuffer);
  audio->recordRateQ16 =
    (uint64_t)audio->recordSampleRate * USB_AUDIO_RECORD_RATE_SCALE;
  audio->recordRateStartTime = 0;
  audio->recordRateLastTime  = 0;
  audio->recordRateFrames    = 0;
  LG_UNLOCK(audio->recordLock);
  audio->recordPacketPhase = 0;
}

static void startRecord(LG_USBAudio * audio)
{
  if (audio->recordAlt != 1 || atomic_load_explicit(
        &audio->recordStreaming, memory_order_acquire))
    return;

  resetRecordData(audio);
  audio->recordPacketId = 0;
  atomic_store_explicit(
      &audio->recordStreaming, true, memory_order_release);

  if (audio->events && audio->events->recordStart)
    audio->events->recordStart(audio->eventOpaque,
        audio->recordSampleRate, USB_AUDIO_LAYOUT_STEREO);

  /* Backend startup may block while the capture graph is configured. Do not
   * turn that setup time into a burst of stale USB packets. */
  audio->recordNextPacketTime = recordTime();
}

static void reconfigureRecord(LG_USBAudio * audio)
{
  if (!atomic_load_explicit(
        &audio->recordStreaming, memory_order_acquire))
    return;

  if (audio->events && audio->events->recordStart)
    audio->events->recordStart(audio->eventOpaque,
        audio->recordSampleRate, USB_AUDIO_LAYOUT_STEREO);

  /* The provider restart has quiesced the old producer. Drop any frames
   * queued around the format transition and restart the USB sample phase. */
  resetRecordData(audio);
  audio->recordNextPacketTime = recordTime();
}

static void stopStreams(LG_USBAudio * audio)
{
  stopPlaybackStreams(audio);
  stopRecord(audio);
}

static void setPlaybackSampleRate(
    LG_USBAudio * audio, uint32_t sampleRate)
{
  if (audio->playbackSampleRate == sampleRate)
    return;

  const bool restartPlayback = audio->playbackStreaming;
  stopPlayback(audio);
  audio->playbackSampleRate = sampleRate;
  resetFeedbackRate(audio);
  if (restartPlayback)
    startPlayback(audio);
}

static void setRecordSampleRate(
    LG_USBAudio * audio, uint32_t sampleRate)
{
  LG_LOCK(audio->recordLock);
  const bool changed = audio->recordSampleRate != sampleRate;
  audio->recordSampleRate = sampleRate;
  LG_UNLOCK(audio->recordLock);
  if (!changed)
    return;

  const bool restartRecord = atomic_load_explicit(
      &audio->recordStreaming, memory_order_acquire);
  if (restartRecord)
    reconfigureRecord(audio);
}

static void resetDevice(LG_USBAudio * audio)
{
  stopStreams(audio);
  audio->configuration       = 0;
  audio->playbackAlt         = 0;
  audio->recordAlt           = 0;
  audio->playbackSampleRate  = LG_USB_AUDIO_DEFAULT_SAMPLE_RATE;
  LG_LOCK(audio->recordLock);
  audio->recordSampleRate    = LG_USB_AUDIO_DEFAULT_SAMPLE_RATE;
  LG_UNLOCK(audio->recordLock);
  resetFeedbackRate(audio);
}

static void sendInterfaceInfo(LG_USBAudio * audio)
{
  struct usb_redir_interface_info_header info =
  {
    .interface_count = 4,
    .interface =
    {
      USB_AUDIO_PLAYBACK_CONTROL_IFACE,
      USB_AUDIO_PLAYBACK_IFACE,
      USB_AUDIO_RECORD_CONTROL_IFACE,
      USB_AUDIO_RECORD_IFACE,
    },
    .interface_class    = { 0x01, 0x01, 0x01, 0x01 },
    .interface_subclass = { 0x01, 0x02, 0x01, 0x02 },
    .interface_protocol = { 0x20, 0x20, 0x20, 0x20 },
  };
  usbredirparser_send_interface_info(audio->parser, &info);
}

static void sendEndpointInfo(LG_USBAudio * audio)
{
  struct usb_redir_ep_info_header info = { 0 };
  const USBAudioLayout * layout = getLayout(audio->playbackAlt);
  memset(info.type, usb_redir_type_invalid, sizeof(info.type));

  info.type[0]             = usb_redir_type_control;
  info.type[16]            = usb_redir_type_control;
  info.max_packet_size[0]  = 64;
  info.max_packet_size[16] = 64;

  if (audio->configuration == USB_AUDIO_CONFIGURATION && layout)
  {
    info.type[USB_AUDIO_PLAYBACK_DATA_EP_INDEX] = usb_redir_type_iso;
    info.interval[USB_AUDIO_PLAYBACK_DATA_EP_INDEX] =
      USB_AUDIO_PLAYBACK_DATA_INTERVAL;
    info.interface[USB_AUDIO_PLAYBACK_DATA_EP_INDEX] =
      USB_AUDIO_PLAYBACK_IFACE;
    info.max_packet_size[USB_AUDIO_PLAYBACK_DATA_EP_INDEX] =
      layoutPacketSize(layout);

    info.type[USB_AUDIO_FEEDBACK_EP_INDEX]            = usb_redir_type_iso;
    info.interval[USB_AUDIO_FEEDBACK_EP_INDEX]        =
      USB_AUDIO_FEEDBACK_INTERVAL;
    info.interface[USB_AUDIO_FEEDBACK_EP_INDEX]       =
      USB_AUDIO_PLAYBACK_IFACE;
    info.max_packet_size[USB_AUDIO_FEEDBACK_EP_INDEX] =
      USB_AUDIO_FEEDBACK_SIZE;
  }

  if (audio->configuration == USB_AUDIO_CONFIGURATION &&
      audio->recordAlt == 1)
  {
    info.type[USB_AUDIO_RECORD_DATA_EP_INDEX] = usb_redir_type_iso;
    info.interval[USB_AUDIO_RECORD_DATA_EP_INDEX] =
      USB_AUDIO_RECORD_DATA_INTERVAL;
    info.interface[USB_AUDIO_RECORD_DATA_EP_INDEX] =
      USB_AUDIO_RECORD_IFACE;
    info.max_packet_size[USB_AUDIO_RECORD_DATA_EP_INDEX] =
      USB_AUDIO_RECORD_PACKET_SIZE;
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
  LG_USBAudio * audio = getAudio(opaque);
  resetDevice(audio);
  sendEndpointInfo(audio);
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
      (request->interface == USB_AUDIO_PLAYBACK_CONTROL_IFACE ||
       request->interface == USB_AUDIO_RECORD_CONTROL_IFACE) &&
      request->alt == 0)
    status.status = usb_redir_success;
  else if (audio->configuration == USB_AUDIO_CONFIGURATION &&
      request->interface == USB_AUDIO_PLAYBACK_IFACE &&
      (request->alt == 0 || getLayout(request->alt)))
  {
    stopPlaybackStreams(audio);
    audio->playbackAlt = request->alt;
    status.status      = usb_redir_success;
  }
  else if (audio->configuration == USB_AUDIO_CONFIGURATION &&
      request->interface == USB_AUDIO_RECORD_IFACE &&
      request->alt <= 1)
  {
    stopRecord(audio);
    audio->recordAlt = request->alt;
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
      (request->interface == USB_AUDIO_PLAYBACK_CONTROL_IFACE ||
       request->interface == USB_AUDIO_RECORD_CONTROL_IFACE))
    status.status = usb_redir_success;
  else if (audio->configuration == USB_AUDIO_CONFIGURATION &&
      request->interface == USB_AUDIO_PLAYBACK_IFACE)
  {
    status.status = usb_redir_success;
    status.alt    = audio->playbackAlt;
  }
  else if (audio->configuration == USB_AUDIO_CONFIGURATION &&
      request->interface == USB_AUDIO_RECORD_IFACE)
  {
    status.status = usb_redir_success;
    status.alt    = audio->recordAlt;
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

static void sendFeedbackPackets(LG_USBAudio * audio, uint32_t count)
{
  uint8_t data[USB_AUDIO_FEEDBACK_SIZE];
  writeLE32(data, atomic_load_explicit(
        &audio->feedbackValue, memory_order_acquire));
  struct usb_redir_iso_packet_header packet =
  {
    .endpoint = USB_AUDIO_FEEDBACK_ENDPOINT,
    .status   = usb_redir_success,
    .length   = sizeof(data),
  };

  for (uint32_t i = 0; i < count; ++i)
    usbredirparser_send_iso_packet(audio->parser,
        audio->feedbackPacketId++,
        &packet, data, USB_AUDIO_FEEDBACK_SIZE);
}

static void sendRecordPackets(LG_USBAudio * audio)
{
  if (!atomic_load_explicit(
        &audio->recordStreaming, memory_order_acquire))
    return;

  const uint64_t now = recordTime();
  if (audio->recordNextPacketTime > now)
    return;

  uint64_t count =
    (now - audio->recordNextPacketTime) /
    USB_AUDIO_RECORD_PACKET_INTERVAL_NS + 1;
  if (count > USB_AUDIO_RECORD_MAX_BATCH)
  {
    count = USB_AUDIO_RECORD_MAX_BATCH;
    audio->recordNextPacketTime = now -
      (count - 1) * USB_AUDIO_RECORD_PACKET_INTERVAL_NS;
  }

  uint8_t data[USB_AUDIO_RECORD_PACKET_SIZE];
  const int target = max(
      (int)(audio->recordSampleRate / 50),
      USB_AUDIO_RECORD_PACKET_FRAMES);
  LG_LOCK(audio->recordLock);
  const uint64_t rateQ16 = audio->recordRateQ16;
  LG_UNLOCK(audio->recordLock);
  struct usb_redir_iso_packet_header packet =
  {
    .endpoint = USB_AUDIO_RECORD_DATA_ENDPOINT,
    .status   = usb_redir_success,
  };

  for (uint64_t i = 0; i < count; ++i)
  {
    audio->recordPacketPhase += rateQ16;
    const uint32_t frames = (uint32_t)(audio->recordPacketPhase /
        USB_AUDIO_RECORD_RATE_DENOMINATOR);
    audio->recordPacketPhase %= USB_AUDIO_RECORD_RATE_DENOMINATOR;
    DEBUG_ASSERT(frames <= USB_AUDIO_RECORD_PACKET_FRAMES);

    const size_t size = frames * USB_AUDIO_RECORD_FRAME_SIZE;
    memset(data, 0, size);

    LG_LOCK(audio->recordLock);
    const int queued = ringbuffer_getCount(audio->recordBuffer);
    if (queued > target)
      ringbuffer_consume(audio->recordBuffer, NULL, queued - target);
    ringbuffer_consume(audio->recordBuffer, data, frames);
    LG_UNLOCK(audio->recordLock);

    packet.length = (uint16_t)size;
    usbredirparser_send_iso_packet(audio->parser,
        audio->recordPacketId++, &packet, data, (int)size);
    audio->recordNextPacketTime +=
      USB_AUDIO_RECORD_PACKET_INTERVAL_NS;
  }
}

static void startISOStream(void * opaque, uint64_t id,
    struct usb_redir_start_iso_stream_header * request)
{
  LG_USBAudio * audio           = getAudio(opaque);
  uint8_t       result          = usb_redir_stall;
  uint32_t      feedbackPrefill = 0;

  if (audio->configuration == USB_AUDIO_CONFIGURATION)
  {
    switch (request->endpoint)
    {
      case USB_AUDIO_PLAYBACK_DATA_ENDPOINT:
        if (!getLayout(audio->playbackAlt))
          break;
        result = usb_redir_success;
        startPlayback(audio);
        break;

      case USB_AUDIO_FEEDBACK_ENDPOINT:
        if (!getLayout(audio->playbackAlt))
          break;
        resetFeedbackRate(audio);
        audio->feedbackStreaming   = true;
        audio->feedbackDataPackets = 0;
        audio->feedbackPacketId     = 0;
        feedbackPrefill =
          (uint32_t)request->pkts_per_urb * request->no_urbs;
        if (!feedbackPrefill)
          feedbackPrefill = 1;
        else if (feedbackPrefill >
            USB_AUDIO_FEEDBACK_MAX_QUEUE)
          feedbackPrefill = USB_AUDIO_FEEDBACK_MAX_QUEUE;
        result = usb_redir_success;
        break;

      case USB_AUDIO_RECORD_DATA_ENDPOINT:
        if (audio->recordAlt != 1)
          break;
        result = usb_redir_success;
        startRecord(audio);
        break;
    }
  }

  sendISOStatus(audio, id, request->endpoint, result);
  if (feedbackPrefill)
    sendFeedbackPackets(audio, feedbackPrefill);
}

static void stopISOStream(void * opaque, uint64_t id,
    struct usb_redir_stop_iso_stream_header * request)
{
  LG_USBAudio * audio = getAudio(opaque);
  uint8_t result = usb_redir_stall;
  switch (request->endpoint)
  {
    case USB_AUDIO_PLAYBACK_DATA_ENDPOINT:
      stopPlayback(audio);
      result = usb_redir_success;
      break;

    case USB_AUDIO_FEEDBACK_ENDPOINT:
      stopFeedback(audio);
      result = usb_redir_success;
      break;

    case USB_AUDIO_RECORD_DATA_ENDPOINT:
      stopRecord(audio);
      result = usb_redir_success;
      break;
  }
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
    case 1: text = "Looking Glass"            ; break;
    case 2: text = "Looking Glass USB Audio"  ; break;
    case 3: text = "LG-UAC2-0006"             ; break;
    case 4: text = "Looking Glass Microphone" ; break;
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
  const uint8_t * response     = NULL;
  size_t          responseSize = 0;
  uint8_t         status       = usb_redir_stall;

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
          /* Full-speed USB cannot carry the maximum PCM24 stream. Expose a
           * functional stereo alternate sized for rates through 96 kHz. */
          const size_t playbackControlSize =
            USB_AUDIO_CONFIGURATION_DESC_SIZE +
            USB_AUDIO_FUNCTION_CONTROL_SIZE;
          memcpy(buffer, l_configurationDescriptor,
              playbackControlSize);
          size_t offset = playbackControlSize;
          memcpy(buffer + offset, l_fullSpeedPlaybackDescriptor,
              sizeof(l_fullSpeedPlaybackDescriptor));
          offset += sizeof(l_fullSpeedPlaybackDescriptor);
          memcpy(buffer + offset,
              l_configurationDescriptor +
                USB_AUDIO_RECORD_FUNCTION_OFFSET,
              USB_AUDIO_FUNCTION_CONTROL_SIZE);
          offset += USB_AUDIO_FUNCTION_CONTROL_SIZE;
          memcpy(buffer + offset, l_fullSpeedRecordDescriptor,
              sizeof(l_fullSpeedRecordDescriptor));
          buffer[1]    = USB_DESCRIPTOR_OTHER_SPEED;
          buffer[2]    = USB_AUDIO_OTHER_SPEED_SIZE & 0xff;
          buffer[3]    = USB_AUDIO_OTHER_SPEED_SIZE >> 8;
          response     = buffer;
          responseSize = USB_AUDIO_OTHER_SPEED_SIZE;
        }
        break;
    }

    if (response)
      status = usb_redir_success;
  }
  else if (request->request == USB_REQUEST_GET_STATUS &&
      request->value == 0 && request->length == sizeof(l_zeroStatus))
  {
    bool valid = false;
    switch (request->requesttype)
    {
      case USB_REQUEST_TYPE_IN_STANDARD_DEVICE:
        valid = request->index == 0;
        break;

      case USB_REQUEST_TYPE_IN_STANDARD_INTERFACE:
        valid = audio->configuration == USB_AUDIO_CONFIGURATION &&
          (request->index == USB_AUDIO_PLAYBACK_CONTROL_IFACE ||
           request->index == USB_AUDIO_PLAYBACK_IFACE ||
           request->index == USB_AUDIO_RECORD_CONTROL_IFACE ||
           request->index == USB_AUDIO_RECORD_IFACE);
        break;

      case USB_REQUEST_TYPE_IN_STANDARD_ENDPOINT:
        valid = request->index == 0x00 || request->index == 0x80 ||
          (audio->configuration == USB_AUDIO_CONFIGURATION &&
           ((getLayout(audio->playbackAlt) &&
             (request->index == USB_AUDIO_PLAYBACK_DATA_ENDPOINT ||
              request->index == USB_AUDIO_FEEDBACK_ENDPOINT)) ||
            (audio->recordAlt == 1 &&
             request->index == USB_AUDIO_RECORD_DATA_ENDPOINT)));
        break;
    }

    if (valid)
    {
      response     = l_zeroStatus;
      responseSize = sizeof(l_zeroStatus);
      status       = usb_redir_success;
    }
  }
  else if (audio->configuration == USB_AUDIO_CONFIGURATION &&
      request->requesttype == USB_REQUEST_TYPE_IN_CLASS_INTERFACE &&
      (request->index ==
         ((USB_AUDIO_PLAYBACK_CLOCK_ID << 8) |
          USB_AUDIO_PLAYBACK_CONTROL_IFACE) ||
       request->index ==
         ((USB_AUDIO_RECORD_CLOCK_ID << 8) |
          USB_AUDIO_RECORD_CONTROL_IFACE)) &&
      (uint8_t)request->value == 0)
  {
    const bool playbackClock = request->index ==
      ((USB_AUDIO_PLAYBACK_CLOCK_ID << 8) |
       USB_AUDIO_PLAYBACK_CONTROL_IFACE);
    const uint8_t control = request->value >> 8;
    if (request->request == USB_REQUEST_CUR &&
        control == USB_AUDIO_CONTROL_FREQUENCY)
    {
      writeLE32(buffer, playbackClock ?
          audio->playbackSampleRate : audio->recordSampleRate);
      response     = buffer;
      responseSize = 4;
      status       = usb_redir_success;
    }
    else if (request->request == USB_REQUEST_RANGE &&
        control == USB_AUDIO_CONTROL_FREQUENCY)
    {
      responseSize = writeSampleRateRange(buffer);
      response     = buffer;
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
  else if (audio->configuration == USB_AUDIO_CONFIGURATION &&
      request->requesttype == USB_REQUEST_TYPE_OUT_CLASS_INTERFACE &&
      request->request == USB_REQUEST_CUR &&
      (request->index ==
         ((USB_AUDIO_PLAYBACK_CLOCK_ID << 8) |
          USB_AUDIO_PLAYBACK_CONTROL_IFACE) ||
       request->index ==
         ((USB_AUDIO_RECORD_CLOCK_ID << 8) |
          USB_AUDIO_RECORD_CONTROL_IFACE)) &&
      request->value == (USB_AUDIO_CONTROL_FREQUENCY << 8) &&
      request->length == 4 && dataLength == 4 && data)
  {
    const uint32_t sampleRate = nearestSampleRate(readLE32(data));
    if (request->index ==
        ((USB_AUDIO_PLAYBACK_CLOCK_ID << 8) |
         USB_AUDIO_PLAYBACK_CONTROL_IFACE))
      setPlaybackSampleRate(audio, sampleRate);
    else
      setRecordSampleRate(audio, sampleRate);
    status = usb_redir_success;
  }

  sendControlResponse(audio, id, request, status, response, responseSize);
  if (data)
    usbredirparser_free_packet_data(audio->parser, data);
}

static void stallPlayback(LG_USBAudio * audio)
{
  stopPlayback(audio);
  sendISOStatus(audio, 0,
      USB_AUDIO_PLAYBACK_DATA_ENDPOINT, usb_redir_stall);
}

static void isoPacket(void * opaque, uint64_t id,
    struct usb_redir_iso_packet_header * packet,
    uint8_t * data, int dataLength)
{
  (void)id;
  LG_USBAudio * audio = getAudio(opaque);
  const USBAudioLayout * layout = getLayout(audio->playbackAlt);

  if (audio->playbackStreaming)
  {
    const uint16_t frameSize =
      layout ? layout->channelCount * USB_AUDIO_SAMPLE_SIZE : 0;
    if (packet->endpoint != USB_AUDIO_PLAYBACK_DATA_ENDPOINT ||
        packet->status != usb_redir_success || dataLength < 0 ||
        packet->length != dataLength ||
        !layout || dataLength > layoutPacketSize(layout) ||
        dataLength % frameSize != 0)
    {
      DEBUG_WARN("Invalid USB audio isochronous packet");
      stallPlayback(audio);
    }
    else
    {
      if (dataLength && audio->events && audio->events->playbackData)
        audio->events->playbackData(audio->eventOpaque, data,
            dataLength / frameSize);

      if (audio->feedbackStreaming &&
          ++audio->feedbackDataPackets == USB_AUDIO_FEEDBACK_INTERVAL)
      {
        audio->feedbackDataPackets = 0;
        sendFeedbackPackets(audio, 1);
      }
    }
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

static void processDevice(void * opaque)
{
  sendRecordPackets(opaque);
}

static const LG_USBRedirDeviceOps l_deviceOps =
{
  .setup   = setupDevice,
  .plug    = plugDevice,
  .process = processDevice,
  .unplug  = unplugDevice,
};

LG_USBAudio * lgUsbAudio_create(
    const LG_USBAudioEventOps * events, void * eventOpaque)
{
  LG_USBAudio * audio = calloc(1, sizeof(*audio));
  if (!audio)
    return NULL;

  audio->events             = events;
  audio->eventOpaque        = eventOpaque;
  audio->playbackSampleRate = LG_USB_AUDIO_DEFAULT_SAMPLE_RATE;
  audio->recordSampleRate   = LG_USB_AUDIO_DEFAULT_SAMPLE_RATE;
  LG_LOCK_INIT(audio->recordLock);
  audio->recordBuffer = ringbuffer_new(
      USB_AUDIO_RECORD_QUEUE_FRAMES, USB_AUDIO_RECORD_FRAME_SIZE);
  if (!audio->recordBuffer)
  {
    free(audio);
    return NULL;
  }

  atomic_init(&audio->feedbackValue,
      encodeFeedbackRate(audio->playbackSampleRate));
  atomic_init(&audio->recordStreaming, false);
  return audio;
}

void lgUsbAudio_setFeedbackRate(LG_USBAudio * audio, double sampleRate)
{
  if (!audio)
    return;

  atomic_store_explicit(&audio->feedbackValue,
      encodeFeedbackRate(sampleRate), memory_order_release);
}

bool lgUsbAudio_recordData(
    LG_USBAudio * audio, const void * data, size_t frames)
{
  if (!audio || frames > INT_MAX || (frames && !data) ||
      !atomic_load_explicit(
        &audio->recordStreaming, memory_order_acquire))
    return false;

  LG_LOCK(audio->recordLock);
  if (!atomic_load_explicit(
        &audio->recordStreaming, memory_order_acquire))
  {
    LG_UNLOCK(audio->recordLock);
    return false;
  }

  const size_t receivedFrames = frames;
  const uint64_t now = recordTime();
  /* Average complete capture batches over time so callback jitter does not
   * become USB packet jitter. recordRateQ16 is frames/second in Q16. */
  const bool resetRate =
    !audio->recordRateLastTime ||
    now - audio->recordRateLastTime > USB_AUDIO_RECORD_RATE_GAP_NS;
  if (resetRate)
  {
    audio->recordRateQ16 =
      (uint64_t)audio->recordSampleRate * USB_AUDIO_RECORD_RATE_SCALE;
    audio->recordRateStartTime = now;
    audio->recordRateFrames    = 0;
  }
  else
    audio->recordRateFrames += receivedFrames;
  audio->recordRateLastTime = now;

  const uint64_t elapsed = now - audio->recordRateStartTime;
  if (elapsed >= USB_AUDIO_RECORD_RATE_SETTLE_NS)
  {
    const double measured =
      (double)audio->recordRateFrames * 1000000000.0 / elapsed;
    if (measured >= audio->recordSampleRate * 0.995 &&
        measured <= audio->recordSampleRate * 1.005)
    {
      const uint64_t measuredQ16 = (uint64_t)(
          measured * USB_AUDIO_RECORD_RATE_SCALE + 0.5);
      audio->recordRateQ16 =
        (audio->recordRateQ16 * 3 + measuredQ16) / 4;
    }

    audio->recordRateStartTime = now;
    audio->recordRateFrames    = 0;
  }

  const int length = ringbuffer_getLength(audio->recordBuffer);
  const uint8_t * input = data;
  bool dropped = false;
  if (frames > (size_t)length)
  {
    input += (frames - length) * USB_AUDIO_RECORD_FRAME_SIZE;
    frames = length;
    ringbuffer_reset(audio->recordBuffer);
    dropped = true;
  }
  else
  {
    const int overflow =
      ringbuffer_getCount(audio->recordBuffer) + (int)frames - length;
    if (overflow > 0)
    {
      ringbuffer_consume(audio->recordBuffer, NULL, overflow);
      dropped = true;
    }
  }

  const int appended = ringbuffer_append(
      audio->recordBuffer, input, (int)frames);
  LG_UNLOCK(audio->recordLock);
  return !dropped && appended == (int)frames;
}

bool lgUsbAudio_recording(const LG_USBAudio * audio)
{
  return audio && atomic_load_explicit(
      &audio->recordStreaming, memory_order_acquire);
}

void lgUsbAudio_destroy(LG_USBAudio * audio)
{
  if (!audio)
    return;

  ringbuffer_free(&audio->recordBuffer);
  LG_LOCK_FREE(audio->recordLock);
  free(audio);
}

const LG_USBRedirDeviceOps * lgUsbAudio_deviceOps(void)
{
  return &l_deviceOps;
}
