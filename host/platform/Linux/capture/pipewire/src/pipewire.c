/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include "interface/capture.h"
#include "interface/platform.h"
#include "common/debug.h"
#include <string.h>
#include <stdlib.h>

struct pipewire
{
};

static struct pipewire * this = NULL;

// forwards

static bool pipewire_deinit();

// implementation

static const char * pipewire_getName(void)
{
  return "Pipewire";
}

static bool pipewire_create(CaptureGetPointerBuffer getPointerBufferFn, CapturePostPointerBuffer postPointerBufferFn)
{
  DEBUG_ASSERT(!this);
  this = calloc(1, sizeof(*this));
  return true;
}

static bool pipewire_init(void)
{
  goto fail;
fail:
  pipewire_deinit();
  return false;
}

static bool pipewire_deinit(void)
{
  return false;
}

static void pipewire_free(void)
{
  free(this);
  this = NULL;
}

static CaptureResult pipewire_capture(void)
{
  return CAPTURE_RESULT_ERROR;
}

static CaptureResult pipewire_waitFrame(CaptureFrame * frame,
    const size_t maxFrameSize)
{
  return CAPTURE_RESULT_ERROR;
}

static CaptureResult pipewire_getFrame(FrameBuffer * frame,
    const unsigned int height, int frameIndex)
{
  return CAPTURE_RESULT_ERROR;
}

struct CaptureInterface Capture_pipewire =
{
  .shortName       = "pipewire",
  .asyncCapture    = false,
  .getName         = pipewire_getName,
  .create          = pipewire_create,
  .init            = pipewire_init,
  .deinit          = pipewire_deinit,
  .free            = pipewire_free,
  .capture         = pipewire_capture,
  .waitFrame       = pipewire_waitFrame,
  .getFrame        = pipewire_getFrame
};
