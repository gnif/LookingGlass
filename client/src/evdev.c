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

#include "evdev.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "app_internal.h"
#include "core.h"
#include "input.h"

#include "common/debug.h"
#include "common/locking.h"
#include "common/option.h"
#include "common/stringlist.h"
#include "common/thread.h"
#include "common/time.h"

#define EVDEV_BITS_SIZE(count) (((count) + 7) / 8)
#define EVDEV_QUEUE_SIZE       2048
#define EVDEV_RETRY_INITIAL_US UINT64_C(250000)
#define EVDEV_RETRY_MAX_US     UINT64_C(5000000)
#define EVDEV_GRAB_RETRY_US    UINT64_C(1000000)
#define EVDEV_READ_BATCHES     16
#define EVDEV_EVENT_DRAINS     16
#define EVDEV_MOTION_LIMIT     (INT_MAX / 2)
#define EVDEV_WHEEL_LIMIT      64
#define EVDEV_GUEST_LEDS_VALID UINT8_C(0x80)

enum EvdevGrabReason
{
  EVDEV_GRAB_KEYBOARD = 1 << 0,
  EVDEV_GRAB_POINTER  = 1 << 1,
};

#define EVDEV_GRAB_MASK           (EVDEV_GRAB_KEYBOARD | EVDEV_GRAB_POINTER)
#define EVDEV_GENERATION_BITS     31
#define EVDEV_GENERATION_MASK     ((UINT64_C(1) << EVDEV_GENERATION_BITS) - 1)
#define EVDEV_KEYBOARD_GEN_SHIFT  2
#define EVDEV_POINTER_GEN_SHIFT   \
  (EVDEV_KEYBOARD_GEN_SHIFT + EVDEV_GENERATION_BITS)

enum EvdevOutputType
{
  EVDEV_OUTPUT_RESET,
  EVDEV_OUTPUT_KEY,
  EVDEV_OUTPUT_BUTTON,
  EVDEV_OUTPUT_MOTION,
  EVDEV_OUTPUT_WHEEL,
  EVDEV_OUTPUT_BARRIER,
};

typedef struct
{
  uint64_t     serial;
  uint64_t     desiredState;
  uint64_t     keyboardTopology;
  uint64_t     pointerTopology;
  unsigned int lanes;
  unsigned int activate;
  unsigned int deactivate;
  unsigned int handoff;
}
EvdevBarrier;

typedef struct
{
  enum EvdevOutputType type;
  uint64_t             generation;
  union
  {
    struct
    {
      unsigned int code;
      bool         down;
    }
    key;

    struct
    {
      unsigned int button;
      bool         down;
    }
    button;

    struct
    {
      int x;
      int y;
    }
    motion;

    struct
    {
      int vertical;
      int horizontal;
    }
    wheel;

    EvdevBarrier barrier;
  };
}
EvdevOutput;

typedef struct
{
  char    * path;
  int       fd;
  bool      writable;
  bool      keyboard;
  bool      pointer;
  bool      absolutePointer;
  uint8_t   ledMask;
  bool      hasWheelHiRes;
  bool      hasHWheelHiRes;
  bool      grabbed;
  bool      dropping;
  bool      openErrorReported;
  bool      combinedWarned;
  bool      hostLEDsValid;
  bool      ledOverride;
  bool      ledDirty;
  uint8_t   hostLEDs;
  uint8_t   observedLEDs;
  uint8_t   appliedLEDs;
  uint8_t   keys[EVDEV_BITS_SIZE(KEY_CNT)];
  uint8_t   forwarded[EVDEV_BITS_SIZE(KEY_CNT)];
  int       mouseX;
  int       mouseY;
  int       wheelHiRes;
  int       hwheelHiRes;
  uint64_t  retryAt;
  uint64_t  retryDelay;
  uint64_t  grabRetryAt;
  uint64_t  keyboardGeneration;
  uint64_t  pointerGeneration;
}
EvdevDevice;

struct EvdevState
{
  char                    * deviceList;
  EvdevDevice             * devices;
  int                       deviceCount;
  bool                      exclusive;
  unsigned int              keys[KEY_CNT];
  unsigned int              forwarded[KEY_CNT];

  int                       epoll;
  int                       commandEvent;
  int                       outputEvent;
  LGThread                * thread;
  uint64_t                  observedState;
  uint64_t                  nextBarrier;
  EvdevBarrier              waitingBarrier;
  unsigned int              needsBarrierLanes;
  unsigned int              readyDeactivateLanes;
  atomic_uint_fast64_t      desiredState;
  atomic_uint_fast64_t      barrierAck;
  atomic_uint_fast64_t      keyboardTopology;
  atomic_uint_fast64_t      pointerTopology;
  atomic_bool               keyboardActive;
  atomic_bool               pointerActive;
  atomic_bool               stop;
  atomic_bool               availabilityDirty;
  atomic_uchar              guestLEDState;
  bool                      ledsListenerRegistered;
  bool                      availabilityListenerRegistered;
  unsigned int              forwardingLanes;

  LG_Lock                   outputLock;
  bool                      outputLockInitialized;
  EvdevOutput               output[EVDEV_QUEUE_SIZE];
  unsigned int              outputHead;
  unsigned int              outputCount;
  unsigned int              outputOverflows;

  bool                      dispatchKeys[KEY_MAX];
  uint32_t                  dispatchButtons;
  bool                      horizontalWheelWarned;
};

static struct EvdevState state =
{
  .epoll        = -1,
  .commandEvent = -1,
  .outputEvent  = -1,
};

static struct Option options[] =
{
  {
    .module         = "input",
    .name           = "evdev",
    .description    = "csv list of evdev input devices to use "
      "for capture mode (ie: /dev/input/by-id/usb-some_device-event-kbd)",
    .type           = OPTION_TYPE_STRING,
    .value.x_string = NULL,
  },
  {
    .module       = "input",
    .name         = "evdevExclusive",
    .description  = "Only use evdev devices for input when in capture mode",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = true
  },
  {0}
};

static bool evdev_bit(const uint8_t * bits, unsigned int bit)
{
  return bits[bit / 8] & (UINT8_C(1) << (bit % 8));
}

static void evdev_setBit(uint8_t * bits, unsigned int bit, bool value)
{
  const uint8_t mask = UINT8_C(1) << (bit % 8);
  if (value)
    bits[bit / 8] |= mask;
  else
    bits[bit / 8] &= ~mask;
}

static int evdev_addLimited(int value, int delta, int limit)
{
  const int64_t result = (int64_t)value + delta;
  if (result > limit)
    return limit;
  if (result < -limit)
    return -limit;
  return result;
}

static uint64_t evdev_generation(uint64_t desiredState,
    unsigned int lane)
{
  const unsigned int shift = lane == EVDEV_GRAB_KEYBOARD ?
    EVDEV_KEYBOARD_GEN_SHIFT : EVDEV_POINTER_GEN_SHIFT;
  return (desiredState >> shift) & EVDEV_GENERATION_MASK;
}

static uint64_t evdev_updateDesired(uint64_t current,
    unsigned int reasons, unsigned int lanes)
{
  uint64_t next = (current & ~EVDEV_GRAB_MASK) | reasons;
  for(unsigned int lane = EVDEV_GRAB_KEYBOARD;
      lane <= EVDEV_GRAB_POINTER; lane <<= 1)
  {
    if (!(lanes & lane))
      continue;

    const unsigned int shift = lane == EVDEV_GRAB_KEYBOARD ?
      EVDEV_KEYBOARD_GEN_SHIFT : EVDEV_POINTER_GEN_SHIFT;
    const uint64_t generation =
      (evdev_generation(current, lane) + 1) & EVDEV_GENERATION_MASK;
    next &= ~(EVDEV_GENERATION_MASK << shift);
    next |= generation << shift;
  }

  return next;
}

static bool evdev_laneStateMatches(uint64_t current, uint64_t expected,
    unsigned int lanes)
{
  for(unsigned int lane = EVDEV_GRAB_KEYBOARD;
      lane <= EVDEV_GRAB_POINTER; lane <<= 1)
    if ((lanes & lane) &&
        (((current ^ expected) & lane) ||
         evdev_generation(current, lane) !=
           evdev_generation(expected, lane)))
      return false;

  return true;
}

static unsigned int evdev_barrierStateLanes(
    const EvdevBarrier * barrier)
{
  return barrier->lanes | barrier->activate |
    barrier->deactivate | barrier->handoff;
}

static bool evdev_barrierMatches(
    const EvdevBarrier * barrier, uint64_t desiredState)
{
  const unsigned int lanes = evdev_barrierStateLanes(barrier);
  if (!evdev_laneStateMatches(
        desiredState, barrier->desiredState, lanes))
    return false;

  if ((lanes & EVDEV_GRAB_KEYBOARD) &&
      atomic_load_explicit(&state.keyboardTopology,
        memory_order_acquire) != barrier->keyboardTopology)
    return false;
  if ((lanes & EVDEV_GRAB_POINTER) &&
      atomic_load_explicit(&state.pointerTopology,
        memory_order_acquire) != barrier->pointerTopology)
    return false;

  return true;
}

