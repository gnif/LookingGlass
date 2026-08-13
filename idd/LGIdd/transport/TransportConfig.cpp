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

#include "transport/TransportConfig.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <wctype.h>

namespace
{
  enum Field : unsigned
  {
    FIELD_ID       = 1U << 0,
    FIELD_KIND     = 1U << 1,
    FIELD_ENABLED  = 1U << 2,
    FIELD_REQUIRED = 1U << 3,
    FIELD_SERVICES = 1U << 4,
    FIELD_PRIORITY = 1U << 5,
    FIELD_SETTINGS = 1U << 6,
  };

  std::wstring Trim(const std::wstring& value)
  {
    size_t begin = 0;
    size_t end   = value.size();
    while (begin < end && iswspace(value[begin]))
      ++begin;
    while (end > begin && iswspace(value[end - 1]))
      --end;
    return value.substr(begin, end - begin);
  }

  bool Equal(const std::wstring& left, const wchar_t * right)
  {
    if (!right)
      return false;

    size_t index = 0;
    for (; index < left.size() && right[index]; ++index)
      if (towlower(left[index]) != towlower(right[index]))
        return false;
    return index == left.size() && !right[index];
  }

  bool ParseBool(const std::wstring& value, bool& result)
  {
    if (Equal(value, L"true") || value == L"1")
    {
      result = true;
      return true;
    }
    if (Equal(value, L"false") || value == L"0")
    {
      result = false;
      return true;
    }
    return false;
  }

  bool ParseId(const std::wstring& value, BackendId& result)
  {
    if (value.empty() || value[0] == L'-')
      return false;

    wchar_t * end = nullptr;
    errno = 0;
    const unsigned long long parsed = wcstoull(value.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end || !parsed || parsed > UINT32_MAX)
      return false;
    result = static_cast<BackendId>(parsed);
    return true;
  }

  bool ParsePriority(const std::wstring& value, int32_t& result)
  {
    if (value.empty())
      return false;

    wchar_t * end = nullptr;
    errno = 0;
    const long long parsed = wcstoll(value.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end || parsed < INT32_MIN ||
        parsed > INT32_MAX)
      return false;
    result = static_cast<int32_t>(parsed);
    return true;
  }

  bool ParseKind(const std::wstring& value, std::wstring& result)
  {
    if (value.empty() || value.size() > 32)
      return false;
    for (wchar_t character : value)
      if (!iswalnum(character) && character != L'_' && character != L'-')
        return false;
    result = value;
    return true;
  }

  bool AddService(const std::wstring& name, uint32_t& services)
  {
    uint32_t service = 0;
    if (Equal(name, L"frame"))
      service = TRANSPORT_SERVICE_FRAME;
    else if (Equal(name, L"control"))
      service = TRANSPORT_SERVICE_CONTROL;
    else if (Equal(name, L"input"))
      service = TRANSPORT_SERVICE_INPUT;
    else if (Equal(name, L"clipboard"))
      service = TRANSPORT_SERVICE_CLIPBOARD;
    else
      return false;

    if (services & service)
      return false;
    services |= service;
    return true;
  }

  bool ParseServices(const std::wstring& value, uint32_t& services)
  {
    if (Equal(value, L"all"))
    {
      services = TRANSPORT_SERVICE_ALL;
      return true;
    }
    if (Equal(value, L"none"))
    {
      services = 0;
      return true;
    }

    services = 0;
    size_t begin = 0;
    while (begin < value.size())
    {
      const size_t separator = value.find(L',', begin);
      const size_t end = separator == std::wstring::npos ?
        value.size() : separator;
      if (!AddService(Trim(value.substr(begin, end - begin)), services))
        return false;
      if (separator == std::wstring::npos)
        return true;
      begin = separator + 1;
    }
    return false;
  }

  bool SetField(const std::wstring& key, const std::wstring& value,
    TransportInstance& instance, unsigned& fields)
  {
    unsigned field = 0;
    bool valid = false;
    if (Equal(key, L"id"))
    {
      field = FIELD_ID;
      valid = ParseId(Trim(value), instance.id);
    }
    else if (Equal(key, L"kind"))
    {
      field = FIELD_KIND;
      valid = ParseKind(Trim(value), instance.kind);
    }
    else if (Equal(key, L"enabled"))
    {
      field = FIELD_ENABLED;
      valid = ParseBool(Trim(value), instance.enabled);
    }
    else if (Equal(key, L"required"))
    {
      field = FIELD_REQUIRED;
      valid = ParseBool(Trim(value), instance.required);
    }
    else if (Equal(key, L"services"))
    {
      field = FIELD_SERVICES;
      valid = ParseServices(Trim(value), instance.services);
    }
    else if (Equal(key, L"priority"))
    {
      field = FIELD_PRIORITY;
      valid = ParsePriority(Trim(value), instance.priority);
    }

    if (!field || (fields & field) || !valid)
      return false;
    fields |= field;
    return true;
  }

