/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include "common/crash.h"
#include "common/debug.h"
#include "common/version.h"

#ifdef ENABLE_BACKTRACE

#include <stdio.h>
#include <inttypes.h>
#include <windows.h>
#include <dbghelp.h>

static const char * exception_name(DWORD code)
{
  switch (code)
  {
    case EXCEPTION_ACCESS_VIOLATION:
      return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:
      return "BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:
      return "FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
      return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:
      return "FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:
      return "FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:
      return "FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:
      return "FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:
      return "FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
      return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
      return "INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:
      return "INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
      return "NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:
      return "PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:
      return "SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:
      return "STACK_OVERFLOW";
    default:
      return "unknown";
  }
}

static LONG CALLBACK exception_filter(EXCEPTION_POINTERS * exc)
{
  PEXCEPTION_RECORD excInfo = exc->ExceptionRecord;
  CONTEXT context;
  memcpy(&context, exc->ContextRecord, sizeof context);

  DEBUG_ERROR("==== FATAL CRASH (%s) ====", BUILD_VERSION);
  DEBUG_ERROR("exception 0x%08lx (%s), address is %p", excInfo->ExceptionCode,
    exception_name(excInfo->ExceptionCode), excInfo->ExceptionAddress);

  if (!SymInitialize(GetCurrentProcess(), NULL, TRUE))
  {
    DEBUG_ERROR("Failed to SymInitialize: 0x%08lx, could not generate stack trace", GetLastError());
    goto fail;
  }
  SymSetOptions(SYMOPT_LOAD_LINES);

  STACKFRAME64 frame     = { 0 };
  frame.AddrPC.Offset    = context.Rip;
  frame.AddrPC.Mode      = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode   = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode   = AddrModeFlat;

  HANDLE hProcess = GetCurrentProcess();
  HANDLE hThread  = GetCurrentThread();

  for (int i = 1; StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread, &frame, &context, NULL,
                              SymFunctionTableAccess64, SymGetModuleBase64, NULL); ++i)
  {
    DWORD64 moduleBase = SymGetModuleBase64(hProcess, frame.AddrPC.Offset);
    char moduleName[MAX_PATH];

    if (moduleBase && GetModuleFileNameA((HMODULE) moduleBase, moduleName, MAX_PATH))
    {
      DWORD64 disp;

      char symbolBuf[sizeof(SYMBOL_INFO) + 255];
      PSYMBOL_INFO symbol = (PSYMBOL_INFO) symbolBuf;
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
      symbol->MaxNameLen = 256;

      if (SymFromAddr(hProcess, frame.AddrPC.Offset, &disp, symbol))
      {
        IMAGEHLP_LINE line = { sizeof(IMAGEHLP_LINE), 0 };
        DWORD lineDisp;

        if (SymGetLineFromAddr64(hProcess, frame.AddrPC.Offset, &lineDisp, &line))
          DEBUG_ERROR("[trace]: %2d: %s:%s+0x%" PRIx64 " (%s:%ld+0x%lx)", i, moduleName, symbol->Name, disp,
            line.FileName, line.LineNumber, lineDisp);
        else
          DEBUG_ERROR("[trace]: %2d: %s:%s+0x%" PRIx64, i, moduleName, symbol->Name, disp);
      }
      else
        DEBUG_ERROR("[trace]: %2d: %s+0x%08" PRIx64, i, moduleName, frame.AddrPC.Offset - moduleBase);
    }
    else
      DEBUG_ERROR("[trace]: %2d: 0x%016" PRIx64, i, frame.AddrPC.Offset);
  }

  SymCleanup(hProcess);

fail:
  fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;
}

bool installCrashHandler(const char * exe)
{
  SetUnhandledExceptionFilter(exception_filter);
  return true;
}

#else
bool installCrashHandler(const char * exe)
{
  return true;
}
#endif
