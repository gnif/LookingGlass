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

#define _GNU_SOURCE
#include "wayland.h"

#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include <wayland-client.h>

#include "common/debug.h"
#include "common/time.h"

struct WaylandPresentationFrame
{
  atomic_uint                      refs;
  struct wl_list                   link;
  struct wp_presentation_feedback * feedback;
  uint64_t                         frameToken;
  struct timespec                  sent;
  struct timespec                  swapReturn;
  struct timespec                  presented;
  bool                             swapDone;
  bool                             swapValid;
  bool                             feedbackDone;
  bool                             feedbackValid;
  bool                             timingComplete;
};

struct PresentationCompletion
{
  uint64_t frameToken;
  uint64_t presentTime;
  bool     notify;
  bool     valid;
};

static void presentationFrameRelease(struct WaylandPresentationFrame * frame)
{
  if (atomic_fetch_sub_explicit(
        &frame->refs, 1, memory_order_acq_rel) == 1)
    free(frame);
}

static bool presentationOrdered(const struct timespec * left,
    const struct timespec * right)
{
  return left->tv_sec > right->tv_sec ||
    (left->tv_sec == right->tv_sec && left->tv_nsec >= right->tv_nsec);
}

static uint64_t presentationDelta(const struct timespec * left,
    const struct timespec * right)
{
  struct timespec delta;
  tsDiff(&delta, left, right);
  return (uint64_t)delta.tv_sec * 1000000000ULL + delta.tv_nsec;
}

static void presentationCompleteLocked(
    struct WaylandPresentationFrame * frame,
    struct PresentationCompletion * completion)
{
  if (frame->timingComplete || !frame->swapDone || !frame->feedbackDone)
    return;

  frame->timingComplete   = true;
  completion->notify      = frame->frameToken != 0;
  completion->frameToken = frame->frameToken;
  completion->valid      = frame->swapValid && frame->feedbackValid &&
    presentationOrdered(&frame->presented, &frame->swapReturn);
  if (completion->valid)
    completion->presentTime = presentationDelta(
        &frame->presented, &frame->swapReturn);
}

static void presentationNotify(
    const struct PresentationCompletion * completion)
{
  if (completion->notify)
    app_handleFramePresented(completion->frameToken,
        completion->presentTime, completion->valid);
}

static void presentationClockId(void * data,
    struct wp_presentation * presentation, uint32_t clkId)
{
  wlWm.clkId = clkId;
  atomic_store_explicit(
      &wlWm.presentationClockValid, true, memory_order_release);
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
  struct WaylandPresentationFrame * frame = opaque;
  const struct timespec present = {
    .tv_sec = (uint64_t) tvSecHi << 32 | tvSecLo,
    .tv_nsec = tvNsec,
  };
  struct PresentationCompletion completion = {};

  if (tvNsec < 1000000000)
  {
    struct timespec delta;
    tsDiff(&delta, &present, &frame->sent);
    ringbuffer_push(wlWm.photonTimings,
        &(float){ delta.tv_sec * 1e3f + delta.tv_nsec * 1e-6f });
  }

  INTERLOCKED_SECTION(wlWm.presentationLock,
  {
    wl_list_remove(&frame->link);
    frame->feedback      = NULL;
    frame->feedbackDone  = true;
    frame->feedbackValid = tvNsec < 1000000000;
    frame->presented     = present;
    presentationCompleteLocked(frame, &completion);
  });

  presentationNotify(&completion);
  wp_presentation_feedback_destroy(feedback);
  presentationFrameRelease(frame);
}

bool waylandGetFramePeriod(uint64_t * period)
{
  /* Frame demand must use the output's nominal rate. Basing demand on the
   * observed presentation interval can lock a VRR session at its current,
   * slower cadence. */
  return waylandOutputGetFramePeriod(period);
}

static void presentationFeedbackDiscarded(void * data,
    struct wp_presentation_feedback * feedback)
{
  struct WaylandPresentationFrame * frame = data;
  struct PresentationCompletion completion = {};

  INTERLOCKED_SECTION(wlWm.presentationLock,
  {
    wl_list_remove(&frame->link);
    frame->feedback      = NULL;
    frame->feedbackDone  = true;
    frame->feedbackValid = false;
    presentationCompleteLocked(frame, &completion);
  });

  presentationNotify(&completion);
  wp_presentation_feedback_destroy(feedback);
  presentationFrameRelease(frame);
}

static const struct wp_presentation_feedback_listener presentationFeedbackListener = {
  .sync_output = presentationFeedbackSyncOutput,
  .presented = presentationFeedbackPresented,
  .discarded = presentationFeedbackDiscarded,
};

