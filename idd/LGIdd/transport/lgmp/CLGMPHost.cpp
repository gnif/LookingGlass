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

#include "transport/lgmp/CLGMPHost.h"

#include "transport/lgmp/CIVSHMEM.h"
#include "platform/CPlatformInfo.h"
#include "CDebug.h"
#include "VersionInfo.h"

#include <LGProtocol/KVMFR.h>

#include <stdlib.h>
#include <string.h>

#include <sstream>
#include <string>

CLGMPHost::~CLGMPHost()
{
  DeInit();
}

bool CLGMPHost::Initialize(CIVSHMEM& ivshmem)
{
  if (m_host)
    return true;

  std::stringstream ss;
  {
    KVMFR kvmfr = {};
    memcpy_s(kvmfr.magic, sizeof(kvmfr.magic), KVMFR_MAGIC, sizeof(KVMFR_MAGIC) - 1);
    kvmfr.version  = KVMFR_VERSION;
    kvmfr.features =
      KVMFR_FEATURE_SETCURSORPOS   |
      KVMFR_FEATURE_WINDOWSIZE     |
      KVMFR_FEATURE_FRAME_SCHEDULE |
      KVMFR_FEATURE_INPUT          |
      KVMFR_FEATURE_CLIPBOARD;
    strncpy_s(kvmfr.hostver, LG_VERSION_STR, sizeof(kvmfr.hostver) - 1);
    ss.write(reinterpret_cast<const char *>(&kvmfr), sizeof(kvmfr));
  }

  {
    const std::string & model = CPlatformInfo::GetCPUModel();

    KVMFRRecord_VMInfo * vmInfo = static_cast<KVMFRRecord_VMInfo *>(calloc(1, sizeof(*vmInfo)));
    if (!vmInfo)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord_VMInfo");
      return false;
    }
    vmInfo->cpus    = static_cast<uint8_t>(CPlatformInfo::GetProcCount  ());
    vmInfo->cores   = static_cast<uint8_t>(CPlatformInfo::GetCoreCount  ());
    vmInfo->sockets = static_cast<uint8_t>(CPlatformInfo::GetSocketCount());

    const uint8_t * uuid = CPlatformInfo::GetUUID();
    memcpy_s (vmInfo->uuid, sizeof(vmInfo->uuid), uuid, 16);
    strncpy_s(vmInfo->capture, "Looking Glass IDD Driver", sizeof(vmInfo->capture));

    KVMFRRecord * record = static_cast<KVMFRRecord *>(calloc(1, sizeof(*record)));
    if (!record)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord");
      return false;
    }

    record->type = KVMFR_RECORD_VMINFO;
    record->size = sizeof(*vmInfo) + (uint32_t)model.length() + 1;

    ss.write(reinterpret_cast<const char*>(record       ), sizeof(*record));
    ss.write(reinterpret_cast<const char*>(vmInfo       ), sizeof(*vmInfo));
    ss.write(reinterpret_cast<const char*>(model.c_str()), model.length() + 1);
  }

  {
    KVMFRRecord_OSInfo * osInfo = static_cast<KVMFRRecord_OSInfo *>(calloc(1, sizeof(*osInfo)));
    if (!osInfo)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord_OSInfo");
      return false;
    }

    osInfo->os = KVMFR_OS_WINDOWS;

    const std::string & osName = CPlatformInfo::GetProductName();

    KVMFRRecord* record = static_cast<KVMFRRecord*>(calloc(1, sizeof(*record)));
    if (!record)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord");
      return false;
    }

    record->type = KVMFR_RECORD_OSINFO;
    record->size = sizeof(*osInfo) + (uint32_t)osName.length() + 1;

    ss.write(reinterpret_cast<const char*>(record), sizeof(*record));
    ss.write(reinterpret_cast<const char*>(osInfo), sizeof(*osInfo));
    ss.write(reinterpret_cast<const char*>(osName.c_str()), osName.length() + 1);
  }

  LGMP_STATUS status;
  std::string udata = ss.str();

  if ((status = lgmpHostInit(ivshmem.GetMem(),
    (uint32_t)ivshmem.GetUsableSize(),
    &m_host, (uint32_t)udata.size(), (uint8_t*)&udata[0])) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostInit Failed: %s", lgmpStatusString(status));
    return false;
  }

  return true;
}

void CLGMPHost::DeInit()
{
  if (m_host)
    lgmpHostFree(&m_host);
}

LGMP_STATUS CLGMPHost::Process()
{
  CSRWExclusiveLock lock(m_processLock);
  const LGMP_STATUS status = lgmpHostProcess(m_host);
  return status;
}

LGMP_STATUS CLGMPHost::CreateQueue(
  const struct LGMPQueueConfig& config, PLGMPHostQueue * queue)
{
  return lgmpHostQueueNew(m_host, config, queue);
}

LGMP_STATUS CLGMPHost::CreateStream(
  const struct LGMPStreamConfig& config, PLGMPHostStream * stream)
{
  return lgmpHostStreamNew(m_host, config, stream);
}

LGMP_STATUS CLGMPHost::Allocate(uint32_t size, PLGMPMemory * memory)
{
  return lgmpHostMemAlloc(m_host, size, memory);
}

LGMP_STATUS CLGMPHost::AllocateAligned(uint32_t size,
  uint32_t alignment, PLGMPMemory * memory)
{
  return lgmpHostMemAllocAligned(m_host, size, alignment, memory);
}

size_t CLGMPHost::Available() const
{
  return lgmpHostMemAvail(m_host);
}
