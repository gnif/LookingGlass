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

#include "CSRWLock.h"
#include "transport/IControlSink.h"
#include "transport/IControlTransport.h"
#include "transport/ITransport.h"

#include <Windows.h>

#include <stdint.h>
#include <vector>

class CControlHub final : public IControlTransport, public IControlEvents
{
private:
  static constexpr unsigned MAX_SINKS = 8;
  static constexpr DWORD RETRY_MS = 5;

  enum class WorkType
  {
    POSITION,
    SHAPE,
    TRANSFORM,
    COUNT,
  };

  struct State
  {
    std::shared_ptr<const D12ColorTransform> transform;
    IDARG_OUT_QUERY_HWCURSOR cursor = {};
    std::shared_ptr<const std::vector<BYTE>> cursorData;
    UINT                     sdrWhiteLevel    = 0;
    uint64_t                 positionRevision = 0;
    uint64_t                 shapeRevision    = 0;
    uint64_t                 transformRevision = 1;
  };

  struct Work
  {
    WorkType type = WorkType::POSITION;
    IControlSink * target = nullptr;
    IDARG_OUT_QUERY_HWCURSOR cursor = {};
    std::shared_ptr<const std::vector<BYTE>> cursorData;
    std::shared_ptr<const D12ColorTransform> transform;
    UINT     sdrWhiteLevel    = 0;
    uint64_t positionRevision = 0;
    uint64_t shapeRevision    = 0;
    uint64_t transformRevision = 0;
    uint64_t bindingSerial     = 0;
    uint64_t replaySerial      = 0;
  };

  struct Sink
  {
    CControlHub * owner = nullptr;
    unsigned      index = 0;
    BackendId     backend = 0;
    uint32_t      epoch   = 0;
    CSRWLock      lock;
    IControlSink * target = nullptr;
    HANDLE         wake   = nullptr;
    HANDLE         idle   = nullptr;
    HANDLE         thread = nullptr;
    uint64_t       deliveredPosition  = 0;
    uint64_t       deliveredShape     = 0;
    uint64_t       deliveredTransform = 0;
    uint64_t       retryAt[static_cast<unsigned>(WorkType::COUNT)] = {};
    unsigned       nextWork = 0;
    uint64_t       bindingSerial = 0;
    uint64_t       replaySerial  = 0;
    bool           active   = false;
    bool           failed   = false;
    bool           failurePending = false;
    bool           reserved = false;
    bool           calling  = false;
  };

  mutable CSRWLock m_listLock;
  mutable CSRWLock m_stateLock;
  CSRWLock         m_lifecycleLock;
  State            m_state;
  Sink             m_sinks[MAX_SINKS];
  HANDLE           m_stopEvent = nullptr;
  bool             m_valid     = false;

  static DWORD WINAPI WorkerProc(void * opaque);
  static bool TokenMatches(const Sink& sink, const ControlToken& token);

  bool BeginWork(Sink& sink, Work& work, DWORD& wait);
  bool CompleteWork(Sink& sink, const Work& work, ControlResult result);
  void Worker(Sink& sink);
  void WakeAll(WorkType type);

public:
  CControlHub();
  ~CControlHub() override;

  CControlHub(const CControlHub&) = delete;
  CControlHub& operator=(const CControlHub&) = delete;

  bool Add(BackendId backend, uint32_t epoch, IControlSink& control);
  void Remove(BackendId backend, uint32_t epoch);
  bool TakeFailure(ControlToken& token);

  void OnControlReplay(const ControlToken& token) override;

  void SendCursor(const IDARG_OUT_QUERY_HWCURSOR& info,
    const BYTE * data, UINT sdrWhiteLevel) override;
  void SetColorTransform(
    std::shared_ptr<const D12ColorTransform> transform) override;
  std::shared_ptr<const D12ColorTransform>
    GetColorTransform() const override;
};
