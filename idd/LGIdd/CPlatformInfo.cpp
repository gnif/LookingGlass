/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#include "CPlatformInfo.h"

#include "CDebug.h"
#include <Windows.h>

size_t      CPlatformInfo::m_pageSize = 0;
std::string CPlatformInfo::m_productName = "Unknown";
uint8_t     CPlatformInfo::m_uuid[16];

std::string CPlatformInfo::m_model   = "Unknown";
int         CPlatformInfo::m_cores   = 0;
int         CPlatformInfo::m_procs   = 0;
int         CPlatformInfo::m_sockets = 0;

void CPlatformInfo::Init()
{
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  m_pageSize = (size_t)si.dwPageSize;

  // we only use this for reporting, it's OK that it might not be exactly accurate
#pragma warning(push)
#pragma warning(disable : 4996)
  OSVERSIONINFOA osvi = {};
  osvi.dwOSVersionInfoSize = sizeof(osvi);
  GetVersionExA(&osvi);
#pragma warning(pop)

  DWORD bufferSize = 0;
  LSTATUS status = RegGetValueA(HKEY_LOCAL_MACHINE,
    "Software\\Microsoft\\Windows NT\\CurrentVersion", "ProductName",
    RRF_RT_REG_SZ, nullptr, nullptr, &bufferSize);
  if (status == ERROR_SUCCESS)
  {
    m_productName.resize(bufferSize);
    status = RegGetValueA(HKEY_LOCAL_MACHINE,
      "Software\\Microsoft\\Windows NT\\CurrentVersion", "ProductName",
      RRF_RT_REG_SZ, nullptr, &m_productName[0], &bufferSize);

    if (status != ERROR_SUCCESS)
    {
      m_productName = "Unknown";
      DEBUG_ERROR("Failed to read the ProductName");
    }
    else
      m_productName.resize(bufferSize - 1); // remove the double null termination
  }
  else
  {
    m_productName = "Windows " +
      std::to_string(osvi.dwMajorVersion) + "." +
      std::to_string(osvi.dwMinorVersion);
  }

  m_productName += " (Build: " +
    std::to_string(osvi.dwBuildNumber) + ") " +
    osvi.szCSDVersion;

  InitUUID();
  InitCPUInfo();
}

#define TABLE_SIG(x) (\
   ((uint32_t)(x[0]) << 24) | \
   ((uint32_t)(x[1]) << 16) | \
   ((uint32_t)(x[2]) << 8 ) | \
   ((uint32_t)(x[3]) << 0 ))

#define SMB_SST_SystemInformation 1

#define SMBVER(major, minor) \
  ((smbData->SMBIOSMajorVersion == major && \
    smbData->SMBIOSMinorVersion >= minor) || \
    (smbData->SMBIOSMajorVersion > major))

#define REVERSE32(x) \
  *(uint32_t*)(x) = ((*(uint32_t*)(x) & 0xFFFF0000) >> 16) | \
                    ((*(uint32_t*)(x) & 0x0000FFFF) << 16)

#define REVERSE16(x) \
  *(uint16_t*)(x) = ((*(uint16_t*)(x) & 0xFF00) >> 8) | \
                    ((*(uint16_t*)(x) & 0x00FF) << 8) 


static void* smbParseData(uint8_t** data, char* strings[])
{
  #pragma pack(push, 1)
  struct SMBHeader
  {
    uint8_t type;
    uint8_t length;
  };
  #pragma pack(pop)  

  SMBHeader* h = (SMBHeader*)*data;

  *data += h->length;
  if (**data)
    for (int i = 1; i < 256 && **data; ++i)
    {
      strings[i] = (char*)*data;
      *data += strlen((char*)*data) + 1;
    }
  else
    ++* data;

  ++* data;
  return h;
}

