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
#include "CClipboardFiles.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CClipboardSpool;

class CClipboardManager final : private IClipboardChannelHandler
{
private:
  struct MarshaledDataObject;
  class RemoteFileProvider;

  enum class WorkType
  {
    STATE,
    RESET,
    RECORD,
    SEND,
    SEND_DATA,
    SEND_FILE_DATA,
    CANCEL_FILE_MANIFEST,
  };

  enum class UIType
  {
    STATE,
    OFFER,
    CLEAR,
    REQUEST,
    FILE_OFFER,
    FILES,
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
    std::shared_ptr<std::vector<uint8_t>> fileData;
    uint64_t deadline = 0;
  };

  struct UIWork
  {
    UIType type = UIType::STATE;
    bool available = false;
    uint64_t epoch = 0;
    KVMFRClipboardMessage record = {};
    std::shared_ptr<MarshaledDataObject> dataObject;
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

  struct IncomingFileRequest
  {
    uint64_t dataset = 0;
    uint64_t transfer = 0;
    uint64_t node = 0;
    KVMFRClipboardFileOperation operation = 0;
    uint64_t nextOffset = 0;
    uint32_t nextSequence = 0;
    uint64_t sizeHint = KVMFR_CLIPBOARD_SIZE_UNKNOWN;
    uint32_t requestedBytes = 0;
    bool began = false;
    bool complete = false;
    bool manifest = false;
    KVMFRClipboardFileError error = KVMFR_CLIPBOARD_FILE_ERROR_NONE;
    HANDLE event = nullptr;
    // Borrowed by a synchronous READ until this request is removed while
    // holding m_fileLock. Manifest LIST responses retain owned vector data.
    uint8_t * output = nullptr;
    uint32_t outputCapacity = 0;
    std::vector<uint8_t> data;

    ~IncomingFileRequest();
  };

  struct RemoteFileManifest
  {
    uint64_t dataset = 0;
    uint64_t acquisition = 0;
    uint64_t currentRequest = 0;
    uint64_t currentParent = 0;
    uint64_t deadline = 0;
    KVMFRClipboardMessage offer = {};
    std::deque<uint64_t> directories;
    std::vector<ClipboardRemoteFileEntry> entries;
    std::unordered_set<uint64_t> nodes;
  };

  struct OutgoingFileRequest
  {
    uint64_t dataset = 0;
    bool cancelled = false;
  };

  static constexpr UINT WM_CLIPBOARD_WORK = WM_APP + 0x4c;
  static constexpr size_t MAX_WORK = 16;
  static constexpr size_t MAX_UI_WORK = 16;
  static constexpr size_t MAX_PENDING_CANCEL =
    KVMFR_CLIPBOARD_FILE_MAX_REQUESTS +
    KVMFR_CLIPBOARD_FILE_MAX_ACQUISITIONS + 1U;
  static constexpr size_t MAX_PENDING_CONTROL = 16;
  static constexpr DWORD RENDER_TIMEOUT_MS = 15000;
  static constexpr DWORD SEND_TIMEOUT_MS = RENDER_TIMEOUT_MS;
  static constexpr DWORD FILE_MANIFEST_TIMEOUT_MS = 30000;

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

  std::mutex m_fileLock;
  std::unordered_map<uint64_t, std::shared_ptr<CLocalClipboardFiles>>
    m_localFileDatasets;
  std::unordered_map<uint64_t, uint64_t> m_localFileAcquisitions;
  std::unordered_map<uint64_t, std::shared_ptr<IncomingFileRequest>>
    m_incomingFileRequests;
  std::unordered_map<uint64_t, uint64_t> m_remoteFileAcquisitions;
  std::unordered_map<uint64_t, OutgoingFileRequest> m_outgoingFileRequests;
  std::unique_ptr<RemoteFileManifest> m_remoteFileManifest;
  std::shared_ptr<RemoteFileProvider> m_remoteFileProvider;
  uint64_t m_localFileDataset = 0;
  IDataObject * m_oleClipboard = nullptr;
  bool m_oleInitialized = false;

  UINT m_formatPNG = 0;
  UINT m_formatJPEG = 0;
  UINT m_formatOrigin = 0;
  bool m_listener = false;
  std::atomic<bool> m_shutdown { false };
  bool m_applyingRemote = false;
  bool m_available = false;
  uint64_t m_epoch                  = 0;
  uint64_t m_localGeneration        = 0;
  DWORD m_localSequence             = 0;
  HWND m_localOwner                 = nullptr;
  DWORD m_materializedSequence      = 0;
  uint64_t m_materializedGeneration = 0;
  DWORD m_localRetrySequence        = 0;
  uint64_t m_localRetryDeadline     = 0;
  uint64_t m_remoteGeneration       = 0;
  uint32_t m_remoteFormats          = 0;
  DWORD m_ownedSequence             = 0;
  std::atomic<uint64_t> m_liveLocalGeneration { 0 };
  std::atomic<uint64_t> m_nextTransfer {
    KVMFR_CLIPBOARD_TRANSFER_HELPER | UINT64_C(1) };
  std::atomic<uint64_t> m_outgoingTransfer { 0 };
  KVMFRClipboardMessage m_pendingRemoteOffer = {};
  uint64_t m_remoteRetryDeadline = 0;

