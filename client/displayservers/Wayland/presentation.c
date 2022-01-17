/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#define _GNU_SOURCE
#include "wayland.h"

#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include <wayland-client.h>

#include "common/debug.h"
#include "common/time.h"

struct FrameData
{
  struct timespec sent;
};

static void presentationClockId(void * data,
    struct wp_presentation * presentation, uint32_t clkId)
{
  wlWm.clkId = clkId;
}

static const struct wp_presentation_listener presentationListener = {
  .clock_id = presentationClockId,
};

static void presentationFeedbackSyncOutput(void * data,
    struct wp_presentation_feedback * feedback, struct wl_output * output)
{
  // Do nothing.
}

static void presentationFeedbackPresented(void * opaque,
    struct wp_presentation_feedback * feedback, uint32_t tvSecHi, uint32_t tvSecLo,
    uint32_t tvNsec, uint32_t refresh, uint32_t seqHi, uint32_t seqLo, uint32_t flags)
{
  struct FrameData * data = opaque;
  struct timespec present = {
    .tv_sec = (uint64_t) tvSecHi << 32 | tvSecLo,
    .tv_nsec = tvNsec,
  };
  struct timespec delta;

  tsDiff(&delta, &present, &data->sent);
  ringbuffer_push(wlWm.photonTimings, &(float){ delta.tv_sec + delta.tv_nsec * 1e-6f });
  free(data);
  wp_presentation_feedback_destroy(feedback);
}

static void presentationFeedbackDiscarded(void * data,
    struct wp_presentation_feedback * feedback)
{
  free(data);
  wp_presentation_feedback_destroy(feedback);
}

static const struct wp_presentation_feedback_listener presentationFeedbackListener = {
  .sync_output = presentationFeedbackSyncOutput,
  .presented = presentationFeedbackPresented,
  .discarded = presentationFeedbackDiscarded,
};

bool waylandPresentationInit(void)
{
  if (wlWm.presentation)
  {
    wlWm.photonTimings = ringbuffer_new(256, sizeof(float));
    wlWm.photonGraph   = app_registerGraph("PHOTON", wlWm.photonTimings,
        0.0f, 30.0f, NULL);
    wp_presentation_add_listener(wlWm.presentation, &presentationListener, NULL);
  }
  return true;
}

void waylandPresentationFree(void)
{
  if (!wlWm.presentation)
    return;

  wp_presentation_destroy(wlWm.presentation);
  app_unregisterGraph(wlWm.photonGraph);
  ringbuffer_free(&wlWm.photonTimings);
}

void waylandPresentationFrame(void)
{
  if (!wlWm.presentation)
    return;

  struct FrameData * data = malloc(sizeof(*data));
  if (clock_gettime(wlWm.clkId, &data->sent))
  {
    DEBUG_ERROR("clock_gettime failed: %s\n", strerror(errno));
    free(data);
  }

  struct wp_presentation_feedback * feedback = wp_presentation_feedback(wlWm.presentation, wlWm.surface);
  wp_presentation_feedback_add_listener(feedback, &presentationFeedbackListener, data);
}