void CPlatformInfo::InitUUID()
{
  // don't warn on zero length arrays
  #pragma warning(push)
  #pragma warning(disable: 4200)
  struct RawSMBIOSData
  {
    BYTE  Used20CallingMethod;
    BYTE  SMBIOSMajorVersion;
    BYTE  SMBIOSMinorVersion;
    BYTE  DmiRevision;
    DWORD Length;
    BYTE  SMBIOSTableData[];
  };
  #pragma warning(pop)

  #pragma pack(push, 1)
  struct SMBSystemInformation
  {
    uint8_t  type;
    uint8_t  length;
    uint16_t handle;
    uint8_t  manufacturerStr;
    uint8_t  productStr;
    uint8_t  versionStr;
    uint8_t  serialStr;
    uint8_t  uuid[16];
    uint8_t  wakeupType;
    uint8_t  skuNumberStr;
    uint8_t  familyStr;
  };
  #pragma pack(pop)

  DWORD smbDataSize;
  RawSMBIOSData * smbData;
  smbDataSize = GetSystemFirmwareTable(TABLE_SIG("RSMB"), 0, nullptr, 0);
  smbData     = (RawSMBIOSData*)new BYTE[smbDataSize];
  if (!smbData)
  {
    DEBUG_ERROR("out of memory");
    return;
  }

  if (GetSystemFirmwareTable(TABLE_SIG("RSMB"), 0, smbData, smbDataSize)
      != smbDataSize)
  {
    DEBUG_ERROR("Failed to read the RSMB table");
    delete[] smbData;
    return;
  }

  uint8_t * data = (uint8_t *)smbData->SMBIOSTableData;
  uint8_t * end  = (uint8_t *)smbData->SMBIOSTableData + smbData->Length;
  char * strings[256] = {};

  while (data != end)
  {
    if (data[0] == SMB_SST_SystemInformation)
    {
      SMBSystemInformation * info = (SMBSystemInformation *)smbParseData(&data, strings);
      memcpy(&m_uuid, &info->uuid, 16);

      REVERSE32(&m_uuid[0]);
      REVERSE16(&m_uuid[0]);
      REVERSE16(&m_uuid[2]);
      REVERSE16(&m_uuid[4]);
      REVERSE16(&m_uuid[6]);
      break;
    }

    smbParseData(&data, strings);
  }

  delete[] smbData;
}

void CPlatformInfo::InitCPUInfo()
{
  DWORD bufferSize = 0;
  LSTATUS status = RegGetValueA(HKEY_LOCAL_MACHINE,
    "HARDWARE\\DESCRIPTION\\SYSTEM\\CentralProcessor\\0",
    "ProcessorNameString", RRF_RT_REG_SZ, nullptr, nullptr, &bufferSize);
  if (status == ERROR_SUCCESS)
  {
    m_model.resize(bufferSize);
    status = RegGetValueA(HKEY_LOCAL_MACHINE,
      "HARDWARE\\DESCRIPTION\\SYSTEM\\CentralProcessor\\0",
      "ProcessorNameString", RRF_RT_REG_SZ, nullptr, &m_model[0], &bufferSize);

    if (status != ERROR_SUCCESS)
    {
      m_model = "Unknown";
      DEBUG_ERROR("Failed to read the CPU Model");
    }
    else
    {
      m_model.resize(bufferSize - 1); // remove the double null termination
      m_model.erase(m_model.find_last_not_of(" \t\n\r\f\v") + 1);
    }
  }

  DWORD cb = 0;
  GetLogicalProcessorInformationEx(RelationAll, nullptr, &cb);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
  {
    DEBUG_ERROR("Failed to call GetLogicalProcessorInformationEx");
    return;
  }

  BYTE * buffer = static_cast<BYTE *>(_malloca(cb));
  if (!buffer)
  {
    DEBUG_ERROR("Failed to allocate buffer for processor information");
    return;
  }
  if (!GetLogicalProcessorInformationEx(RelationAll,
    (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer, &cb))
  {
    DEBUG_ERROR("Failed to call GetLogicalProcessorInformationEx");
    _freea(buffer);
    return;
  }

  DWORD offset = 0;
  while (offset < cb)
  {
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX lpi =
      (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer + offset);
    switch (lpi->Relationship)
    {
      case RelationProcessorCore:
        ++m_cores;

        for(int i = 0; i < lpi->Processor.GroupCount; ++i)
          m_procs += (int)__popcnt64(lpi->Processor.GroupMask[i].Mask);
        break;

      case RelationProcessorPackage:
        ++m_sockets;

      default:
        break;
    }
    offset += lpi->Size;
  }

  _freea(buffer);
}