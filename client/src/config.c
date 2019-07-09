/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#include "main.h"
#include "config.h"
#include "common/option.h"
#include "common/debug.h"
#include "common/stringutils.h"

#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>

// forwards
static bool       optRendererParse       (struct Option * opt, const char * str);
static StringList optRendererValues      (struct Option * opt);
static char *     optRendererToString    (struct Option * opt);
static bool       optPosParse            (struct Option * opt, const char * str);
static StringList optPosValues           (struct Option * opt);
static char *     optPosToString         (struct Option * opt);
static bool       optSizeParse           (struct Option * opt, const char * str);
static StringList optSizeValues          (struct Option * opt);
static char *     optSizeToString        (struct Option * opt);
static char *     optScancodeToString    (struct Option * opt);

static void doLicense();

static struct Option options[] =
{
  // app options
  {
    .module         = "app",
    .name           = "configFile",
    .description    = "A file to read additional configuration from",
    .shortopt       = 'C',
    .type           = OPTION_TYPE_STRING,
    .value.x_string = NULL,
  },
  {
    .module         = "app",
    .name           = "shmFile",
    .description    = "The path to the shared memory file",
    .shortopt       = 'f',
    .type           = OPTION_TYPE_STRING,
    .value.x_string = "/dev/shm/looking-glass",
  },
  {
    .module         = "app",
    .name           = "shmSize",
    .description    = "Specify the size in MB of the shared memory file (0 = detect)",
    .shortopt       = 'L',
    .type           = OPTION_TYPE_INT,
    .value.x_int    = 0,
  },
  {
    .module        = "app",
    .name          = "renderer",
    .description   = "Specify the renderer to use",
    .shortopt      = 'g',
    .type          = OPTION_TYPE_CUSTOM,
    .parser        = optRendererParse,
    .getValues     = optRendererValues,
    .toString      = optRendererToString
  },
  {
    .module         = "app",
    .name           = "license",
    .description    = "Show the license for this application and then terminate",
    .shortopt       = 'l',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {
    .module        = "app",
    .name          = "cursorPollInterval",
    .description   = "How often to check for a cursor update in microseconds",
    .type          = OPTION_TYPE_INT,
    .value.x_int   = 1000
  },
  {
    .module        = "app",
    .name          = "framePollInterval",
    .description   = "How often to check for a frame update in microseconds",
    .type          = OPTION_TYPE_INT,
    .value.x_int   = 1000
  },

  // window options
  {
    .module         = "win",
    .name           = "title",
    .description    = "The window title",
    .type           = OPTION_TYPE_STRING,
    .value.x_string = "Looking Glass (client)"
  },
  {
    .module         = "win",
    .name           = "position",
    .description    = "Initial window position at startup",
    .type           = OPTION_TYPE_CUSTOM,
    .parser         = optPosParse,
    .getValues      = optPosValues,
    .toString       = optPosToString
  },
  {
    .module         = "win",
    .name           = "size",
    .description    = "Initial window size at startup",
    .type           = OPTION_TYPE_CUSTOM,
    .parser         = optSizeParse,
    .getValues      = optSizeValues,
    .toString       = optSizeToString
  },
  {
    .module         = "win",
    .name           = "autoResize",
    .description    = "Auto resize the window to the guest",
    .shortopt       = 'a',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {
    .module         = "win",
    .name           = "allowResize",
    .description    = "Allow the window to be manually resized",
    .shortopt       = 'n',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true,
  },
  {
    .module         = "win",
    .name           = "keepAspect",
    .description    = "Maintain the correct aspect ratio",
    .shortopt       = 'r',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true,
  },
  {
    .module         = "win",
    .name           = "borderless",
    .description    = "Borderless mode",
    .shortopt       = 'd',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {
    .module         = "win",
    .name           = "fullScreen",
    .description    = "Launch in fullscreen borderless mode",
    .shortopt       = 'F',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {
    .module         = "win",
    .name           = "maximize",
    .description    = "Launch window maximized",
    .shortopt       = 'T',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {
    .module         = "win",
    .name           = "minimizeOnFocusLoss",
    .description    = "Minimize window on focus loss",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true,
  },
  {
    .module         = "win",
    .name           = "fpsLimit",
    .description    = "Frame rate limit (0 = disable - not recommended, -1 = auto detect)",
    .shortopt       = 'K',
    .type           = OPTION_TYPE_INT,
    .value.x_int    = -1,
  },
  {
    .module         = "win",
    .name           = "showFPS",
    .description    = "Enable the FPS & UPS display",
    .shortopt       = 'k',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {
    .module         = "win",
    .name           = "ignoreQuit",
    .description    = "Ignore requests to quit (ie: Alt+F4)",
    .shortopt       = 'Q',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {
    .module         = "win",
    .name           = "noScreensaver",
    .description    = "Prevent the screensaver from starting",
    .shortopt       = 'S',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {
    .module         = "win",
    .name           = "alerts",
    .description    = "Show on screen alert messages",
    .shortopt       = 'q',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true,
  },

  // input options
  {
    .module         = "input",
    .name           = "grabKeyboard",
    .description    = "Grab the keyboard in capture mode",
    .shortopt       = 'G',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true,
  },
  {
    .module         = "input",
    .name           = "escapeKey",
    .description    = "Specify the escape key, see https://wiki.libsdl.org/SDLScancodeLookup for valid values",
    .shortopt       = 'm',
    .type           = OPTION_TYPE_INT,
    .value.x_int    = SDL_SCANCODE_SCROLLLOCK,
    .toString       = optScancodeToString
  },
  {
    .module         = "input",
    .name           = "hideCursor",
    .description    = "Hide the local mouse cursor",
    .shortopt       = 'M',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true,
  },
  {
    .module         = "input",
    .name           = "mouseSens",
    .description    = "Initial mouse sensitivity when in capture mode (-9 to 9)",
    .type           = OPTION_TYPE_INT,
    .value.x_int    = 0,
  },

  // spice options
  {
    .module         = "spice",
    .name           = "enable",
    .description    = "Enable the built in SPICE client for input and/or clipboard support",
    .shortopt       = 's',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true
  },
  {
    .module         = "spice",
    .name           = "host",
    .description    = "The SPICE server host or UNIX socket",
    .shortopt       = 'c',
    .type           = OPTION_TYPE_STRING,
    .value.x_string = "127.0.0.1"
  },
  {
    .module         = "spice",
    .name           = "port",
    .description    = "The SPICE server port (0 = unix socket)",
    .shortopt       = 'p',
    .type           = OPTION_TYPE_INT,
    .value.x_int    = 5900
  },
  {
    .module         = "spice",
    .name           = "input",
    .description    = "Use SPICE to send keyboard and mouse input events to the guest",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true
  },
  {
    .module         = "spice",
    .name           = "clipboard",
    .description    = "Use SPICE to syncronize the clipboard contents with the guest",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true
  },
  {
    .module         = "spice",
    .name           = "clipboardToVM",
    .description    = "Allow the clipboard to be syncronized TO the VM",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true
  },
  {
    .module         = "spice",
    .name           = "clipboardToLocal",
    .description    = "Allow the clipboard to be syncronized FROM the VM",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true
  },
  {
    .module         = "spice",
    .name           = "scaleCursor",
    .description    = "Scale cursor input position to screen size when up/down scaled",
    .shortopt       = 'j',
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true
  },
  {0}
};

void config_init()
{
  params.center = true;
  params.w      = 1024;
  params.h      = 768;

  option_register(options);
}

bool config_load(int argc, char * argv[])
{
  // load any global options first
  struct stat st;
  if (stat("/etc/looking-glass-client.ini", &st) >= 0)
  {
    DEBUG_INFO("Loading config from: /etc/looking-glass-client.ini");
    if (!option_load("/etc/looking-glass-client.ini"))
      return false;
  }

  // load user's local options
  struct passwd * pw = getpwuid(getuid());
  char * localFile;
  alloc_sprintf(&localFile, "%s/.looking-glass-client.ini", pw->pw_dir);
  if (stat(localFile, &st) >= 0)
  {
    DEBUG_INFO("Loading config from: %s", localFile);
    if (!option_load(localFile))
    {
      free(localFile);
      return false;
    }
  }
  free(localFile);

  // parse the command line arguments
  if (!option_parse(argc, argv))
    return false;

  // if a file was specified to also load, do it
  const char * configFile = option_get_string("app", "configFile");
  if (configFile)
  {
    DEBUG_INFO("Loading config from: %s", configFile);
    if (!option_load(configFile))
      return false;
  }

  // validate the values are sane
  if (!option_validate())
    return false;

  if (option_get_bool("app", "license"))
  {
    doLicense();
    return false;
  }

  // setup the application params for the basic types
  params.shmFile            = option_get_string("app", "shmFile"           );
  params.shmSize            = option_get_int   ("app", "shmSize"           ) * 1048576;
  params.cursorPollInterval = option_get_int   ("app", "cursorPollInterval");
  params.framePollInterval  = option_get_int   ("app", "framePollInterval" );

  params.windowTitle   = option_get_string("win", "title"        );
  params.autoResize    = option_get_bool  ("win", "autoResize"   );
  params.allowResize   = option_get_bool  ("win", "allowResize"  );
  params.keepAspect    = option_get_bool  ("win", "keepAspect"   );
  params.borderless    = option_get_bool  ("win", "borderless"   );
  params.fullscreen    = option_get_bool  ("win", "fullScreen"   );
  params.maximize      = option_get_bool  ("win", "maximize"     );
  params.fpsLimit      = option_get_int   ("win", "fpsLimit"     );
  params.showFPS       = option_get_bool  ("win", "showFPS"      );
  params.ignoreQuit    = option_get_bool  ("win", "ignoreQuit"   );
  params.noScreensaver = option_get_bool  ("win", "noScreensaver");
  params.showAlerts    = option_get_bool  ("win", "alerts"       );

  params.grabKeyboard  = option_get_bool  ("input", "grabKeyboard");
  params.escapeKey     = option_get_int   ("input", "escapeKey"   );
  params.hideMouse     = option_get_bool  ("input", "hideCursor"  );
  params.mouseSens     = option_get_int   ("input", "mouseSens"   );

  params.minimizeOnFocusLoss = option_get_bool("win", "minimizeOnFocusLoss");

  if (option_get_bool("spice", "enable"))
  {
    params.spiceHost         = option_get_string("spice", "host");
    params.spicePort         = option_get_int   ("spice", "port");

    params.useSpiceInput     = option_get_bool("spice", "input"    );
    params.useSpiceClipboard = option_get_bool("spice", "clipboard");

    if (params.useSpiceClipboard)
    {
      params.clipboardToVM    = option_get_bool("spice", "clipboardToVM"   );
      params.clipboardToLocal = option_get_bool("spice", "clipboardToLocal");

      if (!params.clipboardToVM && !params.clipboardToLocal)
        params.useSpiceClipboard = false;
    }

    params.scaleMouseInput = option_get_bool("spice", "scaleCursor");
  }

  return true;
}

void config_free()
{
  option_free();
}

static void doLicense()
{
  fprintf(stderr,
    "\n"
    "Looking Glass - KVM FrameRelay (KVMFR) Client\n"
    "Copyright(C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>\n"
    "https://looking-glass.hostfission.com\n"
    "\n"
    "This program is free software; you can redistribute it and / or modify it under\n"
    "the terms of the GNU General Public License as published by the Free Software\n"
    "Foundation; either version 2 of the License, or (at your option) any later\n"
    "version.\n"
    "\n"
    "This program is distributed in the hope that it will be useful, but WITHOUT ANY\n"
    "WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A\n"
    "PARTICULAR PURPOSE.See the GNU General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU General Public License along with\n"
    "this program; if not, write to the Free Software Foundation, Inc., 59 Temple\n"
    "Place, Suite 330, Boston, MA 02111 - 1307 USA\n"
    "\n"
  );
}

static bool optRendererParse(struct Option * opt, const char * str)
{
  if (strcasecmp(str, "auto") == 0)
  {
    params.forceRenderer = false;
    return true;
  }

  for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
    if (strcasecmp(str, LG_Renderers[i]->get_name()) == 0)
    {
      params.forceRenderer      = true;
      params.forceRendererIndex = i;
      return true;
    }

  return false;
}

static StringList optRendererValues(struct Option * opt)
{
  StringList sl = stringlist_new(false);

  // this typecast is safe as the stringlist doesn't own the values
  for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
    stringlist_push(sl, (char *)LG_Renderers[i]->get_name());

  return sl;
}

static char * optRendererToString(struct Option * opt)
{
  if (!params.forceRenderer)
    return strdup("auto");

  if (params.forceRendererIndex >= LG_RENDERER_COUNT)
    return NULL;

  return strdup(LG_Renderers[params.forceRendererIndex]->get_name());
}

static bool optPosParse(struct Option * opt, const char * str)
{
  if (strcmp(str, "center") == 0)
  {
    params.center = true;
    return true;
  }

  if (sscanf(str, "%dx%d", &params.x, &params.y) == 2)
  {
    params.center = false;
    return true;
  }

  return false;
}

static StringList optPosValues(struct Option * opt)
{
  StringList sl = stringlist_new(false);
  stringlist_push(sl, "center");
  stringlist_push(sl, "<left>x<top>, ie: 100x100");
  return sl;
}

static char * optPosToString(struct Option * opt)
{
  if (params.center)
    return strdup("center");

  int len = snprintf(NULL, 0, "%dx%d", params.x, params.y);
  char * str = malloc(len + 1);
  sprintf(str, "%dx%d", params.x, params.y);

  return str;
}

static bool optSizeParse(struct Option * opt, const char * str)
{
  if (sscanf(str, "%dx%d", &params.w, &params.h) == 2)
  {
    if (params.w < 1 || params.h < 1)
      return false;
    return true;
  }

  return false;
}

static StringList optSizeValues(struct Option * opt)
{
  StringList sl = stringlist_new(false);
  stringlist_push(sl, "<left>x<top>, ie: 100x100");
  return sl;
}

static char * optSizeToString(struct Option * opt)
{
  int len = snprintf(NULL, 0, "%dx%d", params.w, params.h);
  char * str = malloc(len + 1);
  sprintf(str, "%dx%d", params.w, params.h);

  return str;
}

static char * optScancodeToString(struct Option * opt)
{
  char * str;
  alloc_sprintf(&str, "%d = %s", opt->value.x_int, SDL_GetScancodeName(opt->value.x_int));
  return str;
}
