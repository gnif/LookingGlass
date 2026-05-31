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

#pragma once

#include <string>

class CPlatformInfo
{
private:
  static size_t      m_pageSize;
  static std::string m_productName;
  static uint8_t     m_uuid[16];

  static std::string m_model;
  static int m_cores;
  static int m_procs;
  static int m_sockets;

  static void InitUUID();
  static void InitCPUInfo();

public:
  static void Init();

  inline static size_t GetPageSize() { return m_pageSize; }
  inline static const std::string& GetProductName() { return m_productName; }
  inline static const uint8_t* GetUUID() { return m_uuid; }

  inline static const std::string & GetCPUModel() { return m_model; }
  inline static int GetCoreCount() { return m_cores; }
  inline static int GetProcCount() { return m_procs; }
  inline static int GetSocketCount() { return m_sockets; }
};