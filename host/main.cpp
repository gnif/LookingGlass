#include <Windows.h>
#include <tchar.h>
#include "common\debug.h"

#include "Service.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdParam, int iCmdShow)
{
#ifdef DEBUG
  AllocConsole();
#endif

  Service *svc = svc->Get();
  if (!svc->Initialize())
  {
    DEBUG_ERROR("Failed to initialize service");
    return -1;
  }

  while (true)
    svc->Process();

  svc->DeInitialize();
  return 0;
}