/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#pragma once

#include <windows.h>
#include <stdint.h>
#include "common/debug.h"

#ifdef ENABLE_TRACING
  #define TRACE          TraceUtil::Trace(__FUNCTION__, __LINE__)
  #define TRACE_START(x) TraceUtil::TraceStart(x)
  #define TRACE_END      TraceUtil::TraceEnd()
#else
  #define TRACE
  #define TRACE_START(x)
  #define TRACE_END
#endif

class TraceUtil
{
private:
  static double m_freq;
  static LARGE_INTEGER m_last;
  static LARGE_INTEGER m_start;
  static const char * m_traceName;

public:
  static void Initialize()
  {
#ifdef ENABLE_TRACING
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&now);
    m_freq = (double)now.QuadPart / 1000.0;
    QueryPerformanceCounter(&m_last);
#endif
  }

  static inline void Trace(const char * function, const unsigned int line)
  {    
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double diff = (now.QuadPart - m_last.QuadPart) / m_freq;
    m_last = now;

    DEBUG_INFO("Trace [%8.4f] %s:%u", diff, function, line);
  }

  static inline void TraceStart(const char * traceName)
  {
    QueryPerformanceCounter(&m_start);
    m_traceName = traceName;
  }

  static inline void TraceEnd()
  {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double diff = (now.QuadPart - m_start.QuadPart) / m_freq;

    DEBUG_INFO("Trace [%8.4f] %s", diff, m_traceName);
  }
};