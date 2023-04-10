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