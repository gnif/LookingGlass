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

#include "RefreshRate.h"

#include <cwchar>
#include <cwctype>
#include <limits>

static std::wstring Trim(const std::wstring& text)
{
  size_t begin = 0;
  size_t end   = text.size();
  while (begin < end && std::iswspace(text[begin]))
    ++begin;
  while (end > begin && std::iswspace(text[end - 1]))
    --end;
  return text.substr(begin, end - begin);
}

static bool ToUnsigned(const std::wstring& text, unsigned& value)
{
  if (text.empty())
    return false;

  wchar_t * end = nullptr;
  const unsigned long long parsed = std::wcstoull(text.c_str(), &end, 10);
  if (!end || *end != L'\0' ||
      parsed > std::numeric_limits<unsigned>::max())
    return false;

  value = (unsigned)parsed;
  return true;
}

bool LGParseRefreshRate(const std::wstring& text, unsigned& refreshMilliHz)
{
  const std::wstring value = Trim(text);
  const size_t decimal = value.find(L'.');
  if (value.empty() ||
      (decimal != std::wstring::npos &&
        (value.find(L'.', decimal + 1) != std::wstring::npos ||
         decimal == 0 || decimal + 4 < value.size())))
    return false;

  const std::wstring wholeText = value.substr(0, decimal);
  const std::wstring fractionText = decimal == std::wstring::npos ?
    std::wstring() : value.substr(decimal + 1);
  unsigned whole;
  unsigned fraction = 0;
  if (!ToUnsigned(wholeText, whole) ||
      (decimal != std::wstring::npos && fractionText.empty()) ||
      (!fractionText.empty() && !ToUnsigned(fractionText, fraction)))
    return false;

  if (fractionText.size() == 1)
    fraction *= 100;
  else if (fractionText.size() == 2)
    fraction *= 10;

  const unsigned long long valueMilliHz =
    (unsigned long long)whole * 1000 + fraction;
  if (valueMilliHz < 23900 || valueMilliHz > 1000000)
    return false;

  refreshMilliHz = (unsigned)valueMilliHz;
  return true;
}

std::wstring LGFormatRefreshRate(unsigned refreshMilliHz)
{
  std::wstring result = std::to_wstring(refreshMilliHz / 1000);
  const unsigned fraction = refreshMilliHz % 1000;
  if (!fraction)
    return result;

  const wchar_t text[] =
  {
    L'.',
    (wchar_t)(L'0' + fraction / 100),
    (wchar_t)(L'0' + fraction / 10 % 10),
    (wchar_t)(L'0' + fraction % 10),
    L'\0'
  };
  result.append(text);
  return result;
}
