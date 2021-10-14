/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include "main.h"
#include "config.h"
#include "kb.h"

#include "common/option.h"
#include "common/debug.h"
#include "common/paths.h"
#include "common/stringutils.h"

#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>
#include <string.h>

// forwards
static bool       optRendererParse   (struct Option * opt, const char * str);
static StringList optRendererValues  (struct Option * opt);
static char *     optRendererToString(struct Option * opt);
static bool       optPosParse        (struct Option * opt, const char * str);
static StringList optPosValues       (struct Option * opt);
static char *     optPosToString     (struct Option * opt);
static bool       optSizeParse       (struct Option * opt, const char * str);
static StringList optSizeValues      (struct Option * opt);
static char *     optSizeToString    (struct Option * opt);
static bool       optScancodeValidate(struct Option * opt, const char ** error);
static char *     optScancodeToString(struct Option * opt);
static bool       optRotateValidate  (struct Option * opt, const char ** error);

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
  {
    .module        = "app",
    .name          = "allowDMA",
    .description   = "Allow direct DMA transfers if supported (see `README.md` in the `module` dir)",
    .type          = OPTION_TYPE_BOOL,
    .value.x_bool  = true
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
    .name           = "forceAspect",
    .description    = "Force the window to maintain the aspect ratio",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true,
  },
  {
    .module         = "win",
    .name           = "dontUpscale",
    .description    = "Never try to upscale the window",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {
    .module         = "win",
    .name           = "shrinkOnUpscale",
    .description    = "Limit the window dimensions when dontUpscale is enabled",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
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
    .value.x_bool   = false,
  },
  {
    .module         = "win",
    .name           = "fpsMin",
    .description    = "Frame rate minimum (0 = disable - not recommended, -1 = auto detect)",
    .shortopt       = 'K',
    .type           = OPTION_TYPE_INT,
    .value.x_int    = -1,
  },
  {
    .module         = "win",
    .name           = "ignoreQuit",
    .description    = "Ignore requests to quit (i.e. Alt+F4)",
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
    .name           = "autoScreensaver",
    .description    = "Prevent the screensaver from starting when guest requests it",
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
  {
    .module         = "win",
    .name           = "quickSplash",
    .description    = "Skip fading out the splash screen when a connection is established",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {
    .module         = "win",
    .name           = "rotate",
    .description    = "Rotate the displayed image (0, 90, 180, 270)",
    .type           = OPTION_TYPE_INT,
    .validator      = optRotateValidate,
    .value.x_int    = 0,
  },
  {
    .module         = "win",
    .name           = "uiFont",
    .description    = "The font to use when rendering on-screen UI",
    .type           = OPTION_TYPE_STRING,
    .value.x_string = "DejaVu Sans Mono",
  },
  {
    .module         = "win",
    .name           = "uiSize",
    .description    = "The font size to use when rendering on-screen UI",
    .type           = OPTION_TYPE_INT,
    .value.x_int    = 14
  },
  {
    .module         = "win",
    .name           = "jitRender",
    .description    = "Enable just-in-time rendering",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
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
    .name           = "grabKeyboardOnFocus",
    .description    = "Grab the keyboard when focused",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {
    .module         = "input",
    .name           = "releaseKeysOnFocusLoss",
    .description    = "On focus loss, send key up events to guest for all held keys",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true
  },
  {
    .module         = "input",
    .name           = "escapeKey",
    .description    = "Specify the escape key, see <linux/input-event-codes.h> for valid values",
    .shortopt       = 'm',
    .type           = OPTION_TYPE_INT,
    .value.x_int    = KEY_SCROLLLOCK,
    .validator      = optScancodeValidate,
    .toString       = optScancodeToString,
  },
  {
    .module         = "input",
    .name           = "ignoreWindowsKeys",
    .description    = "Do not pass events for the windows keys to the guest",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false
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
  {
    .module         = "input",
    .name           = "mouseSmoothing",
    .description    = "Apply simple mouse smoothing when rawMouse is not in use (helps reduce aliasing)",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true,
  },
  {
    .module         = "input",
    .name           = "rawMouse",
    .description    = "Use RAW mouse input when in capture mode (good for gaming)",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false,
  },
  {
    .module         = "input",
    .name           = "mouseRedraw",
    .description    = "Mouse movements trigger redraws (ignores FPS minimum)",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true,
  },
  {
    .module         = "input",
    .name           = "autoCapture",
    .description    = "Try to keep the mouse captured when needed",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false
  },
  {
    .module         = "input",
    .name           = "captureOnly",
    .description    = "Only enable input via SPICE if in capture mode",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false
  },
  {
    .module         = "input",
    .name           = "helpMenuDelay",
    .description    = "Show help menu after holding down the escape key for this many milliseconds",
    .type           = OPTION_TYPE_INT,
    .value.x_int    = 200
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
    .description    = "Use SPICE to synchronize the clipboard contents with the guest",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true
  },
  {
    .module         = "spice",
    .name           = "clipboardToVM",
    .description    = "Allow the clipboard to be synchronized TO the VM",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = true
  },
  {
    .module         = "spice",
    .name           = "clipboardToLocal",
    .description    = "Allow the clipboard to be synchronized FROM the VM",
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
  {
    .module         = "spice",
    .name           = "captureOnStart",
    .description    = "Capture mouse and keyboard on start",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false
  },
  {
    .module         = "spice",
    .name           = "alwaysShowCursor",
    .description    = "Always show host cursor",
    .type           = OPTION_TYPE_BOOL,
    .value.x_bool   = false
  },
  {
    .module        = "spice",
    .name          = "showCursorDot",
    .description   = "Use a \"dot\" cursor when the window does not have focus",
    .type          = OPTION_TYPE_BOOL,
    .value.x_bool  = true
  },
  {0}
};

void config_init(void)
{
  g_params.center = true;
  g_params.w      = 1024;
  g_params.h      = 768;

  option_register(options);
}

bool config_load(int argc, char * argv[])
{
  // load any global options first
  struct stat st;
  if (stat("/etc/looking-glass-client.ini", &st) >= 0 && S_ISREG(st.st_mode))
  {
    DEBUG_INFO("Loading config from: /etc/looking-glass-client.ini");
    if (!option_load("/etc/looking-glass-client.ini"))
      return false;
  }

  // load config from user's home directory
  struct passwd * pw = getpwuid(getuid());
  char * localFile;
  alloc_sprintf(&localFile, "%s/.looking-glass-client.ini", pw->pw_dir);
  if (stat(localFile, &st) >= 0 && S_ISREG(st.st_mode))
  {
    DEBUG_INFO("Loading config from: %s", localFile);
    if (!option_load(localFile))
    {
      free(localFile);
      return false;
    }
  }
  free(localFile);

  // load config from XDG_CONFIG_HOME
  char * xdgFile;
  alloc_sprintf(&xdgFile, "%s/client.ini", lgConfigDir());

  if (xdgFile && stat(xdgFile, &st) >= 0 && S_ISREG(st.st_mode))
  {
    DEBUG_INFO("Loading config from: %s", xdgFile);
    if (!option_load(xdgFile))
    {
      free(xdgFile);
      return false;
    }
  }
  free(xdgFile);

  // parse the command line arguments
  if (!option_parse(argc, argv))
    return false;

  // if a file was specified to also load, do it
  const char * configFile = option_get_string("app", "configFile");
  if (configFile)
  {
    if (stat(configFile, &st) < 0 || !S_ISREG(st.st_mode))
    {
      DEBUG_ERROR("app:configFile set to invalid file: %s", configFile);
      return false;
    }

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
  g_params.cursorPollInterval = option_get_int   ("app"  , "cursorPollInterval");
  g_params.framePollInterval  = option_get_int   ("app"  , "framePollInterval" );
  g_params.allowDMA           = option_get_bool  ("app"  , "allowDMA"          );

  g_params.windowTitle     = option_get_string("win", "title"          );
  g_params.autoResize      = option_get_bool  ("win", "autoResize"     );
  g_params.allowResize     = option_get_bool  ("win", "allowResize"    );
  g_params.keepAspect      = option_get_bool  ("win", "keepAspect"     );
  g_params.forceAspect     = option_get_bool  ("win", "forceAspect"    );
  g_params.dontUpscale     = option_get_bool  ("win", "dontUpscale"    );
  g_params.shrinkOnUpscale = option_get_bool  ("win", "shrinkOnUpscale");
  g_params.borderless      = option_get_bool  ("win", "borderless"     );
  g_params.fullscreen      = option_get_bool  ("win", "fullScreen"     );
  g_params.maximize        = option_get_bool  ("win", "maximize"       );
  g_params.fpsMin          = option_get_int   ("win", "fpsMin"         );
  g_params.ignoreQuit      = option_get_bool  ("win", "ignoreQuit"     );
  g_params.noScreensaver   = option_get_bool  ("win", "noScreensaver"  );
  g_params.autoScreensaver = option_get_bool  ("win", "autoScreensaver");
  g_params.showAlerts      = option_get_bool  ("win", "alerts"         );
  g_params.quickSplash     = option_get_bool  ("win", "quickSplash"    );
  g_params.uiFont          = option_get_string("win"  , "uiFont"       );
  g_params.uiSize          = option_get_int   ("win"  , "uiSize"       );
  g_params.jitRender       = option_get_bool  ("win"  , "jitRender"    );

  if (g_params.noScreensaver && g_params.autoScreensaver)
  {
    DEBUG_WARN("win:noScreensaver (-S) and win:autoScreensaver "
        "can't be used simultaneously");
    return false;
  }

  switch(option_get_int("win", "rotate"))
  {
    case 0  : g_params.winRotate = LG_ROTATE_0  ; break;
    case 90 : g_params.winRotate = LG_ROTATE_90 ; break;
    case 180: g_params.winRotate = LG_ROTATE_180; break;
    case 270: g_params.winRotate = LG_ROTATE_270; break;
  }

  g_params.grabKeyboard           = option_get_bool("input", "grabKeyboard"          );
  g_params.grabKeyboardOnFocus    = option_get_bool("input", "grabKeyboardOnFocus"   );
  g_params.releaseKeysOnFocusLoss = option_get_bool("input", "releaseKeysOnFocusLoss");
  g_params.escapeKey              = option_get_int ("input", "escapeKey"             );
  g_params.ignoreWindowsKeys      = option_get_bool("input", "ignoreWindowsKeys"     );
  g_params.hideMouse              = option_get_bool("input", "hideCursor"            );
  g_params.mouseSens              = option_get_int ("input", "mouseSens"             );
  g_params.mouseSmoothing         = option_get_bool("input", "mouseSmoothing"        );
  g_params.rawMouse               = option_get_bool("input", "rawMouse"              );
  g_params.mouseRedraw            = option_get_bool("input", "mouseRedraw"           );
  g_params.autoCapture            = option_get_bool("input", "autoCapture"           );
  g_params.captureInputOnly       = option_get_bool("input", "captureOnly"           );

  if (g_params.jitRender && !g_params.mouseRedraw)
  {
    DEBUG_WARN("win:jitRender is enabled, forcing input:mouseRedraw");
    g_params.mouseRedraw = true;
  }

  g_params.helpMenuDelayUs = option_get_int("input", "helpMenuDelay") * (uint64_t) 1000;

  g_params.minimizeOnFocusLoss = option_get_bool("win", "minimizeOnFocusLoss");

  if (option_get_bool("spice", "enable"))
  {
    g_params.spiceHost         = option_get_string("spice", "host");
    g_params.spicePort         = option_get_int   ("spice", "port");

    g_params.useSpiceInput     = option_get_bool("spice", "input"    );
    g_params.useSpiceClipboard = option_get_bool("spice", "clipboard");

    if (g_params.useSpiceClipboard)
    {
      g_params.clipboardToVM     = option_get_bool("spice", "clipboardToVM"   );
      g_params.clipboardToLocal  = option_get_bool("spice", "clipboardToLocal");
      g_params.useSpiceClipboard = g_params.clipboardToVM || g_params.clipboardToLocal;
    }
    else
    {
      g_params.clipboardToVM    = false;
      g_params.clipboardToLocal = false;
    }

    g_params.scaleMouseInput  = option_get_bool("spice", "scaleCursor");
    g_params.captureOnStart   = option_get_bool("spice", "captureOnStart");
    g_params.alwaysShowCursor = option_get_bool("spice", "alwaysShowCursor");
    g_params.showCursorDot    = option_get_bool("spice", "showCursorDot");
  }

  return true;
}

void config_free(void)
{
  option_free();
}

static void doLicense(void)
{
  fprintf(stderr,
    // BEGIN LICENSE BLOCK
    "\n"
    "Looking Glass\n"
    "Copyright © 2017-2021 The Looking Glass Authors\n"
    "https://looking-glass.io\n"
    "\n"
    "This program is free software; you can redistribute it and/or modify it under\n"
    "the terms of the GNU General Public License as published by the Free Software\n"
    "Foundation; either version 2 of the License, or (at your option) any later\n"
    "version.\n"
    "\n"
    "This program is distributed in the hope that it will be useful, but WITHOUT ANY\n"
    "WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A\n"
    "PARTICULAR PURPOSE. See the GNU General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU General Public License along with\n"
    "this program; if not, write to the Free Software Foundation, Inc., 59 Temple\n"
    "Place, Suite 330, Boston, MA 02111-1307 USA\n"
    "\n"
    // END LICENSE BLOCK
  );
}

static bool optRendererParse(struct Option * opt, const char * str)
{
  if (!str)
    return false;

  if (strcasecmp(str, "auto") == 0)
  {
    g_params.forceRenderer = false;
    return true;
  }

  for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
    if (strcasecmp(str, LG_Renderers[i]->getName()) == 0)
    {
      g_params.forceRenderer      = true;
      g_params.forceRendererIndex = i;
      return true;
    }

  return false;
}

static StringList optRendererValues(struct Option * opt)
{
  StringList sl = stringlist_new(false);

  // this typecast is safe as the stringlist doesn't own the values
  for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
    stringlist_push(sl, (char *)LG_Renderers[i]->getName());

  return sl;
}

static char * optRendererToString(struct Option * opt)
{
  if (!g_params.forceRenderer)
    return strdup("auto");

  if (g_params.forceRendererIndex >= LG_RENDERER_COUNT)
    return NULL;

  return strdup(LG_Renderers[g_params.forceRendererIndex]->getName());
}

static bool optPosParse(struct Option * opt, const char * str)
{
  if (!str)
    return false;

  if (strcmp(str, "center") == 0)
  {
    g_params.center = true;
    return true;
  }

  if (sscanf(str, "%dx%d", &g_params.x, &g_params.y) == 2)
  {
    g_params.center = false;
    return true;
  }

  return false;
}

static StringList optPosValues(struct Option * opt)
{
  StringList sl = stringlist_new(false);
  stringlist_push(sl, "center");
  stringlist_push(sl, "<left>x<top>, e.g. 100x100");
  return sl;
}

static char * optPosToString(struct Option * opt)
{
  if (g_params.center)
    return strdup("center");

  int len = snprintf(NULL, 0, "%dx%d", g_params.x, g_params.y);
  char * str = malloc(len + 1);
  sprintf(str, "%dx%d", g_params.x, g_params.y);

  return str;
}

static bool optSizeParse(struct Option * opt, const char * str)
{
  if (!str)
    return false;

  if (sscanf(str, "%ux%u", &g_params.w, &g_params.h) == 2)
  {
    if (g_params.w < 1 || g_params.h < 1)
      return false;
    return true;
  }

  return false;
}

static StringList optSizeValues(struct Option * opt)
{
  StringList sl = stringlist_new(false);
  stringlist_push(sl, "<left>x<top>, e.g. 100x100");
  return sl;
}

static char * optSizeToString(struct Option * opt)
{
  int len = snprintf(NULL, 0, "%ux%u", g_params.w, g_params.h);
  char * str = malloc(len + 1);
  sprintf(str, "%ux%u", g_params.w, g_params.h);

  return str;
}

static bool optScancodeValidate(struct Option * opt, const char ** error)
{
  if (opt->value.x_int >= 0 && opt->value.x_int < KEY_MAX)
    return true;

  *error = "Out of range";
  return false;
}

static char * optScancodeToString(struct Option * opt)
{
  char * str;
  alloc_sprintf(&str, "%d = %s", opt->value.x_int,
      linux_to_str[opt->value.x_int]);
  return str;
}

static bool optRotateValidate(struct Option * opt, const char ** error)
{
  switch(opt->value.x_int)
  {
    case   0:
    case  90:
    case 180:
    case 270:
      return true;
  }

  *error = "Rotation angle must be one of 0, 90, 180 or 270";
  return false;
}