  static DWORD WINAPI ThreadProc(void * context);
  void Thread();

  bool QueueWork(Work&& work);
  bool QueueCancellation(const KVMFRClipboardMessage& record,
    uint64_t deadline);
  bool QueueCancel(const KVMFRClipboardMessage& record, uint32_t reason,
    uint64_t deadline = 0);
  void QueueControl(WorkType type, bool available,
    uint64_t epoch, uint32_t reason);
  bool QueueUI(UIWork&& work);
  void DiscardPendingRemoteUI();
  void DrainUI();
  void ProcessWork(Work&& work);
  void ProcessRecord(const KVMFRClipboardMessage& record,
    const uint8_t * data);
  void ProcessData(const KVMFRClipboardMessage& record,
    const uint8_t * data);
  void ProcessSend(Work&& work);
  void ProcessSendData(Work&& work);
  void ProcessSendFileData(Work&& work);
  void ProcessFileRecord(const KVMFRClipboardMessage& record,
    const uint8_t * data);
  void ProcessFileData(const KVMFRClipboardMessage& record,
    const uint8_t * data);
  bool QueueFileCancel(const KVMFRClipboardMessage& record,
    KVMFRClipboardFileError error);
  void HandleFileAcquire(const KVMFRClipboardMessage& record);
  void HandleFileRelease(const KVMFRClipboardMessage& record);
  void HandleFileRequest(const KVMFRClipboardMessage& record);
  void StartRemoteFileOffer(const KVMFRClipboardMessage& record);
  void StartRemoteFileList(uint64_t parent);
  void ContinueRemoteFileManifest(
    const std::shared_ptr<IncomingFileRequest>& request);
  void FinishRemoteFileManifest();
  void FailRemoteFileManifest(KVMFRClipboardFileError error);
  DWORD RemoteFileManifestWait() const;
  void CheckRemoteFileManifestTimeout();
  void CancelFileRequests(KVMFRClipboardFileError error,
    uint64_t transfer = 0);
  HRESULT ReadRemoteFile(uint64_t dataset, uint64_t acquisition,
    uint64_t node, uint64_t offset, void * data, ULONG length, ULONG& read);
  void ReleaseRemoteFileDataset(uint64_t dataset, uint64_t acquisition);
  uint64_t NextHelperTransfer();
  void PruneLocalFileDatasets();
  void RetireLocalFileDataset();
  void CancelIncoming(uint32_t reason, uint64_t transfer = 0);
  void ReleaseOutgoing(uint64_t transfer);

  void HandleState(bool available, uint64_t epoch);
  void HandleOffer(const KVMFRClipboardMessage& record);
  void HandleClear(const KVMFRClipboardMessage& record);
  void HandleRequest(const KVMFRClipboardMessage& record);
  void HandlePendingFileOffer(const KVMFRClipboardMessage& record);
  void HandleFileDataObject(UIWork& work);
  void HandleClipboardUpdate();
  void HandleDestroyClipboard();
  void RenderFormat(UINT format, uint64_t deadline = 0);
  void RenderAllFormats();
  void RetryLocalClipboard();
  void RetryRemoteOffer();

  HRESULT OpenClipboardRetry(const char * stage) const;
  bool IsOurClipboard();
  uint32_t EnumerateFormats() const;
  std::shared_ptr<CLocalClipboardFiles> CaptureClipboardFiles(
    DWORD sequence, int rawFormatCount, bool& viaOLE,
    const char *& retryStage, KVMFRClipboardFileError& error,
    HRESULT& oleError);
  void PublishLocalClipboard();
  void PublishClear(uint64_t generation);
  bool ApplyRemoteOffer(uint32_t formats, uint64_t generation);
  void ClearOwnedClipboard();
  void InvalidateOutgoing(uint32_t reason);
  void InvalidateLocalClipboard(uint32_t reason);
  void ClearLocalRetry();
  void DeferLocalClipboard(DWORD sequence, const char * stage);
  void ExpireLocalRetry(DWORD sequence);
  void ClearRemoteRetry();
  bool SetOriginMarker(uint64_t generation) const;

  KVMFRClipboardFormat ToWireFormat(UINT format) const;
  UINT ToWindowsFormat(KVMFRClipboardFormat format) const;
  std::shared_ptr<CClipboardSpool> CaptureFormat(
    KVMFRClipboardFormat format, DWORD sequence, HWND owner,
    DWORD& capturedSequence);
  bool MaterializeFormat(KVMFRClipboardFormat format,
    UINT windowsFormat, CClipboardSpool& spool);

  void ClipboardState(bool available, uint64_t epoch) override;
  ClipboardChannelResult ClipboardRecord(
    const KVMFRClipboardMessage& record,
    std::vector<uint8_t>&& data) override;
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
