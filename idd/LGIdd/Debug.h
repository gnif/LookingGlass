#pragma once

#include <Windows.h>
#include <tchar.h>

VOID _DBGPRINT(PCSTR kszFunction, INT iLineNumber, LPCSTR kszDebugFormatString, ...);
#define DBGPRINT(kszDebugFormatString, ...) \
  _DBGPRINT(__FUNCTION__, __LINE__, kszDebugFormatString, __VA_ARGS__)