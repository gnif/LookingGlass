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

#ifndef _H_LG_CLIENT_EXTERNAL_FRAME_
#define _H_LG_CLIENT_EXTERNAL_FRAME_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum LG_ExternalFrameType
{
  LG_EXTERNAL_FRAME_INVALID     = 0,
  LG_EXTERNAL_FRAME_DMABUF      = 1,
  LG_EXTERNAL_FRAME_EGL_TEXTURE = 2,
}
LG_ExternalFrameType;

#define LG_EXTERNAL_FRAME_EGL_TEXTURE_VERSION 1U

/*
 * A texture created in a context which shares objects with the renderer's EGL
 * context. The producer must perform all decode and colour conversion on the
 * GPU and expose a completed RGB(A) GL_TEXTURE_2D; this interface deliberately
 * provides no CPU pixel or YUV conversion path.
 *
 * readySync is an optional GLsync encoded as an integer-sized handle. The
 * consumer waits on it before sampling, but does not delete it. Both the
 * texture and readySync remain owned by the external frame and remain valid
 * while the consumer holds a reference to that frame.
 */
typedef struct LG_ExternalFrameEGLTexture
{
  uint32_t  version;
  uint32_t  size;
  uint32_t  texture;
  uint32_t  target;
  uintptr_t readySync;
}
LG_ExternalFrameEGLTexture;

typedef struct LG_ExternalFrame LG_ExternalFrame;

/*
 * A NULL pointer means there is no external resource. External frames own
 * their native resource. Their address is a stable cache identity: a new
 * native export must use a new object. Query results are immutable, borrowed,
 * and remain valid while any reference to the object is held. Consumers must
 * never close or otherwise release a queried resource.
 *
 * LG_EXTERNAL_FRAME_DMABUF queries return one native file descriptor in an
 * int-sized result buffer. LG_EXTERNAL_FRAME_EGL_TEXTURE queries return an
 * LG_ExternalFrameEGLTexture.
 */
typedef struct LG_ExternalFrameOps
{
  LG_ExternalFrameType type;
  void (*retain )(LG_ExternalFrame * frame);
  void (*release)(LG_ExternalFrame * frame);
  bool (*query  )(const LG_ExternalFrame * frame, void * result,
      size_t resultSize);
}
LG_ExternalFrameOps;

struct LG_ExternalFrame
{
  const LG_ExternalFrameOps * ops;
};

static inline LG_ExternalFrameType lgExternalFrameType(
    const LG_ExternalFrame * frame)
{
  return frame && frame->ops ? frame->ops->type : LG_EXTERNAL_FRAME_INVALID;
}

static inline LG_ExternalFrame * lgExternalFrameRetain(
    LG_ExternalFrame * frame)
{
  if (frame)
    frame->ops->retain(frame);
  return frame;
}

static inline void lgExternalFrameRelease(LG_ExternalFrame * frame)
{
  if (frame)
    frame->ops->release(frame);
}

static inline bool lgExternalFrameQuery(const LG_ExternalFrame * frame,
    LG_ExternalFrameType type, void * result, size_t resultSize)
{
  return frame && frame->ops && frame->ops->type == type &&
    frame->ops->query(frame, result, resultSize);
}

#endif