bool waylandPresentationInit(void)
{
  LG_LOCK_INIT(wlWm.presentationLock);
  wl_list_init(&wlWm.presentationFrames);
  atomic_store_explicit(
      &wlWm.presentationClockValid, false, memory_order_release);

  if (wlWm.presentation)
  {
    wlWm.photonTimings = ringbuffer_new(256, sizeof(float));
    wlWm.photonGraph   = app_registerGraph("PHOTON", wlWm.photonTimings,
        0.0f, 30.0f, NULL);
    app_setGraphCompact(wlWm.photonGraph, true);
    wp_presentation_add_listener(wlWm.presentation, &presentationListener, NULL);
  }
  return true;
}

void waylandPresentationFree(void)
{
  atomic_store_explicit(
      &wlWm.presentationClockValid, false, memory_order_release);

  for (;;)
  {
    struct WaylandPresentationFrame * frame = NULL;
    struct wp_presentation_feedback * feedback = NULL;
    struct PresentationCompletion      completion = {};

    LG_LOCK(wlWm.presentationLock);
    if (!wl_list_empty(&wlWm.presentationFrames))
    {
      frame = wl_container_of(
          wlWm.presentationFrames.next, frame, link);
      wl_list_remove(&frame->link);
      feedback             = frame->feedback;
      frame->feedback      = NULL;
      frame->swapDone      = true;
      frame->swapValid     = false;
      frame->feedbackDone  = true;
      frame->feedbackValid = false;
      presentationCompleteLocked(frame, &completion);
    }
    LG_UNLOCK(wlWm.presentationLock);

    if (!frame)
      break;

    presentationNotify(&completion);
    wp_presentation_feedback_destroy(feedback);
    presentationFrameRelease(frame);
  }

  if (wlWm.presentation)
  {
    wp_presentation_destroy(wlWm.presentation);
    wlWm.presentation = NULL;
    app_unregisterGraph(wlWm.photonGraph);
    ringbuffer_free(&wlWm.photonTimings);
  }
  LG_LOCK_FREE(wlWm.presentationLock);
}

struct WaylandPresentationFrame * waylandPresentationFrame(
    uint64_t frameToken)
{
  if (!wlWm.presentation || !atomic_load_explicit(
        &wlWm.presentationClockValid, memory_order_acquire))
    return NULL;

  struct WaylandPresentationFrame * frame = calloc(1, sizeof(*frame));
  if (!frame)
  {
    DEBUG_ERROR("out of memory");
    return NULL;
  }

  if (clock_gettime(wlWm.clkId, &frame->sent))
  {
    DEBUG_ERROR("clock_gettime failed: %s", strerror(errno));
    free(frame);
    return NULL;
  }

  frame->feedback =
    wp_presentation_feedback(wlWm.presentation, wlWm.surface);
  if (!frame->feedback)
  {
    free(frame);
    return NULL;
  }

  frame->frameToken     = frameToken;
  frame->timingComplete = frameToken == 0;
  atomic_init(&frame->refs, 2);
  if (wp_presentation_feedback_add_listener(frame->feedback,
        &presentationFeedbackListener, frame) < 0)
  {
    wp_presentation_feedback_destroy(frame->feedback);
    free(frame);
    return NULL;
  }

  INTERLOCKED_SECTION(wlWm.presentationLock,
  {
    wl_list_insert(&wlWm.presentationFrames, &frame->link);
  });
  return frame;
}

void waylandPresentationSwapDone(struct WaylandPresentationFrame * frame,
    bool result, bool * presentTracked)
{
  *presentTracked = false;
  if (!frame)
    return;

  struct timespec swapReturn = {};
  const bool      swapValid  = result &&
    clock_gettime(wlWm.clkId, &swapReturn) == 0;
  if (result && !swapValid)
    DEBUG_ERROR("clock_gettime failed: %s", strerror(errno));

  struct PresentationCompletion completion = {};
  INTERLOCKED_SECTION(wlWm.presentationLock,
  {
    frame->swapDone   = true;
    frame->swapValid  = swapValid;
    frame->swapReturn = swapReturn;

    if (!swapValid && !frame->timingComplete)
    {
      frame->timingComplete  = true;
      completion.notify      = true;
      completion.frameToken  = frame->frameToken;
      completion.presentTime = 0;
      completion.valid       = false;
    }
    else
      presentationCompleteLocked(frame, &completion);
  });

  *presentTracked = result && frame->frameToken != 0;
  presentationNotify(&completion);
  presentationFrameRelease(frame);
}
