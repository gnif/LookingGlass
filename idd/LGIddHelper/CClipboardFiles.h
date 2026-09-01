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

#include <Windows.h>
#include <ObjIdl.h>
#include <OleIdl.h>
#include <ShellAPI.h>

#include <LGProtocol/KVMFRClipboard.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct ClipboardRemoteFileEntry
{
  uint64_t node = 0;
  uint64_t parent = 0;
  uint64_t size = 0;
  uint64_t createdNs = 0;
  uint64_t modifiedNs = 0;
  KVMFRClipboardFileType type = KVMFR_CLIPBOARD_FILE_TYPE_REGULAR;
  std::wstring name;
};

class CLocalClipboardFiles final
{
public:
  struct Node
  {
    uint64_t id = 0;
    uint64_t size = 0;
    uint64_t createdNs = 0;
    uint64_t modifiedNs = 0;
    KVMFRClipboardFileType type = KVMFR_CLIPBOARD_FILE_TYPE_REGULAR;
    std::wstring name;
    std::wstring path;
    bool childrenLoaded = false;
    DWORD volumeSerial = 0;
    DWORD fileIndexHigh = 0;
    DWORD fileIndexLow = 0;
    std::vector<uint64_t> children;
  };

private:
  std::mutex m_lock;
  std::vector<Node> m_nodes;
  std::vector<uint64_t> m_roots;
  HANDLE m_userToken = nullptr;
  bool m_published = false;

  explicit CLocalClipboardFiles(HANDLE userToken);
  bool AddRoot(const std::wstring& path, KVMFRClipboardFileError& error);
  bool RefreshRoots(KVMFRClipboardFileError& error);
  bool LoadChildren(size_t index, KVMFRClipboardFileError& error);
  bool CaptureIdentity(Node& node, KVMFRClipboardFileError& error) const;
  bool CheckIdentity(const Node& node, KVMFRClipboardFileError& error) const;

public:
  ~CLocalClipboardFiles();

  CLocalClipboardFiles(const CLocalClipboardFiles&) = delete;
  CLocalClipboardFiles& operator=(const CLocalClipboardFiles&) = delete;

  static std::shared_ptr<CLocalClipboardFiles> Capture(HDROP drop,
    KVMFRClipboardFileError& error);

  size_t RootCount() const;
  bool List(uint64_t node, std::vector<uint8_t>& data,
    KVMFRClipboardFileError& error);
  bool Read(uint64_t node, uint64_t offset, uint32_t length,
    std::vector<uint8_t>& data, KVMFRClipboardFileError& error);
};

class IRemoteClipboardFileProvider
{
public:
  virtual ~IRemoteClipboardFileProvider() = default;

  virtual HRESULT ReadClipboardFile(uint64_t dataset, uint64_t acquisition,
    uint64_t node, uint64_t offset, void * data, ULONG length,
    ULONG& read) = 0;
  virtual void ReleaseClipboardFileDataset(uint64_t dataset,
    uint64_t acquisition) = 0;
};

HRESULT ParseClipboardFileList(uint64_t parent, const uint8_t * data,
  size_t length, std::vector<ClipboardRemoteFileEntry>& entries);

HRESULT CreateClipboardFileDataObject(uint64_t dataset, uint64_t acquisition,
  std::vector<ClipboardRemoteFileEntry>&& entries,
  std::shared_ptr<IRemoteClipboardFileProvider> provider,
  IDataObject ** object);
