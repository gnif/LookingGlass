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

#include "CClipboardChannel.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

class CClipboardSpool;

class CClipboardManager final : private IClipboardChannelHandler
{
private:
  enum class WorkType
  {
    STATE,
    RESET,
    RECORD,
    SEND,
    SEND_DATA,
  };

  enum class UIType
  {
    STATE,
    OFFER,
    CLEAR,
    REQUEST,
  };

  struct Work
  {
    WorkType type = WorkType::STATE;
    bool available = false;
    uint64_t epoch = 0;
    uint32_t reason = 0;
    KVMFRClipboardMessage record = {};
    std::vector<uint8_t> data;
    std::shared_ptr<CClipboardSpool> spool;
    uint64_t deadline = 0;
  };

  struct UIWork
  {
    UIType type = UIType::STATE;
    bool available = false;
    uint64_t epoch = 0;
    KVMFRClipboardMessage record = {};
  };

  struct PendingCancel
  {
    bool valid = false;
    KVMFRClipboardMessage record = {};
    uint64_t deadline = 0;
  };

  struct PendingControl
  {
    WorkType type = WorkType::STATE;
    bool available = false;
    uint64_t epoch = 0;
    uint32_t reason = 0;
  };

  static_assert(std::is_nothrow_move_constructible_v<Work>);
  static_assert(std::is_nothrow_move_assignable_v<Work>);

  struct IncomingTransfer
  {
    uint64_t transfer = 0;
    uint64_t generation = 0;
    KVMFRClipboardFormat format = KVMFR_CLIPBOARD_FORMAT_NONE;
    uint64_t nextOffset = 0;
    uint32_t nextSequence = 0;
    uint64_t sizeHint = KVMFR_CLIPBOARD_SIZE_UNKNOWN;
    bool began = false;
    bool complete = false;
    uint32_t error = ERROR_SUCCESS;
    HANDLE event = nullptr;
    std::shared_ptr<CClipboardSpool> spool;

    ~IncomingTransfer();
  };

  static constexpr UINT WM_CLIPBOARD_WORK = WM_APP + 0x4c;
  static constexpr size_t MAX_WORK = 16;
  static constexpr size_t MAX_UI_WORK = 16;
  static constexpr size_t MAX_PENDING_CANCEL = 8;
  static constexpr size_t MAX_PENDING_CONTROL = 16;
  static constexpr DWORD RENDER_TIMEOUT_MS = 15000;
  static constexpr DWORD SEND_TIMEOUT_MS = RENDER_TIMEOUT_MS;

  HWND m_hwnd;
  CClipboardChannel& m_channel;
  HANDLE m_stop = nullptr;
  HANDLE m_wake = nullptr;
  HANDLE m_thread = nullptr;

  std::mutex m_workLock;
  std::array<std::optional<Work>, MAX_WORK> m_recordWork;
  size_t m_recordWorkCount = 0;
  std::array<std::optional<Work>, MAX_WORK> m_sendWork;
  size_t m_sendWorkCount = 0;
  std::array<PendingCancel, MAX_PENDING_CANCEL> m_pendingCancel;
  size_t m_pendingCancelCursor = 0;
  std::array<PendingControl, MAX_PENDING_CONTROL> m_pendingControl;
  size_t m_pendingControlCount = 0;

  std::recursive_mutex m_uiLock;
  std::array<UIWork, MAX_UI_WORK> m_uiWork;
  size_t m_uiWorkCount = 0;

  std::mutex m_transferLock;
  std::shared_ptr<IncomingTransfer> m_incoming;
  std::mutex m_outgoingLock;

  UINT m_formatPNG = 0;
  UINT m_formatJPEG = 0;
  UINT m_formatOrigin = 0;
  bool m_listener = false;
  std::atomic<bool> m_shutdown { false };
  bool m_applyingRemote = false;
  bool m_available = false;
  uint64_t m_epoch = 0;
  uint64_t m_localGeneration = 0;
  DWORD m_localSequence = 0;
  uint64_t m_remoteGeneration = 0;
  uint32_t m_remoteFormats = 0;
  DWORD m_ownedSequence = 0;
  std::atomic<uint64_t> m_liveLocalGeneration { 0 };
  std::atomic<uint64_t> m_nextTransfer {
    KVMFR_CLIPBOARD_TRANSFER_HELPER | UINT64_C(1) };
  std::atomic<uint64_t> m_outgoingTransfer { 0 };
  KVMFRClipboardMessage m_pendingRemoteOffer = {};
  uint64_t m_remoteRetryDeadline = 0;

  static DWORD WINAPI ThreadProc(void * context);
  void Thread();

  bool QueueWork(Work&& work);
  bool QueueCancel(const KVMFRClipboardMessage& record, uint32_t reason,
    uint64_t deadline = 0);
  void QueueControl(WorkType type, bool available,
    uint64_t epoch, uint32_t reason);
  bool QueueUI(UIWork&& work);
  void DrainUI();
  void ProcessWork(Work&& work);
  void ProcessRecord(const KVMFRClipboardMessage& record,
    const uint8_t * data);
  void ProcessData(const KVMFRClipboardMessage& record,
    const uint8_t * data);
  void ProcessSend(Work&& work);
  void ProcessSendData(Work&& work);
  void CancelIncoming(uint32_t reason, uint64_t transfer = 0);
  void ReleaseOutgoing(uint64_t transfer);

  void HandleState(bool available, uint64_t epoch);
  void HandleOffer(const KVMFRClipboardMessage& record);
  void HandleClear(const KVMFRClipboardMessage& record);
  void HandleRequest(const KVMFRClipboardMessage& record);
  void HandleClipboardUpdate();
  void HandleDestroyClipboard();
  void RenderFormat(UINT format, uint64_t deadline = 0);
  void RenderAllFormats();
  void RetryRemoteOffer();

  bool OpenClipboardRetry() const;
  bool IsOurClipboard();
  uint32_t EnumerateFormats() const;
  void PublishLocalClipboard();
  void PublishClear(uint64_t generation);
  bool ApplyRemoteOffer(uint32_t formats, uint64_t generation);
  void ClearOwnedClipboard();
  void InvalidateOutgoing(uint32_t reason);
  void InvalidateLocalClipboard(uint32_t reason);
  void ClearRemoteRetry();
  bool SetOriginMarker(uint64_t generation) const;

  KVMFRClipboardFormat ToWireFormat(UINT format) const;
  UINT ToWindowsFormat(KVMFRClipboardFormat format) const;
  std::shared_ptr<CClipboardSpool> CaptureFormat(
    KVMFRClipboardFormat format, DWORD sequence);
  bool MaterializeFormat(KVMFRClipboardFormat format,
    CClipboardSpool& spool);

  void ClipboardState(bool available, uint64_t epoch) override;
  ClipboardChannelResult ClipboardRecord(const KVMFRClipboardMessage& record,
    const uint8_t * data) override;
  void ClipboardReset(uint64_t epoch, uint32_t reason) override;

public:
  CClipboardManager(HWND hwnd, CClipboardChannel& channel);
  ~CClipboardManager();

  CClipboardManager(const CClipboardManager&) = delete;
  CClipboardManager& operator=(const CClipboardManager&) = delete;

  bool Initialize();
  void Shutdown();
  bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam,
    LRESULT& result);
};