static uint64_t evdev_nextBarrier(void)
{
  uint64_t serial = ++state.nextBarrier;
  if (!serial)
    serial = ++state.nextBarrier;
  return serial;
}

static void evdev_signal(int fd, const char * name);
static unsigned int evdev_deviceLanes(const EvdevDevice * device);
static unsigned int evdev_deviceForwardingLanes(
    const EvdevDevice * device);

static uint8_t evdev_convertLEDs(const uint8_t * leds)
{
  uint8_t result = 0;
  if (evdev_bit(leds, LED_NUML))
    result |= LG_KEYBOARD_LED_NUM_LOCK;
  if (evdev_bit(leds, LED_CAPSL))
    result |= LG_KEYBOARD_LED_CAPS_LOCK;
  if (evdev_bit(leds, LED_SCROLLL))
    result |= LG_KEYBOARD_LED_SCROLL_LOCK;
  if (evdev_bit(leds, LED_COMPOSE))
    result |= LG_KEYBOARD_LED_COMPOSE;
  if (evdev_bit(leds, LED_KANA))
    result |= LG_KEYBOARD_LED_KANA;
  return result;
}

static bool evdev_readLEDs(EvdevDevice * device, uint8_t * result)
{
  uint8_t leds[EVDEV_BITS_SIZE(LED_CNT)] = {0};
  if (ioctl(device->fd, EVIOCGLED(sizeof(leds)), leds) < 0)
  {
    DEBUG_WARN("Failed to query keyboard LEDs for %s: %s",
        device->path, strerror(errno));
    return false;
  }

  *result = evdev_convertLEDs(leds);
  return true;
}

static bool evdev_writeLEDs(EvdevDevice * device, uint8_t leds)
{
  struct input_event events[6] = {0};
  int count = 0;
  if (device->ledMask & LG_KEYBOARD_LED_NUM_LOCK)
    events[count++] = (struct input_event)
    {
      .type  = EV_LED,
      .code  = LED_NUML,
      .value = !!(leds & LG_KEYBOARD_LED_NUM_LOCK),
    };
  if (device->ledMask & LG_KEYBOARD_LED_CAPS_LOCK)
    events[count++] = (struct input_event)
    {
      .type  = EV_LED,
      .code  = LED_CAPSL,
      .value = !!(leds & LG_KEYBOARD_LED_CAPS_LOCK),
    };
  if (device->ledMask & LG_KEYBOARD_LED_SCROLL_LOCK)
    events[count++] = (struct input_event)
    {
      .type  = EV_LED,
      .code  = LED_SCROLLL,
      .value = !!(leds & LG_KEYBOARD_LED_SCROLL_LOCK),
    };
  if (device->ledMask & LG_KEYBOARD_LED_COMPOSE)
    events[count++] = (struct input_event)
    {
      .type  = EV_LED,
      .code  = LED_COMPOSE,
      .value = !!(leds & LG_KEYBOARD_LED_COMPOSE),
    };
  if (device->ledMask & LG_KEYBOARD_LED_KANA)
    events[count++] = (struct input_event)
    {
      .type  = EV_LED,
      .code  = LED_KANA,
      .value = !!(leds & LG_KEYBOARD_LED_KANA),
    };
  events[count++] = (struct input_event)
  {
    .type = EV_SYN,
    .code = SYN_REPORT,
  };

  const size_t expected = count * sizeof(*events);
  const ssize_t size = write(device->fd, events, expected);
  if (size == (ssize_t)expected)
    return true;

  DEBUG_WARN("Failed to set keyboard LEDs for %s: %s", device->path,
      size < 0 ? strerror(errno) : "incomplete write");
  return false;
}

static void evdev_restoreLEDs(EvdevDevice * device)
{
  if (!device->ledOverride)
    return;

  if (device->hostLEDsValid && device->writable &&
      !evdev_writeLEDs(device, device->hostLEDs))
    return;

  if (device->hostLEDsValid)
    device->observedLEDs = device->hostLEDs;
  device->ledOverride = false;
  device->ledDirty    = false;
  device->appliedLEDs = UINT8_MAX;
}

static bool evdev_anyLEDOverride(const EvdevDevice * except)
{
  for(int i = 0; i < state.deviceCount; ++i)
  {
    const EvdevDevice * device = &state.devices[i];
    if (device != except && device->ledOverride)
      return true;
  }

  return false;
}

static void evdev_updateLEDs(EvdevDevice * device)
{
  const uint8_t guestState = atomic_load_explicit(
      &state.guestLEDState, memory_order_acquire);
  const bool    valid      = guestState & EVDEV_GUEST_LEDS_VALID;
  const uint8_t leds       = guestState & ~EVDEV_GUEST_LEDS_VALID;
  if (!device->grabbed || !device->keyboard || !device->ledMask ||
      !device->writable || !valid)
  {
    evdev_restoreLEDs(device);

    if (!device->grabbed && device->keyboard && device->ledMask &&
        !device->hostLEDsValid && !evdev_anyLEDOverride(device) &&
        evdev_readLEDs(device, &device->hostLEDs))
    {
      device->observedLEDs  = device->hostLEDs;
      device->hostLEDsValid = true;
    }
    return;
  }

  if (!device->ledOverride)
  {
    /* The host state is tracked while the device is ungrabbed. Do not query
     * it after another event node may already have applied the guest state. */
    if (!device->hostLEDsValid)
      return;

    device->ledOverride = true;
    device->ledDirty    = false;
    device->appliedLEDs = UINT8_MAX;
  }

  if ((device->appliedLEDs != leds || device->ledDirty) &&
      evdev_writeLEDs(device, leds))
  {
    device->appliedLEDs = leds;
    device->ledDirty    = false;
  }
}

static void evdev_keyboardLEDs(void * opaque, bool valid, uint8_t leds)
{
  (void)opaque;
  leds &= LG_KEYBOARD_LED_NUM_LOCK    |
          LG_KEYBOARD_LED_CAPS_LOCK   |
          LG_KEYBOARD_LED_SCROLL_LOCK |
          LG_KEYBOARD_LED_COMPOSE     |
          LG_KEYBOARD_LED_KANA;
  atomic_store_explicit(&state.guestLEDState,
      leds | (valid ? EVDEV_GUEST_LEDS_VALID : 0), memory_order_release);
  evdev_signal(state.commandEvent, "command");
}

static void evdev_availabilityChanged(
    void * opaque, bool available)
{
  (void)opaque;
  (void)available;
  atomic_store_explicit(&state.availabilityDirty, true,
      memory_order_release);
  evdev_signal(state.outputEvent, "output");
}

static void evdev_signal(int fd, const char * name)
{
  if (fd < 0)
    return;

  const uint64_t value = 1;
  if (write(fd, &value, sizeof(value)) < 0 &&
      errno != EAGAIN && errno != EBADF)
    DEBUG_WARN("Failed to signal the evdev %s event: %s",
        name, strerror(errno));
}

static void evdev_drainEvent(int fd)
{
  uint64_t value;
  for(unsigned int i = 0; i < EVDEV_EVENT_DRAINS; ++i)
  {
    const ssize_t size = read(fd, &value, sizeof(value));
    if (size == (ssize_t)sizeof(value))
      return;
    if (size >= 0 || errno != EINTR)
      return;
  }
}

void evdev_earlyInit(void)
{
  option_register(options);
}

static bool evdev_isMouseButton(unsigned int code)
{
  return code >= BTN_MOUSE && code <= BTN_TASK;
}

static unsigned int evdev_codeLane(unsigned int code)
{
  return evdev_isMouseButton(code) ?
    EVDEV_GRAB_POINTER : EVDEV_GRAB_KEYBOARD;
}

static void evdev_clearForwarded(void)
{
  memset(state.forwarded, 0, sizeof(state.forwarded));
  for(int i = 0; i < state.deviceCount; ++i)
    memset(state.devices[i].forwarded, 0,
        sizeof(state.devices[i].forwarded));
}

