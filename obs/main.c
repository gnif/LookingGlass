#include <obs/obs-module.h>
#include <common/version.h>
#include <stdio.h>

#ifdef _WIN32
#undef EXPORT
#define EXPORT __declspec(dllexport)
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("looking-glass-obs", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
  return "Looking Glass Client";
}

extern struct obs_source_info lg_source;

MODULE_EXPORT bool obs_module_load(void)
{
  printf("Looking Glass OBS Client (%s)\n", BUILD_VERSION);
  obs_register_source(&lg_source);
  return true;
}

#if defined(_WIN32) && defined(__GNUC__)
/* GCC requires a DLL entry point even without any standard library included. */
/* Types extracted from windows.h to avoid polluting the rest of the file. */
int __stdcall DallMainCRTStartup(void* instance, unsigned reason, void* reserved)
{
  (void) instance;
  (void) reason;
  (void) reserved;
  return 1;
}
#endif
