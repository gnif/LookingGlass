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

#pragma once

#include "transport/CLGMPHost.h"
#include "postprocess/D12FrameFormat.h"

#include "common/KVMFR.h"

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>

#include <memory>

class CLGMPControl
{
private:
  static constexpr int POINTER_SHAPE_BUFFERS   = 3;
  static constexpr int COLOR_TRANSFORM_BUFFERS = 3;

  CLGMPHost& m_host;

  PLGMPHostQueue m_pointerQueue = nullptr;
  PLGMPMemory    m_pointerMemory[LGMP_Q_POINTER_LEN] = {};
  PLGMPMemory    m_pointerShapeMemory[POINTER_SHAPE_BUFFERS] = {};
  PLGMPMemory    m_pointerTransformMemory[COLOR_TRANSFORM_BUFFERS] = {};
  PLGMPMemory    m_pointerShape = nullptr;
  int  m_pointerMemoryIndex    = 0;
  int  m_pointerShapeIndex     = 0;
  int  m_pointerTransformIndex = 0;
  bool m_cursorVisible         = false;
  int  m_cursorX               = 0;
  int  m_cursorY               = 0;

  mutable SRWLOCK                         m_colorTransformLock = SRWLOCK_INIT;
  std::shared_ptr<const D12ColorTransform> m_colorTransform;

  void SendColorTransform();
  void ResendCursor();

public:
  explicit CLGMPControl(CLGMPHost& host) :
    m_host(host) {}
  ~CLGMPControl();

  CLGMPControl(const CLGMPControl&) = delete;
  CLGMPControl& operator=(const CLGMPControl&) = delete;

  bool Initialize();
  void DeInit();

  LGMP_STATUS ReadDataWithSource(void * data, size_t * size,
    uint32_t * sourceClientID);
  LGMP_STATUS AckData();
  bool HasNewSubscribers();

  void SendCursor(const IDARG_OUT_QUERY_HWCURSOR& info, const BYTE * data,
    UINT sdrWhiteLevel);
  void SetColorTransform(
    std::shared_ptr<const D12ColorTransform> transform);
  std::shared_ptr<const D12ColorTransform> GetColorTransform() const;
  void ResendState();
};