static bool evdev_queueOutput(const EvdevOutput * output)
{
  bool overflow = false;

  LG_LOCK(state.outputLock);
  if (state.outputCount)
  {
    const unsigned int lastIndex =
      (state.outputHead + state.outputCount - 1) % EVDEV_QUEUE_SIZE;
    EvdevOutput * last = &state.output[lastIndex];

    if (last->type == output->type &&
        last->generation == output->generation &&
        output->type == EVDEV_OUTPUT_MOTION)
    {
      last->motion.x = evdev_addLimited(last->motion.x,
          output->motion.x, EVDEV_MOTION_LIMIT);
      last->motion.y = evdev_addLimited(last->motion.y,
          output->motion.y, EVDEV_MOTION_LIMIT);
      LG_UNLOCK(state.outputLock);
      evdev_signal(state.outputEvent, "output");
      return true;
    }

    if (last->type == output->type &&
        last->generation == output->generation &&
        output->type == EVDEV_OUTPUT_WHEEL)
    {
      last->wheel.vertical = evdev_addLimited(last->wheel.vertical,
          output->wheel.vertical, EVDEV_WHEEL_LIMIT);
      last->wheel.horizontal = evdev_addLimited(last->wheel.horizontal,
          output->wheel.horizontal, EVDEV_WHEEL_LIMIT);
      LG_UNLOCK(state.outputLock);
      evdev_signal(state.outputEvent, "output");
      return true;
    }
  }

  if (state.outputCount == EVDEV_QUEUE_SIZE)
  {
    state.outputHead = 0;
    if (state.waitingBarrier.serial)
    {
      const uint64_t serial = evdev_nextBarrier();
      state.waitingBarrier.serial = serial;
      state.outputCount = 2;
      state.output[0] = (EvdevOutput)
      {
        .type    = EVDEV_OUTPUT_RESET,
        .barrier = { .lanes = EVDEV_GRAB_MASK },
      };
      state.output[1] = (EvdevOutput)
      {
        .type    = EVDEV_OUTPUT_BARRIER,
        .barrier = state.waitingBarrier,
      };
    }
    else
    {
      state.outputCount = 1;
      state.output[0].type = EVDEV_OUTPUT_RESET;
      state.output[0].barrier.lanes = EVDEV_GRAB_MASK;
    }
    ++state.outputOverflows;
    overflow = true;
  }
  else
  {
    const unsigned int index =
      (state.outputHead + state.outputCount) % EVDEV_QUEUE_SIZE;
    state.output[index] = *output;
    ++state.outputCount;
  }
  LG_UNLOCK(state.outputLock);

  if (overflow)
  {
    evdev_clearForwarded();
    DEBUG_WARN("evdev output queue overflowed; input state was reset");
  }

  evdev_signal(state.outputEvent, "output");
  return !overflow;
}

static void evdev_queueKey(
    const EvdevDevice * device, unsigned int code, bool down)
{
  if (evdev_isMouseButton(code))
  {
    static const unsigned int map[] = {1, 3, 2, 6, 7, 8, 9, 10};
    const EvdevOutput output =
    {
      .type       = EVDEV_OUTPUT_BUTTON,
      .generation = device->pointerGeneration,
      .button     =
      {
        .button = map[code - BTN_MOUSE],
        .down   = down,
      },
    };
    evdev_queueOutput(&output);
    return;
  }

  /* The application key-state arrays intentionally exclude KEY_MAX. */
  if (code == KEY_RESERVED || code >= BTN_MISC || code >= KEY_MAX)
    return;

  const EvdevOutput output =
  {
    .type       = EVDEV_OUTPUT_KEY,
    .generation = device->keyboardGeneration,
    .key        =
    {
      .code = code,
      .down = down,
    },
  };
  evdev_queueOutput(&output);
}

static void evdev_cancelOutput(void)
{
  state.waitingBarrier       = (EvdevBarrier){0};
  state.needsBarrierLanes    = 0;
  state.readyDeactivateLanes = 0;

  LG_LOCK(state.outputLock);
  state.outputHead  = 0;
  state.outputCount = 1;
  state.output[0]   = (EvdevOutput)
  {
    .type    = EVDEV_OUTPUT_RESET,
    .barrier = { .lanes = EVDEV_GRAB_MASK },
  };
  LG_UNLOCK(state.outputLock);

  evdev_clearForwarded();
  evdev_signal(state.outputEvent, "output");
}

static void evdev_updateKey(EvdevDevice * device, unsigned int code,
    bool down, bool forward)
{
  if (code >= KEY_CNT)
    return;

  const bool wasDown = evdev_bit(device->keys, code);
  if (wasDown != down)
  {
    evdev_setBit(device->keys, code, down);
    if (down)
      ++state.keys[code];
    else if (state.keys[code])
      --state.keys[code];
  }

  const bool wasForwarded = evdev_bit(device->forwarded, code);
  if (down && forward)
  {
    if (wasForwarded)
      return;

    evdev_setBit(device->forwarded, code, true);
    if (++state.forwarded[code] == 1)
      evdev_queueKey(device, code, true);
    return;
  }

  if (!wasForwarded)
    return;

  evdev_setBit(device->forwarded, code, false);
  if (state.forwarded[code] && --state.forwarded[code] == 0)
    evdev_queueKey(device, code, false);
}