  bool ParseInstance(const std::wstring& source,
    TransportInstance& instance)
  {
    const std::wstring& line = source;
    if (Trim(line).empty() || line.size() > 4096)
      return false;

    unsigned fields = 0;
    size_t begin = 0;
    while (begin < line.size())
    {
      const size_t separator = line.find(L';', begin);
      const size_t end = separator == std::wstring::npos ?
        line.size() : separator;
      const std::wstring field = line.substr(begin, end - begin);
      const size_t equals = field.find(L'=');
      if (equals == std::wstring::npos)
        return false;

      const std::wstring key = Trim(field.substr(0, equals));
      if (Equal(key, L"settings"))
      {
        if (fields & FIELD_SETTINGS)
          return false;
        instance.settings = line.substr(begin + equals + 1);
        fields |= FIELD_SETTINGS;
        begin = line.size();
        break;
      }

      if (!SetField(key, field.substr(equals + 1), instance, fields))
        return false;
      if (separator == std::wstring::npos)
        break;
      begin = separator + 1;
      if (begin == line.size())
        return false;
    }

    return (fields & (FIELD_ID | FIELD_KIND)) ==
      (FIELD_ID | FIELD_KIND);
  }

  int FindKind(const std::wstring& name,
    const TransportKind * kinds, unsigned kindCount)
  {
    for (unsigned i = 0; i < kindCount; ++i)
      if (kinds[i].name && Equal(name, kinds[i].name))
        return static_cast<int>(i);
    return -1;
  }

  bool TryResolve(const TransportInstances& instances,
    const TransportKind * kinds, unsigned kindCount,
    ResolvedTransportInstances& resolved)
  {
    if (!kinds || !kindCount || instances.empty() ||
        instances.size() > TRANSPORT_MAX_INSTANCES)
      return false;

    ResolvedTransportInstances result;
    int primary = -1;
    for (const TransportInstance& instance : instances)
    {
      if (!instance.id || instance.kind.empty() ||
          (instance.services & ~TRANSPORT_SERVICE_ALL))
        return false;

      for (const TransportInstance& other : instances)
        if (&other != &instance && other.id == instance.id)
          return false;

      const int kindIndex = FindKind(instance.kind, kinds, kindCount);
      if (kindIndex < 0 || !kinds[kindIndex].activeLimit)
        return false;
      if (!instance.enabled)
        continue;

      unsigned active = 0;
      for (const ResolvedTransportInstance& existing : result)
        if (existing.kindIndex == static_cast<unsigned>(kindIndex))
          ++active;
      if (active == kinds[kindIndex].activeLimit ||
          result.size() == TRANSPORT_MAX_INSTANCES)
        return false;

      ResolvedTransportInstance selected;
      selected.config    = instance;
      selected.kindIndex = static_cast<unsigned>(kindIndex);
      if (instance.required &&
          (instance.services & TRANSPORT_SERVICE_FRAME) &&
          (primary < 0 || instance.priority >
            result[primary].config.priority))
        primary = static_cast<int>(result.size());
      result.push_back(selected);
    }

    if (primary < 0)
      return false;
    result[primary].primary = true;
    resolved.swap(result);
    return true;
  }
}

TransportInstances DefaultTransportInstances()
{
  TransportInstance instance;
  instance.id       = 1;
  instance.kind     = L"LGMP";
  instance.enabled  = true;
  instance.required = true;
  instance.services = TRANSPORT_SERVICE_ALL;
  instance.priority = 0;

  TransportInstances instances;
  instances.push_back(instance);
  return instances;
}

bool ParseTransportInstances(const std::vector<std::wstring>& entries,
  TransportInstances& instances)
{
  TransportInstances parsed;
  if (entries.empty() || entries.size() > TRANSPORT_MAX_INSTANCES)
    return false;

  bool hasRequiredFrame = false;
  for (const std::wstring& entry : entries)
  {
    TransportInstance instance;
    if (!ParseInstance(entry, instance))
      return false;

    for (const TransportInstance& existing : parsed)
      if (existing.id == instance.id)
        return false;

    if (instance.enabled && instance.required &&
        (instance.services & TRANSPORT_SERVICE_FRAME))
      hasRequiredFrame = true;
    parsed.push_back(instance);
  }

  if (!hasRequiredFrame)
    return false;
  instances.swap(parsed);
  return true;
}

bool ResolveTransportInstances(const TransportInstances& configured,
  const TransportKind * kinds, unsigned kindCount,
  ResolvedTransportInstances& resolved, bool& usedDefaults)
{
  usedDefaults = false;
  if (TryResolve(configured, kinds, kindCount, resolved))
    return true;

  usedDefaults = true;
  const TransportInstances defaults = DefaultTransportInstances();
  if (TryResolve(defaults, kinds, kindCount, resolved))
    return true;

  resolved.clear();
  return false;
}
