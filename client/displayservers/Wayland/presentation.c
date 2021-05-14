/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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
  printf("Presented in %3jd.%06lums, vsync: %d, hw_clock: %d, hw_compl: %d, zero_copy: %d\n",
      (intmax_t) delta.tv_sec * 1000 + delta.tv_nsec / 1000000, delta.tv_nsec % 1000000,
      (bool) (flags & WP_PRESENTATION_FEEDBACK_KIND_VSYNC),
      (bool) (flags & WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK),
      (bool) (flags & WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION),
      (bool) (flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY));
  free(data);
}

static void presentationFeedbackDiscarded(void * data,
    struct wp_presentation_feedback * feedback)
{
  free(data);
}

static const struct wp_presentation_feedback_listener presentationFeedbackListener = {
  .sync_output = presentationFeedbackSyncOutput,
  .presented = presentationFeedbackPresented,
  .discarded = presentationFeedbackDiscarded,
};

bool waylandPresentationInit(void)
{
  if (wlWm.presentation)
    wp_presentation_add_listener(wlWm.presentation, &presentationListener, NULL);
  return true;
}

void waylandPresentationFree(void)
{
  wp_presentation_destroy(wlWm.presentation);
}

void waylandPresentationFrame(void)
{
  struct FrameData * data = malloc(sizeof(struct FrameData));
  if (clock_gettime(wlWm.clkId, &data->sent))
  {
    DEBUG_ERROR("clock_gettime failed: %s\n", strerror(errno));
    free(data);
  }

  struct wp_presentation_feedback * feedback = wp_presentation_feedback(wlWm.presentation, wlWm.surface);
  wp_presentation_feedback_add_listener(feedback, &presentationFeedbackListener, data);
}