static bool evdev_syncKeys(
    EvdevDevice * device, unsigned int forwardLanes)
{
  uint8_t keys[EVDEV_BITS_SIZE(KEY_CNT)] = {0};
  if (ioctl(device->fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
  {
    if (errno != ENODEV)
      DEBUG_WARN("Failed to query key state for %s: %s",
          device->path, strerror(errno));
    return false;
  }

  for(unsigned int code = 0; code < KEY_CNT; ++code)
    evdev_updateKey(device, code, evdev_bit(keys, code),
        forwardLanes & evdev_codeLane(code));

  return true;
}

static void evdev_resetTransient(EvdevDevice * device)
{
  device->mouseX      = 0;
  device->mouseY      = 0;
  device->wheelHiRes  = 0;
  device->hwheelHiRes = 0;
  device->dropping    = false;
}

static void evdev_releaseState(EvdevDevice * device, bool forward)
{
  for(unsigned int code = 0; code < KEY_CNT; ++code)
  {
    if (evdev_bit(device->forwarded, code))
    {
      evdev_setBit(device->forwarded, code, false);
      if (state.forwarded[code] && --state.forwarded[code] == 0 && forward)
        evdev_queueKey(device, code, false);
    }

    if (evdev_bit(device->keys, code))
    {
      evdev_setBit(device->keys, code, false);
      if (state.keys[code])
        --state.keys[code];
    }
  }

  evdev_resetTransient(device);
}

static bool evdev_lanePublished(unsigned int lane)
{
  if (atomic_load_explicit(&state.stop, memory_order_acquire))
    return false;

  const atomic_bool * active = lane == EVDEV_GRAB_KEYBOARD ?
    &state.keyboardActive : &state.pointerActive;
  return atomic_load_explicit(active, memory_order_acquire);
}

static void evdev_publishActive(unsigned int lanes, bool active)
{
  if (lanes & EVDEV_GRAB_KEYBOARD)
    atomic_store_explicit(&state.keyboardActive, active,
        memory_order_release);
  if (lanes & EVDEV_GRAB_POINTER)
    atomic_store_explicit(&state.pointerActive, active,
        memory_order_release);
}

static void evdev_topologyChanged(unsigned int lanes)
{
  if (!lanes)
    return;

  if (lanes & EVDEV_GRAB_KEYBOARD)
    atomic_fetch_add_explicit(
        &state.keyboardTopology, 1, memory_order_acq_rel);
  if (lanes & EVDEV_GRAB_POINTER)
    atomic_fetch_add_explicit(
        &state.pointerTopology, 1, memory_order_acq_rel);
  state.readyDeactivateLanes &= ~lanes;
  state.needsBarrierLanes    |=  lanes;
}

static void evdev_closeDevice(EvdevDevice * device, bool removed)
{
  if (device->fd < 0)
    return;

  if (removed)
    DEBUG_WARN("Device was removed: %s", device->path);

  const unsigned int lanes = evdev_deviceLanes(device);
  evdev_releaseState(device, true);
  evdev_restoreLEDs(device);
  epoll_ctl(state.epoll, EPOLL_CTL_DEL, device->fd, NULL);
  close(device->fd);

  device->fd              = -1;
  device->writable        = false;
  device->keyboard        = false;
  device->pointer         = false;
  device->absolutePointer = false;
  device->ledMask         = 0;
  device->grabbed         = false;
  device->hostLEDsValid   = false;
  device->ledOverride     = false;
  device->ledDirty        = false;
  device->observedLEDs    = 0;
  device->appliedLEDs     = UINT8_MAX;
  device->grabRetryAt     = 0;
  device->retryAt         = microtime() + EVDEV_RETRY_INITIAL_US;
  device->retryDelay      = EVDEV_RETRY_INITIAL_US;
  evdev_topologyChanged(lanes);
}

static void evdev_scheduleRetry(EvdevDevice * device, uint64_t now)
{
  device->retryAt = now + device->retryDelay;
  device->retryDelay *= 2;
  if (device->retryDelay > EVDEV_RETRY_MAX_US)
    device->retryDelay = EVDEV_RETRY_MAX_US;
}

static bool evdev_queryCapabilities(EvdevDevice * device)
{
  uint8_t events[EVDEV_BITS_SIZE(EV_CNT)] = {0};
  uint8_t keys  [EVDEV_BITS_SIZE(KEY_CNT)] = {0};
  uint8_t rel   [EVDEV_BITS_SIZE(REL_CNT)] = {0};
  uint8_t abs   [EVDEV_BITS_SIZE(ABS_CNT)] = {0};
  uint8_t leds  [EVDEV_BITS_SIZE(LED_CNT)] = {0};

  if (ioctl(device->fd, EVIOCGBIT(0, sizeof(events)), events) < 0)
  {
    DEBUG_ERROR("Failed to query capabilities for %s: %s",
        device->path, strerror(errno));
    return false;
  }

  if (evdev_bit(events, EV_KEY) &&
      ioctl(device->fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0)
  {
    DEBUG_ERROR("Failed to query keys for %s: %s",
        device->path, strerror(errno));
    return false;
  }

  if (evdev_bit(events, EV_REL) &&
      ioctl(device->fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) < 0)
  {
    DEBUG_ERROR("Failed to query relative axes for %s: %s",
        device->path, strerror(errno));
    return false;
  }

  if (evdev_bit(events, EV_ABS) &&
      ioctl(device->fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0)
  {
    DEBUG_ERROR("Failed to query absolute axes for %s: %s",
        device->path, strerror(errno));
    return false;
  }

  if (evdev_bit(events, EV_LED) &&
      ioctl(device->fd, EVIOCGBIT(EV_LED, sizeof(leds)), leds) < 0)
  {
    DEBUG_ERROR("Failed to query LEDs for %s: %s",
        device->path, strerror(errno));
    return false;
  }

  device->keyboard = false;
  for(unsigned int code = KEY_ESC; code < BTN_MISC; ++code)
    if (evdev_bit(keys, code))
    {
      device->keyboard = true;
      break;
    }

  device->hasWheelHiRes  = evdev_bit(rel, REL_WHEEL_HI_RES);
  device->hasHWheelHiRes = evdev_bit(rel, REL_HWHEEL_HI_RES);
  const bool relativePointer =
    evdev_bit(rel, REL_X) || evdev_bit(rel, REL_Y);
  const bool wheelPointer =
    evdev_bit(rel, REL_WHEEL)          ||
    evdev_bit(rel, REL_HWHEEL)         ||
    device->hasWheelHiRes              ||
    device->hasHWheelHiRes;
  device->absolutePointer =
    evdev_bit(abs, ABS_X) || evdev_bit(abs, ABS_Y);
  device->pointer = relativePointer || wheelPointer;
  for(unsigned int code = BTN_MOUSE; code <= BTN_TASK; ++code)
    device->pointer = device->pointer || evdev_bit(keys, code);
  if (device->absolutePointer && !relativePointer)
  {
    device->pointer = false;
    DEBUG_WARN("Absolute evdev pointer will be handled by the display "
        "server: %s", device->path);
  }

  device->ledMask        = 0;
  if (evdev_bit(leds, LED_NUML))
    device->ledMask |= LG_KEYBOARD_LED_NUM_LOCK;
  if (evdev_bit(leds, LED_CAPSL))
    device->ledMask |= LG_KEYBOARD_LED_CAPS_LOCK;
  if (evdev_bit(leds, LED_SCROLLL))
    device->ledMask |= LG_KEYBOARD_LED_SCROLL_LOCK;
  if (evdev_bit(leds, LED_COMPOSE))
    device->ledMask |= LG_KEYBOARD_LED_COMPOSE;
  if (evdev_bit(leds, LED_KANA))
    device->ledMask |= LG_KEYBOARD_LED_KANA;

  const char * type = device->keyboard && device->pointer ?
    "keyboard, pointer" :
    device->keyboard && device->absolutePointer ?
    "keyboard, absolute pointer" : device->keyboard ? "keyboard" :
    device->pointer ? "pointer" : device->absolutePointer ?
    "absolute pointer" : "other";
  DEBUG_INFO("Opened %s (%s)", device->path, type);
  return true;
}

static bool evdev_openDevice(EvdevDevice * device, uint64_t now)
{
  if (device->fd >= 0 || now < device->retryAt)
    return device->fd >= 0;

  device->fd       = open(device->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  device->writable = device->fd >= 0;
  if (device->fd < 0)
  {
    device->fd       = open(device->path,
        O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    device->writable = false;
  }

  if (device->fd < 0)
  {
    if (!device->openErrorReported)
    {
      DEBUG_ERROR("Unable to open %s (%s)", device->path, strerror(errno));
      device->openErrorReported = true;
    }

    evdev_scheduleRetry(device, now);
    return false;
  }

  if (!evdev_queryCapabilities(device))
    goto err;

  struct epoll_event event =
  {
    .events   = EPOLLIN | EPOLLERR | EPOLLHUP,
    .data.ptr = device
  };

  if (epoll_ctl(state.epoll, EPOLL_CTL_ADD, device->fd, &event) != 0)
  {
    DEBUG_ERROR("Failed to add %s to epoll: %s",
        device->path, strerror(errno));
    goto err;
  }

  device->openErrorReported = false;
  device->retryAt           = 0;
  device->retryDelay        = EVDEV_RETRY_INITIAL_US;
  if (!evdev_syncKeys(device, 0))
    goto errEpoll;
  if (device->ledMask && !evdev_anyLEDOverride(device))
  {
    device->hostLEDsValid = evdev_readLEDs(device, &device->hostLEDs);
    if (device->hostLEDsValid)
      device->observedLEDs = device->hostLEDs;
  }
  else
    device->hostLEDsValid = false;

  evdev_topologyChanged(evdev_deviceLanes(device));
  return true;

errEpoll:
  epoll_ctl(state.epoll, EPOLL_CTL_DEL, device->fd, NULL);
err:
  close(device->fd);
  device->fd       = -1;
  device->writable = false;
  evdev_scheduleRetry(device, now);
  return false;
}

static bool evdev_neutral(const EvdevDevice * device)
{
  for(unsigned int code = 0; code < KEY_CNT; ++code)
    if (evdev_bit(device->keys, code))
      return false;

  return true;
}

static bool evdev_shouldGrab(const EvdevDevice * device,
    unsigned int reasons)
{
  const bool combined = device->keyboard &&
    (device->pointer || device->absolutePointer);
  if (combined && (reasons & EVDEV_GRAB_MASK) != EVDEV_GRAB_MASK)
    return false;

  if ((reasons & EVDEV_GRAB_POINTER) && device->pointer)
    return true;

  if ((reasons & EVDEV_GRAB_KEYBOARD) && device->keyboard)
    return !device->absolutePointer;

  return false;
}

static unsigned int evdev_deviceLanes(const EvdevDevice * device)
{
  return
    (device->keyboard ? EVDEV_GRAB_KEYBOARD : 0) |
    ((device->pointer || device->absolutePointer) ?
      EVDEV_GRAB_POINTER : 0);
}

static void evdev_setDeviceGenerations(EvdevDevice * device,
    uint64_t desiredState, unsigned int lanes)
{
  const unsigned int deviceLanes = evdev_deviceLanes(device);
  if ((lanes & deviceLanes) & EVDEV_GRAB_KEYBOARD)
    device->keyboardGeneration =
      evdev_generation(desiredState, EVDEV_GRAB_KEYBOARD);
  if ((lanes & deviceLanes) & EVDEV_GRAB_POINTER)
    device->pointerGeneration =
      evdev_generation(desiredState, EVDEV_GRAB_POINTER);
}

static unsigned int evdev_changedLanes(uint64_t oldState,
    uint64_t newState)
{
  unsigned int lanes = 0;
  for(unsigned int lane = EVDEV_GRAB_KEYBOARD;
      lane <= EVDEV_GRAB_POINTER; lane <<= 1)
    if (!evdev_laneStateMatches(oldState, newState, lane))
      lanes |= lane;
  return lanes;
}

static unsigned int evdev_publishedLanes(void)
{
  return
    (evdev_lanePublished(EVDEV_GRAB_KEYBOARD) ?
      EVDEV_GRAB_KEYBOARD : 0) |
    (evdev_lanePublished(EVDEV_GRAB_POINTER) ?
      EVDEV_GRAB_POINTER : 0);
}

static unsigned int evdev_targetActiveLanes(unsigned int reasons)
{
  bool keyboardSeen   = false;
  bool keyboardActive = true;
  bool pointerSeen    = false;
  bool pointerActive  = true;
  for(int i = 0; i < state.deviceCount; ++i)
  {
    const EvdevDevice * device = &state.devices[i];
    if (device->fd < 0)
      continue;

    const bool active = device->grabbed &&
      evdev_shouldGrab(device, reasons);
    if (device->keyboard)
    {
      keyboardSeen   = true;
      keyboardActive = keyboardActive && active;
    }
    if (device->pointer || device->absolutePointer)
    {
      pointerSeen   = true;
      pointerActive = pointerActive && active;
    }
  }

  return
    (keyboardSeen && keyboardActive && (reasons & EVDEV_GRAB_KEYBOARD) ?
      EVDEV_GRAB_KEYBOARD : 0) |
    (pointerSeen && pointerActive && (reasons & EVDEV_GRAB_POINTER) ?
      EVDEV_GRAB_POINTER : 0);
}

static unsigned int evdev_coupleLanes(
    unsigned int lanes, unsigned int reasons)
{
  unsigned int previous;
  do
  {
    previous = lanes;
    for(int i = 0; i < state.deviceCount; ++i)
    {
      const EvdevDevice * device = &state.devices[i];
      if (device->fd < 0 || !evdev_shouldGrab(device, reasons))
        continue;

      const unsigned int deviceLanes =
        evdev_deviceLanes(device) & reasons;
      if (deviceLanes & ~lanes)
        lanes &= ~deviceLanes;
    }
  }
  while(lanes != previous);

  return lanes;
}

static unsigned int evdev_acquirableLanes(
    unsigned int reasons, uint64_t now)
{
  bool keyboardSeen  = false;
  bool keyboardReady = true;
  bool pointerSeen   = false;
  bool pointerReady  = true;
  for(int i = 0; i < state.deviceCount; ++i)
  {
    const EvdevDevice * device = &state.devices[i];
    if (device->fd < 0)
      continue;

    const bool ready = evdev_shouldGrab(device, reasons) &&
      evdev_neutral(device) &&
      (device->grabbed || now >= device->grabRetryAt);
    if (device->keyboard)
    {
      keyboardSeen  = true;
      keyboardReady = keyboardReady && ready;
    }
    if (device->pointer || device->absolutePointer)
    {
      pointerSeen  = true;
      pointerReady = pointerReady && ready;
    }
  }

  const unsigned int lanes =
    (keyboardSeen && keyboardReady && (reasons & EVDEV_GRAB_KEYBOARD) ?
      EVDEV_GRAB_KEYBOARD : 0) |
    (pointerSeen && pointerReady && (reasons & EVDEV_GRAB_POINTER) ?
      EVDEV_GRAB_POINTER : 0);
  return evdev_coupleLanes(lanes, reasons);
}

static bool evdev_shouldOwn(const EvdevDevice * device,
    unsigned int reasons, unsigned int effective)
{
  if (!evdev_shouldGrab(device, reasons))
    return false;

  return !(evdev_deviceLanes(device) & reasons & ~effective);
}

static unsigned int evdev_deviceForwardingLanes(
    const EvdevDevice * device)
{
  const uint64_t desiredState = atomic_load_explicit(
      &state.desiredState, memory_order_acquire);
  if (!device->grabbed || !evdev_shouldGrab(device,
        desiredState & EVDEV_GRAB_MASK))
    return 0;

  return evdev_deviceLanes(device) & state.forwardingLanes;
}

static bool evdev_lanesNeutral(unsigned int lanes,
    unsigned int reasons)
{
  for(int i = 0; i < state.deviceCount; ++i)
  {
    const EvdevDevice * device = &state.devices[i];
    if (device->fd >= 0 && evdev_shouldGrab(device, reasons) &&
        (evdev_deviceLanes(device) & lanes) && !evdev_neutral(device))
      return false;
  }

  return true;
}

static bool evdev_relinquishNeutral(unsigned int lane,
    unsigned int reasons, unsigned int effective)
{
  for(int i = 0; i < state.deviceCount; ++i)
  {
    const EvdevDevice * device = &state.devices[i];
    if (device->fd >= 0 && device->grabbed &&
        !evdev_shouldOwn(device, reasons, effective) &&
        (evdev_deviceLanes(device) & lane) && !evdev_neutral(device))
      return false;
  }

  return true;
}

static bool evdev_queueBarrier(uint64_t desiredState,
    unsigned int lanes, unsigned int activate, unsigned int deactivate,
    unsigned int handoff)
{
  const EvdevBarrier barrier =
  {
    .serial           = evdev_nextBarrier(),
    .desiredState     = desiredState,
    .keyboardTopology = atomic_load_explicit(
        &state.keyboardTopology, memory_order_acquire),
    .pointerTopology  = atomic_load_explicit(
        &state.pointerTopology, memory_order_acquire),
    .lanes            = lanes,
    .activate         = activate,
    .deactivate       = deactivate,
    .handoff          = handoff,
  };
  const EvdevOutput output =
  {
    .type    = EVDEV_OUTPUT_BARRIER,
    .barrier = barrier,
  };
  if (!evdev_queueOutput(&output))
    return false;

  state.waitingBarrier = barrier;
  return true;
}

static bool evdev_setDeviceGrab(
    EvdevDevice * device, bool grab, uint64_t now)
{
  if (device->fd < 0)
    return false;

  if (device->grabbed == grab)
  {
    device->grabRetryAt = 0;
    return true;
  }

  if (now < device->grabRetryAt)
    return device->grabbed == grab;

  if (!grab)
    evdev_restoreLEDs(device);

  if (ioctl(device->fd, EVIOCGRAB, grab ? (void *)1 : (void *)0) < 0)
  {
    if (errno == ENODEV)
    {
      evdev_closeDevice(device, true);
      return false;
    }

    DEBUG_ERROR("Failed to %s %s: %s", grab ? "grab" : "ungrab",
        device->path, strerror(errno));
    device->grabRetryAt = now + EVDEV_GRAB_RETRY_US;
    return false;
  }

  DEBUG_INFO("%s %s", grab ? "Grabbed" : "Ungrabbed", device->path);
  device->grabbed     = grab;
  device->grabRetryAt = 0;
  evdev_resetTransient(device);
  evdev_updateLEDs(device);
  return true;
}

static void evdev_applyGrab(uint64_t now)
{
  uint64_t     desiredState = atomic_load_explicit(
      &state.desiredState, memory_order_acquire);
  unsigned int reasons      = desiredState & EVDEV_GRAB_MASK;

  if ((reasons & EVDEV_GRAB_MASK) &&
      (reasons & EVDEV_GRAB_MASK) != EVDEV_GRAB_MASK)
    for(int i = 0; i < state.deviceCount; ++i)
    {
      EvdevDevice * device = &state.devices[i];
      if (device->fd >= 0 && device->keyboard &&
          (device->pointer || device->absolutePointer) &&
          !device->combinedWarned)
      {
        DEBUG_WARN("Leaving combined keyboard/pointer device ungrabbed "
            "during partial capture: %s", device->path);
        device->combinedWarned = true;
      }
    }

  for(int i = 0; i < state.deviceCount; ++i)
    if (state.devices[i].fd >= 0)
      evdev_updateLEDs(&state.devices[i]);

  if (state.waitingBarrier.serial)
  {
    if (atomic_load_explicit(&state.barrierAck, memory_order_acquire) !=
        state.waitingBarrier.serial)
      return;

    desiredState = atomic_load_explicit(
        &state.desiredState, memory_order_acquire);
    reasons = desiredState & EVDEV_GRAB_MASK;
    const unsigned int barrierLanes =
      evdev_barrierStateLanes(&state.waitingBarrier);
    if (evdev_barrierMatches(&state.waitingBarrier, desiredState))
      state.readyDeactivateLanes |= state.waitingBarrier.handoff;
    else
    {
      state.readyDeactivateLanes &= ~barrierLanes;
      state.needsBarrierLanes    |=  state.waitingBarrier.lanes;
    }
    state.waitingBarrier = (EvdevBarrier){0};
  }

  if (desiredState != state.observedState)
  {
    const unsigned int changed =
      evdev_changedLanes(state.observedState, desiredState);
    for(int i = 0; i < state.deviceCount; ++i)
    {
      EvdevDevice * device = &state.devices[i];
      if (device->fd >= 0 &&
          !evdev_syncKeys(device,
            evdev_deviceForwardingLanes(device)))
      {
        evdev_closeDevice(device, errno == ENODEV);
        continue;
      }

      if (device->grabbed && evdev_shouldGrab(device, reasons))
        evdev_setDeviceGenerations(device, desiredState,
            changed & reasons);
    }

    state.readyDeactivateLanes &= ~changed;
    state.needsBarrierLanes    |=  changed;
    state.observedState         =  desiredState;
  }

  for(int i = 0; i < state.deviceCount; ++i)
  {
    EvdevDevice * device = &state.devices[i];
    const unsigned int lanes = evdev_deviceLanes(device) & reasons;
    if (device->fd >= 0 &&
        (!device->grabbed || !evdev_shouldGrab(device, reasons) ||
         (lanes & ~state.forwardingLanes)) &&
        !evdev_syncKeys(device, evdev_deviceForwardingLanes(device)))
      evdev_closeDevice(device, errno == ENODEV);
  }

  const unsigned int alreadyActive = evdev_coupleLanes(
      evdev_targetActiveLanes(reasons), reasons);
  const unsigned int acquire = alreadyActive |
    evdev_acquirableLanes(reasons, now);
  for(int i = 0; i < state.deviceCount; ++i)
  {
    EvdevDevice * device = &state.devices[i];
    if (device->fd < 0)
      continue;

    const bool grab = evdev_shouldGrab(device, reasons);
    const unsigned int lanes = evdev_deviceLanes(device) & reasons;
    if (!grab || device->grabbed || (lanes & ~acquire))
      continue;

    if (evdev_setDeviceGrab(device, true, now) && device->grabbed)
    {
      const unsigned int deviceLanes = evdev_deviceLanes(device);
      evdev_setDeviceGenerations(device, desiredState, deviceLanes);
      state.readyDeactivateLanes &= ~deviceLanes;
      state.needsBarrierLanes    |=  deviceLanes;
    }
  }

  const unsigned int effective = evdev_coupleLanes(
      evdev_targetActiveLanes(reasons), reasons);
  state.forwardingLanes = effective;
  for(int i = 0; i < state.deviceCount; ++i)
  {
    EvdevDevice * device = &state.devices[i];
    if (device->fd >= 0 && device->grabbed &&
        !evdev_shouldOwn(device, reasons, effective) &&
        !evdev_syncKeys(device, 0))
      evdev_closeDevice(device, errno == ENODEV);
  }

  bool         toUngrab    = false;
  unsigned int ungrabLanes = 0;
  for(int i = 0; i < state.deviceCount; ++i)
  {
    EvdevDevice * device = &state.devices[i];
    if (device->fd < 0)
      continue;

    const bool grab = evdev_shouldGrab(device, reasons);
    const bool own  = evdev_shouldOwn(device, reasons, effective);
    if (!grab && !device->grabbed)
      device->grabRetryAt = 0;
    if (own && device->grabbed)
      device->grabRetryAt = 0;
    if (!own && device->grabbed)
    {
      toUngrab = true;
      ungrabLanes |= evdev_deviceLanes(device);
    }
  }

  const unsigned int published = evdev_publishedLanes();
  const unsigned int pendingHandoff =
    ungrabLanes & ~state.readyDeactivateLanes;
  unsigned int reset          = 0;
  unsigned int activate       = 0;
  unsigned int deactivate     = published & ~effective;
  unsigned int handoff        = 0;
  unsigned int satisfiedNeeds = 0;
  for(unsigned int lane = EVDEV_GRAB_KEYBOARD;
      lane <= EVDEV_GRAB_POINTER; lane <<= 1)
  {
    const bool desired = reasons & lane;
    const bool active  = effective & lane;
    const bool current = published & lane;
    const bool needed  = state.needsBarrierLanes & lane;

    if (!desired)
    {
      if (needed || current || (pendingHandoff & lane))
        reset |= lane;
    }
    else if (active)
    {
      if ((needed || !current) && evdev_lanesNeutral(lane, reasons))
        reset |= lane;
    }
    else if (current)
      reset |= lane;

    if (reset & lane)
    {
      satisfiedNeeds |= lane;
      if (active)
        activate |= lane;
      else
        deactivate |= lane;
    }

    if (!(pendingHandoff & lane))
      continue;

    if ((!desired && (reset & lane)) ||
        evdev_relinquishNeutral(lane, reasons, effective))
    {
      handoff    |= lane;
      deactivate |= lane;
      if (active)
        activate |= lane;
    }
  }

  if (reset || activate || deactivate || handoff)
  {
    if (!evdev_queueBarrier(desiredState, reset,
          activate, deactivate, handoff))
      return;

    state.needsBarrierLanes &= ~satisfiedNeeds;
    return;
  }

  if (toUngrab)
  {
    if (ungrabLanes & ~state.readyDeactivateLanes)
      return;

    for(int i = 0; i < state.deviceCount; ++i)
    {
      const EvdevDevice * device = &state.devices[i];
      if (device->fd >= 0 && device->grabbed &&
          !evdev_shouldOwn(device, reasons, effective) &&
          !evdev_neutral(device))
        return;
    }

    for(int i = 0; i < state.deviceCount; ++i)
    {
      EvdevDevice * device = &state.devices[i];
      if (device->fd >= 0 && device->grabbed &&
          !evdev_shouldOwn(device, reasons, effective))
        evdev_setDeviceGrab(device, false, now);
    }
  }

  unsigned int remaining = 0;
  for(int i = 0; i < state.deviceCount; ++i)
  {
    const EvdevDevice * device = &state.devices[i];
    if (device->fd >= 0 && device->grabbed &&
        !evdev_shouldOwn(device, reasons, effective))
      remaining |= evdev_deviceLanes(device);
  }
  state.readyDeactivateLanes &= remaining;
}

static void evdev_flushMotion(EvdevDevice * device)
{
  const int x = device->mouseX;
  const int y = device->mouseY;
  device->mouseX = 0;
  device->mouseY = 0;

  if (!(evdev_deviceForwardingLanes(device) & EVDEV_GRAB_POINTER) ||
      (!x && !y))
    return;

  const EvdevOutput output =
  {
    .type       = EVDEV_OUTPUT_MOTION,
    .generation = device->pointerGeneration,
    .motion     = { .x = x, .y = y },
  };
  evdev_queueOutput(&output);
}

static void evdev_queueWheel(
    EvdevDevice * device, int vertical, int horizontal)
{
  if (vertical > EVDEV_WHEEL_LIMIT)
    vertical = EVDEV_WHEEL_LIMIT;
  else if (vertical < -EVDEV_WHEEL_LIMIT)
    vertical = -EVDEV_WHEEL_LIMIT;
  if (horizontal > EVDEV_WHEEL_LIMIT)
    horizontal = EVDEV_WHEEL_LIMIT;
  else if (horizontal < -EVDEV_WHEEL_LIMIT)
    horizontal = -EVDEV_WHEEL_LIMIT;

  if (!vertical && !horizontal)
    return;

  const EvdevOutput output =
  {
    .type       = EVDEV_OUTPUT_WHEEL,
    .generation = device->pointerGeneration,
    .wheel      =
    {
      .vertical   = vertical,
      .horizontal = horizontal,
    },
  };
  evdev_queueOutput(&output);
}

static void evdev_handleRelative(EvdevDevice * device,
    const struct input_event * event)
{
  if (!(evdev_deviceForwardingLanes(device) & EVDEV_GRAB_POINTER))
    return;

  switch(event->code)
  {
    case REL_X:
      device->mouseX = evdev_addLimited(device->mouseX,
          event->value, EVDEV_MOTION_LIMIT);
      break;

    case REL_Y:
      device->mouseY = evdev_addLimited(device->mouseY,
          event->value, EVDEV_MOTION_LIMIT);
      break;

    case REL_WHEEL:
      if (!device->hasWheelHiRes)
      {
        evdev_flushMotion(device);
        evdev_queueWheel(device, event->value, 0);
      }
      break;

    case REL_WHEEL_HI_RES:
    {
      evdev_flushMotion(device);
      const int64_t total = (int64_t)device->wheelHiRes + event->value;
      const int64_t steps64 = total / 120;
      const int steps = steps64 > INT_MAX ? INT_MAX :
        steps64 < INT_MIN ? INT_MIN : (int)steps64;
      device->wheelHiRes = (int)(total % 120);
      evdev_queueWheel(device, steps, 0);
      break;
    }

    case REL_HWHEEL:
      if (!device->hasHWheelHiRes)
      {
        evdev_flushMotion(device);
        evdev_queueWheel(device, 0, event->value);
      }
      break;

    case REL_HWHEEL_HI_RES:
    {
      evdev_flushMotion(device);
      const int64_t total = (int64_t)device->hwheelHiRes + event->value;
      const int64_t steps64 = total / 120;
      const int steps = steps64 > INT_MAX ? INT_MAX :
        steps64 < INT_MIN ? INT_MIN : (int)steps64;
      device->hwheelHiRes = (int)(total % 120);
      evdev_queueWheel(device, 0, steps);
      break;
    }
  }
}

static void evdev_handleKey(EvdevDevice * device,
    const struct input_event * event)
{
  if (event->code >= KEY_CNT || event->value == 2)
    return;

  if (evdev_isMouseButton(event->code))
    evdev_flushMotion(device);

  if (event->value == 0 || event->value == 1)
    evdev_updateKey(device, event->code, event->value == 1,
        evdev_deviceForwardingLanes(device) &
          evdev_codeLane(event->code));
}

static uint8_t evdev_ledForCode(unsigned int code)
{
  switch(code)
  {
    case LED_NUML:
      return LG_KEYBOARD_LED_NUM_LOCK;
    case LED_CAPSL:
      return LG_KEYBOARD_LED_CAPS_LOCK;
    case LED_SCROLLL:
      return LG_KEYBOARD_LED_SCROLL_LOCK;
    case LED_COMPOSE:
      return LG_KEYBOARD_LED_COMPOSE;
    case LED_KANA:
      return LG_KEYBOARD_LED_KANA;
  }

  return 0;
}

static void evdev_handleLED(EvdevDevice * device,
    const struct input_event * event)
{
  const uint8_t led = evdev_ledForCode(event->code);
  if (!led)
    return;

  if (event->value)
    device->observedLEDs |= led;
  else
    device->observedLEDs &= ~led;

  if (device->ledOverride)
  {
    const bool observed = device->observedLEDs & led;
    const bool applied  = device->appliedLEDs  & led;
    if (observed != applied)
    {
      if (observed)
        device->hostLEDs |= led;
      else
        device->hostLEDs &= ~led;
      device->hostLEDsValid = true;
      device->ledDirty      = true;
    }
    return;
  }

  device->hostLEDs      = device->observedLEDs;
  device->hostLEDsValid = true;
}

static void evdev_handleEvents(EvdevDevice * device,
    const struct input_event * events, int count)
{
  for(int i = 0; i < count; ++i)
  {
    const struct input_event * event = &events[i];
    if (device->dropping)
    {
      if (event->type == EV_SYN && event->code == SYN_REPORT)
      {
        device->dropping = false;
        if (!evdev_syncKeys(device,
              evdev_deviceForwardingLanes(device)))
        {
          evdev_closeDevice(device, errno == ENODEV);
          return;
        }
        if (device->ledMask && !device->ledOverride)
        {
          device->hostLEDsValid =
            evdev_readLEDs(device, &device->hostLEDs);
          if (device->hostLEDsValid)
            device->observedLEDs = device->hostLEDs;
        }
      }
      continue;
    }

    switch(event->type)
    {
      case EV_SYN:
        if (event->code == SYN_DROPPED)
        {
          device->mouseX       = 0;
          device->mouseY       = 0;
          device->wheelHiRes   = 0;
          device->hwheelHiRes  = 0;
          device->dropping     = true;
        }
        else if (event->code == SYN_REPORT)
        {
          evdev_flushMotion(device);
          if (device->ledDirty)
            evdev_updateLEDs(device);
        }
        break;

      case EV_KEY:
        evdev_handleKey(device, event);
        break;

      case EV_REL:
        evdev_handleRelative(device, event);
        break;

      case EV_LED:
        evdev_handleLED(device, event);
        break;
    }
  }
}

static int evdev_waitTimeout(uint64_t now)
{
  uint64_t next = now + UINT64_C(1000000);
  for(int i = 0; i < state.deviceCount; ++i)
  {
    const EvdevDevice * device = &state.devices[i];
    if (!state.waitingBarrier.serial &&
        device->fd < 0 && device->retryAt < next)
      next = device->retryAt;
    if (device->grabRetryAt > now && device->grabRetryAt < next)
      next = device->grabRetryAt;
  }

  if (next <= now)
    return 0;

  const uint64_t delay = next - now;
  return delay >= UINT64_C(1000000) ? 1000 : (delay + 999) / 1000;
}

static void evdev_invalidateDesired(void)
{
  uint_fast64_t current = atomic_load_explicit(
      &state.desiredState, memory_order_acquire);
  for(;;)
  {
    const uint_fast64_t next =
      evdev_updateDesired(current, 0, EVDEV_GRAB_MASK);
    if (atomic_compare_exchange_weak_explicit(&state.desiredState,
          &current, next, memory_order_acq_rel, memory_order_acquire))
      return;
  }
}

static void evdev_shutdownDevices(void)
{
  evdev_invalidateDesired();
  atomic_fetch_add_explicit(
      &state.keyboardTopology, 1, memory_order_acq_rel);
  atomic_fetch_add_explicit(
      &state.pointerTopology, 1, memory_order_acq_rel);

  for(int i = 0; i < state.deviceCount; ++i)
  {
    EvdevDevice * device = &state.devices[i];
    if (device->fd < 0)
      continue;

    evdev_releaseState(device, false);
    evdev_restoreLEDs(device);
    if (device->grabbed &&
        ioctl(device->fd, EVIOCGRAB, (void *)0) < 0 && errno != ENODEV)
      DEBUG_WARN("Failed to release %s during shutdown: %s",
          device->path, strerror(errno));
    else if (device->grabbed)
      DEBUG_INFO("Ungrabbed %s", device->path);

    epoll_ctl(state.epoll, EPOLL_CTL_DEL, device->fd, NULL);
    close(device->fd);
    device->fd      = -1;
    device->grabbed = false;
  }

  atomic_store_explicit(&state.keyboardActive, false,
      memory_order_release);
  atomic_store_explicit(&state.pointerActive, false,
      memory_order_release);

  evdev_cancelOutput();
}

static int evdev_thread(void * opaque)
{
  (void)opaque;
  struct epoll_event * events = calloc(state.deviceCount + 1,
      sizeof(*events));
  if (!events)
  {
    DEBUG_ERROR("Failed to allocate evdev events");
    return -1;
  }

  struct input_event messages[256];
  DEBUG_INFO("evdev thread started");

  while(!atomic_load_explicit(&state.stop, memory_order_acquire))
  {
    uint64_t now = microtime();
    if (!state.waitingBarrier.serial)
      for(int i = 0; i < state.deviceCount; ++i)
        evdev_openDevice(&state.devices[i], now);

    evdev_applyGrab(now);
    const int waiting = epoll_wait(state.epoll, events,
        state.deviceCount + 1, evdev_waitTimeout(now));
    if (waiting < 0)
    {
      if (errno != EINTR)
        DEBUG_WARN("evdev epoll wait failed: %s", strerror(errno));
      continue;
    }

    for(int i = 0; i < waiting; ++i)
    {
      if (atomic_load_explicit(&state.stop, memory_order_acquire))
        break;

      if (!events[i].data.ptr)
      {
        evdev_drainEvent(state.commandEvent);
        continue;
      }

      EvdevDevice * device = events[i].data.ptr;
      if (device->fd < 0)
        continue;

      if (events[i].events & (EPOLLERR | EPOLLHUP))
      {
        evdev_closeDevice(device, true);
        continue;
      }

      unsigned int batches = 0;
      while(device->fd >= 0 && batches < EVDEV_READ_BATCHES &&
          !atomic_load_explicit(&state.stop, memory_order_acquire))
      {
        const ssize_t size = read(device->fd, messages, sizeof(messages));
        if (size > 0)
        {
          ++batches;
          if (size % sizeof(*messages))
            DEBUG_WARN("Incomplete evdev read: %s", device->path);
          evdev_handleEvents(device, messages, size / sizeof(*messages));
          continue;
        }

        if (size < 0 && errno == EINTR)
          continue;
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
          break;

        if (size < 0 && errno != ENODEV)
          DEBUG_WARN("Failed to read evdev event from %s: %s",
              device->path, strerror(errno));
        evdev_closeDevice(device, true);
      }
    }
  }

  evdev_shutdownDevices();
  free(events);
  DEBUG_INFO("evdev thread stopped");
  return 0;
}

static bool evdev_popOutput(EvdevOutput * output)
{
  LG_LOCK(state.outputLock);
  if (!state.outputCount)
  {
    LG_UNLOCK(state.outputLock);
    return false;
  }

  *output = state.output[state.outputHead];
  state.outputHead = (state.outputHead + 1) % EVDEV_QUEUE_SIZE;
  --state.outputCount;
  LG_UNLOCK(state.outputLock);
  return true;
}

static void evdev_dispatchModifiers(void)
{
  app_handleKeyboardModifiersInternal(
      state.dispatchKeys[KEY_LEFTCTRL]  ||
        state.dispatchKeys[KEY_RIGHTCTRL],
      state.dispatchKeys[KEY_LEFTSHIFT] ||
        state.dispatchKeys[KEY_RIGHTSHIFT],
      state.dispatchKeys[KEY_LEFTALT]   ||
        state.dispatchKeys[KEY_RIGHTALT],
      state.dispatchKeys[KEY_LEFTMETA]  ||
        state.dispatchKeys[KEY_RIGHTMETA]);
}

static void evdev_dispatchReset(unsigned int lanes)
{
  const bool keyboard = lanes & EVDEV_GRAB_KEYBOARD;
  const bool pointer  = lanes & EVDEV_GRAB_POINTER;

  if (keyboard)
    memset(state.dispatchKeys, 0, sizeof(state.dispatchKeys));
  if (pointer)
    state.dispatchButtons = 0;

  app_handleInputResetInternal(keyboard, pointer);
}

static bool evdev_outputCurrent(
    const EvdevOutput * output, unsigned int lane)
{
  if (atomic_load_explicit(&state.stop, memory_order_acquire))
    return false;

  const uint_fast64_t desiredState = atomic_load_explicit(
      &state.desiredState, memory_order_acquire);
  return evdev_lanePublished(lane) && (desiredState & lane) &&
    output->generation == evdev_generation(desiredState, lane);
}

int evdev_getEventFD(void)
{
  return state.outputEvent;
}

void evdev_dispatch(void * opaque)
{
  (void)opaque;
  if (state.outputEvent < 0)
    return;

  evdev_drainEvent(state.outputEvent);

  if (!atomic_load_explicit(&state.stop, memory_order_acquire) &&
      atomic_exchange_explicit(&state.availabilityDirty, false,
        memory_order_acq_rel))
    core_updateKeyboardGrab();

  EvdevOutput output;
  while(evdev_popOutput(&output))
  {
    switch(output.type)
    {
      case EVDEV_OUTPUT_RESET:
        evdev_dispatchReset(output.barrier.lanes);
        break;

      case EVDEV_OUTPUT_KEY:
        if (output.key.code >= KEY_MAX ||
            state.dispatchKeys[output.key.code] == output.key.down ||
            (output.key.down && !evdev_outputCurrent(
              &output, EVDEV_GRAB_KEYBOARD)))
          break;
        state.dispatchKeys[output.key.code] = output.key.down;
        if (output.key.down)
          app_handleKeyPressInternal(output.key.code);
        else
          app_handleKeyReleaseInternal(output.key.code);
        evdev_dispatchModifiers();
        break;

      case EVDEV_OUTPUT_BUTTON:
      {
        if (output.button.button >= 32)
          break;
        const uint32_t mask = UINT32_C(1) << output.button.button;
        const bool down = state.dispatchButtons & mask;
        if (down == output.button.down ||
            (output.button.down && !evdev_outputCurrent(
              &output, EVDEV_GRAB_POINTER)))
          break;
        if (output.button.down)
        {
          state.dispatchButtons |= mask;
          app_handleButtonPressInternal(output.button.button);
        }
        else
        {
          state.dispatchButtons &= ~mask;
          app_handleButtonReleaseInternal(output.button.button);
        }
        break;
      }

      case EVDEV_OUTPUT_MOTION:
        if (evdev_outputCurrent(&output, EVDEV_GRAB_POINTER))
          core_handleMouseGrabbed(output.motion.x, output.motion.y);
        break;

      case EVDEV_OUTPUT_WHEEL:
      {
        if (!evdev_outputCurrent(&output, EVDEV_GRAB_POINTER))
          break;

        const int button = output.wheel.vertical > 0 ? 4 : 5;
        const int steps  = output.wheel.vertical < 0 ?
          -output.wheel.vertical : output.wheel.vertical;
        for(int i = 0; i < steps; ++i)
        {
          app_handleButtonPressInternal(button);
          app_handleButtonReleaseInternal(button);
        }

        if (output.wheel.horizontal && !state.horizontalWheelWarned)
        {
          DEBUG_WARN("Horizontal evdev wheel input is unsupported by the "
              "active input protocol");
          state.horizontalWheelWarned = true;
        }
        break;
      }

      case EVDEV_OUTPUT_BARRIER:
      {
        const uint64_t desiredState = atomic_load_explicit(
            &state.desiredState, memory_order_acquire);
        if (!atomic_load_explicit(&state.stop, memory_order_acquire) &&
            evdev_barrierMatches(&output.barrier, desiredState))
        {
          evdev_dispatchReset(output.barrier.lanes);
          if (output.barrier.activate & EVDEV_GRAB_KEYBOARD)
            lgInput_setKeyboardLEDsSync(false);
          evdev_publishActive(output.barrier.deactivate, false);
          evdev_publishActive(output.barrier.activate, true);
          const uint64_t currentState = atomic_load_explicit(
              &state.desiredState, memory_order_acquire);
          if (output.barrier.activate &&
              !evdev_barrierMatches(&output.barrier, currentState))
            evdev_publishActive(output.barrier.activate, false);
          core_updateKeyboardLEDsSync();
        }
        atomic_store_explicit(&state.barrierAck,
            output.barrier.serial, memory_order_release);
        evdev_signal(state.commandEvent, "command");
        break;
      }
    }
  }
}

bool evdev_start(void)
{
  const char * deviceList = option_get_string("input", "evdev");
  if (!deviceList)
    return false;

  LG_LOCK_INIT(state.outputLock);
  state.outputLockInitialized = true;

  state.deviceList = strdup(deviceList);
  if (!state.deviceList)
    return false;

  StringList list = stringlist_new(false);
  if (!list)
    return false;

  char * token = strtok(state.deviceList, ",");
  while(token)
  {
    stringlist_push(list, token);
    token = strtok(NULL, ",");
  }

  state.deviceCount = stringlist_count(list);
  if (!state.deviceCount)
  {
    stringlist_free(&list);
    return false;
  }

  state.devices = calloc(state.deviceCount, sizeof(*state.devices));
  if (!state.devices)
  {
    stringlist_free(&list);
    return false;
  }

  for(int i = 0; i < state.deviceCount; ++i)
  {
    state.devices[i].path       = stringlist_at(list, i);
    state.devices[i].fd         = -1;
    state.devices[i].retryDelay = EVDEV_RETRY_INITIAL_US;
  }
  stringlist_free(&list);

  state.exclusive = option_get_bool("input", "evdevExclusive");
  atomic_init(&state.desiredState, 0);
  atomic_init(&state.barrierAck, 0);
  atomic_init(&state.keyboardTopology, 0);
  atomic_init(&state.pointerTopology, 0);
  atomic_init(&state.keyboardActive, false);
  atomic_init(&state.pointerActive, false);
  atomic_init(&state.stop, false);
  atomic_init(&state.availabilityDirty, false);
  atomic_init(&state.guestLEDState, 0);

  state.epoll = epoll_create1(EPOLL_CLOEXEC);
  if (state.epoll < 0)
  {
    DEBUG_ERROR("Failed to create evdev epoll: %s", strerror(errno));
    return false;
  }

  state.commandEvent = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (state.commandEvent < 0)
  {
    DEBUG_ERROR("Failed to create evdev command event: %s", strerror(errno));
    return false;
  }

  state.outputEvent = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (state.outputEvent < 0)
  {
    DEBUG_ERROR("Failed to create evdev output event: %s", strerror(errno));
    return false;
  }

  struct epoll_event commandEvent =
  {
    .events   = EPOLLIN,
    .data.ptr = NULL
  };
  if (epoll_ctl(state.epoll, EPOLL_CTL_ADD,
        state.commandEvent, &commandEvent) != 0)
  {
    DEBUG_ERROR("Failed to add the evdev command event to epoll: %s",
        strerror(errno));
    return false;
  }

  if (!lgCreateThread("Evdev", evdev_thread, NULL, &state.thread))
  {
    DEBUG_ERROR("Failed to create the evdev thread");
    return false;
  }

  lgInput_setKeyboardLEDsListener(evdev_keyboardLEDs, NULL);
  state.ledsListenerRegistered = true;
  lgInput_setAvailabilityListener(evdev_availabilityChanged, NULL);
  state.availabilityListenerRegistered = true;
  return true;
}

void evdev_stop(void)
{
  if (state.availabilityListenerRegistered)
  {
    lgInput_setAvailabilityListener(NULL, NULL);
    state.availabilityListenerRegistered = false;
    atomic_store_explicit(&state.availabilityDirty, false,
        memory_order_release);
  }

  if (state.ledsListenerRegistered)
  {
    lgInput_setKeyboardLEDsListener(NULL, NULL);
    state.ledsListenerRegistered = false;
  }

  if (!state.thread)
    return;

  atomic_store_explicit(&state.stop, true, memory_order_release);
  evdev_signal(state.commandEvent, "command");
  lgJoinThread(state.thread, NULL);
  state.thread = NULL;
}

void evdev_free(void)
{
  evdev_stop();

  if (state.outputEvent >= 0)
  {
    close(state.outputEvent);
    state.outputEvent = -1;
  }

  if (state.commandEvent >= 0)
  {
    close(state.commandEvent);
    state.commandEvent = -1;
  }

  if (state.epoll >= 0)
  {
    close(state.epoll);
    state.epoll = -1;
  }

  if (state.outputLockInitialized)
  {
    LG_LOCK(state.outputLock);
    state.outputHead  = 0;
    state.outputCount = 0;
    LG_UNLOCK(state.outputLock);
    LG_LOCK_FREE(state.outputLock);
    state.outputLockInitialized = false;
  }

  free(state.devices);
  state.devices     = NULL;
  state.deviceCount = 0;

  free(state.deviceList);
  state.deviceList = NULL;
}

void evdev_setGrab(bool keyboard, bool pointer)
{
  if (atomic_load_explicit(&state.stop, memory_order_acquire))
    return;

  const unsigned int reasons =
    (keyboard ? EVDEV_GRAB_KEYBOARD : 0) |
    (pointer  ? EVDEV_GRAB_POINTER  : 0);

  uint_fast64_t current = atomic_load_explicit(
      &state.desiredState, memory_order_acquire);
  while((current & EVDEV_GRAB_MASK) != reasons)
  {
    const unsigned int changed =
      (current ^ reasons) & EVDEV_GRAB_MASK;
    const uint_fast64_t next =
      evdev_updateDesired(current, reasons, changed);
    if (atomic_compare_exchange_weak_explicit(&state.desiredState,
          &current, next, memory_order_acq_rel, memory_order_acquire))
    {
      evdev_signal(state.commandEvent, "command");
      return;
    }
  }
}

bool evdev_isExclusive(void)
{
  return state.exclusive && evdev_lanePublished(EVDEV_GRAB_KEYBOARD);
}

bool evdev_isPointerExclusive(void)
{
  return state.exclusive && evdev_lanePublished(EVDEV_GRAB_POINTER);
}
