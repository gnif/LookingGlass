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

#include "transport/network/NetworkSettings.h"

#include <errno.h>
#include <stdlib.h>
#include <utility>
#include <wctype.h>

namespace
{
  enum Field : unsigned
  {
    FIELD_BIND        = 1U << 0,
    FIELD_PORT        = 1U << 1,
    FIELD_MAX_CLIENTS = 1U << 2,
    FIELD_AUTH        = 1U << 3,
    FIELD_PASSWORD    = 1U << 4,
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

  bool Fail(std::wstring * error, const wchar_t * message)
  {
    if (error)
      *error = message;
    return false;
  }

  bool ValidText(const std::wstring& value, size_t maximum)
  {
    if (value.empty() || value.size() > maximum)
      return false;
    for (wchar_t character : value)
      if (iswcntrl(character))
        return false;
    return true;
  }

  bool ParseUnsigned(const std::wstring& value,
    unsigned long maximum, unsigned long& result)
  {
    if (value.empty() || value[0] == L'-')
      return false;

    wchar_t * end = nullptr;
    errno = 0;
    const unsigned long parsed = wcstoul(value.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end || !parsed || parsed > maximum)
      return false;
    result = parsed;
    return true;
  }

  bool SetField(const std::wstring& key, const std::wstring& rawValue,
    NetworkSettings& settings, unsigned& fields,
    std::wstring * error)
  {
    const std::wstring value = Trim(rawValue);
    unsigned           field = 0;
    if (Equal(key, L"bind"))
    {
      field = FIELD_BIND;
      if (!ValidText(value, 255))
        return Fail(error, L"invalid bind address");
      settings.bindAddress = value;
    }
    else if (Equal(key, L"port"))
    {
      field = FIELD_PORT;
      unsigned long parsed = 0;
      if (!ParseUnsigned(value, UINT16_MAX, parsed))
        return Fail(error, L"invalid port");
      settings.port = static_cast<uint16_t>(parsed);
    }
    else if (Equal(key, L"maxClients"))
    {
      field = FIELD_MAX_CLIENTS;
      unsigned long parsed = 0;
      if (!ParseUnsigned(value, NETWORK_MAX_CLIENTS, parsed))
        return Fail(error, L"invalid maximum client count");
      settings.maxClients = static_cast<uint16_t>(parsed);
    }
    else if (Equal(key, L"auth"))
    {
      field = FIELD_AUTH;
      if (Equal(value, L"none"))
        settings.auth = LG_NET_AUTH_NONE;
      else if (Equal(value, L"password"))
        settings.auth = LG_NET_AUTH_PASSWORD;
      else
        return Fail(error, L"invalid authentication mode");
    }
    else if (Equal(key, L"password"))
    {
      field = FIELD_PASSWORD;
      if (!ValidText(value, 255))
        return Fail(error, L"invalid password");
      settings.password = value;
    }
    else
      return Fail(error, L"unknown setting");

    if (fields & field)
      return Fail(error, L"duplicate setting");
    fields |= field;
    return true;
  }
}

bool ParseNetworkSettings(const std::wstring& source,
  NetworkSettings& settings, std::wstring * error)
{
  NetworkSettings parsed;
  if (error)
    error->clear();

  const std::wstring input = Trim(source);
  if (input.empty())
  {
    settings = std::move(parsed);
    return true;
  }
  if (input.size() > 4096)
    return Fail(error, L"settings are too long");

  unsigned fields = 0;
  size_t   begin  = 0;
  while (begin < input.size())
  {
    const size_t       separator = input.find(L';', begin);
    const size_t       end       = separator == std::wstring::npos ?
      input.size() : separator;
    const std::wstring field     = input.substr(begin, end - begin);
    const size_t       equals    = field.find(L'=');
    if (equals == std::wstring::npos ||
        !SetField(Trim(field.substr(0, equals)),
          field.substr(equals + 1), parsed, fields, error))
      return false;

    if (separator == std::wstring::npos)
      break;
    begin = separator + 1;
    if (begin == input.size())
      return Fail(error, L"empty setting");
  }

  if (parsed.auth == LG_NET_AUTH_PASSWORD && parsed.password.empty())
    return Fail(error, L"password authentication requires a password");
  if (parsed.auth == LG_NET_AUTH_NONE && !parsed.password.empty())
    return Fail(error, L"password requires password authentication");

  settings = std::move(parsed);
  return true;
}
