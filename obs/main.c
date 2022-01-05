/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#include <obs/obs-module.h>
#include <common/version.h>
#include <stdio.h>

#include "common/debug.h"

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
  debug_init();
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
