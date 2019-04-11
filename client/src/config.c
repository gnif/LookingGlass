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
#include "common/debug.h"

#include <sys/stat.h>
#include <libconfig.h>
#include <pwd.h>
#include <unistd.h>

static bool load(const char * configFile);
static void doLicense();
static void doHelp(char * app);

bool config_load(int argc, char * argv[])
{
  // duplicate the constants to avoid crashing out when trying to free
  // these values. This is to allow the defaults to be overridden.
  params.shmFile     = strdup(params.shmFile    );
  params.spiceHost   = strdup(params.spiceHost  );
  params.windowTitle = strdup(params.windowTitle);

  // load any global then local config options first
  struct stat st;
  if (stat("/etc/looking-glass.conf", &st) >= 0)
  {
    DEBUG_INFO("Loading config from: /etc/looking-glass.conf");
    if (!load("/etc/looking-glass.conf"))
      return false;
  }

  struct passwd * pw = getpwuid(getuid());
  const char pattern[] = "%s/.looking-glass.conf";
  const size_t len = strlen(pw->pw_dir) + sizeof(pattern);
  char buffer[len];
  snprintf(buffer, len, pattern, pw->pw_dir);
  if (stat(buffer, &st) >= 0)
  {
    DEBUG_INFO("Loading config from: %s", buffer);
    if (!load(buffer))
      return false;
  }

  for(;;)
  {
    switch(getopt(argc, argv, "hC:f:L:s:c:p:jMvK:kg:o:anrdFx:y:w:b:QSGm:lqt:"))
    {
      case '?':
      case 'h':
      default :
        doHelp(argv[0]);
        return false;

      case -1:
        break;

      case 'C':
        params.configFile = optarg;
        if (!load(optarg))
          return false;
        continue;

      case 'f':
        free(params.shmFile);
        params.shmFile = strdup(optarg);
        continue;

      case 'L':
        params.shmSize = atoi(optarg) * 1024 * 1024;
        continue;

      case 's':
      {
        if (strcasecmp("ALL", optarg) == 0)
        {
          params.useSpiceInput     = false;
          params.useSpiceClipboard = false;
        }
        else if (strcasecmp("INPUT"             , optarg) == 0) params.useSpiceInput     = false;
        else if (strcasecmp("CLIPBOARD"         , optarg) == 0) params.useSpiceClipboard = false;
        else if (strcasecmp("CLIPBOARD_TO_VM"   , optarg) == 0) params.clipboardToVM     = false;
        else if (strcasecmp("CLIPBOARD_TO_LOCAL", optarg) == 0) params.clipboardToLocal  = false;
        else
        {
          fprintf(stderr, "Invalid spice feature: %s\n", optarg);
          fprintf(stderr, "Must be one of ALL, INPUT, CLIPBOARD, CLIPBOARD_TO_VM, CLIPBOARD_TO_LOCAL\n");
          doHelp(argv[0]);
          return false;
        }
        continue;
      }

      case 'c':
        free(params.spiceHost);
        params.spiceHost = strdup(optarg);
        continue;

      case 'p':
        params.spicePort = atoi(optarg);
        continue;

      case 'j':
        params.scaleMouseInput = false;
        continue;

      case 'M':
        params.hideMouse = false;
        continue;

      case 'K':
        params.fpsLimit = atoi(optarg);
        continue;

      case 'k':
        params.showFPS = true;
        continue;

      case 'g':
      {
        bool ok = false;
        for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
          if (strcasecmp(LG_Renderers[i]->get_name(), optarg) == 0)
          {
            params.forceRenderer      = true;
            params.forceRendererIndex = i;
            ok = true;
            break;
          }

        if (!ok)
        {
          fprintf(stderr, "No such renderer: %s\n", optarg);
          fprintf(stderr, "Use '-o list' obtain a list of options\n");
          doHelp(argv[0]);
          return false;
        }

        continue;
      }

      case 'o':
      {
        if (strcasecmp(optarg, "list") == 0)
        {
          size_t maxLen = 0;
          for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
          {
            const LG_Renderer * r = LG_Renderers[i];
            for(unsigned int j = 0; j < r->option_count; ++j)
            {
              const size_t len = strlen(r->options[j].name);
              if (len > maxLen)
                maxLen = len;
            }
          }

          fprintf(stderr, "\nRenderer Option List\n");
          for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
          {
            const LG_Renderer * r = LG_Renderers[i];
            fprintf(stderr, "\n%s\n", r->get_name());
            for(unsigned int j = 0; j < r->option_count; ++j)
            {
              const size_t pad = maxLen - strlen(r->options[j].name);
              for(int i = 0; i < pad; ++i)
                fputc(' ', stderr);

              fprintf(stderr, "  %s - %s\n", r->options[j].name, r->options[j].desc);
            }
          }
          fprintf(stderr, "\n");
          return false;
        }

        const LG_Renderer  * renderer = NULL;
        RendererOpts       * opts     = NULL;

        const size_t len  = strlen(optarg);
        const char * name = strtok(optarg, ":");

        for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
          if (strcasecmp(LG_Renderers[i]->get_name(), name) == 0)
          {
            renderer = LG_Renderers[i];
            opts     = &params.rendererOpts[i];
            break;
          }

        if (!renderer)
        {
          fprintf(stderr, "No such renderer: %s\n", name);
          doHelp(argv[0]);
          return false;
        }

        const char * option = strtok(NULL  , "=");
        if (!option)
        {
          fprintf(stderr, "Renderer option name not specified\n");
          doHelp(argv[0]);
          return false;
        }

        const LG_RendererOpt * opt = NULL;
        for(unsigned int i = 0; i < renderer->option_count; ++i)
          if (strcasecmp(option, renderer->options[i].name) == 0)
          {
            opt = &renderer->options[i];
            break;
          }

        if (!opt)
        {
          fprintf(stderr, "Renderer \"%s\" doesn't have the option: %s\n", renderer->get_name(), option);
          doHelp(argv[0]);
          return false;
        }

        const char * value = NULL;
        if (len > strlen(name) + strlen(option) + 2)
          value = option + strlen(option) + 1;

        if (opt->validator && !opt->validator(value))
        {
          fprintf(stderr, "Renderer \"%s\" reported invalid value for option \"%s\"\n", renderer->get_name(), option);
          doHelp(argv[0]);
          return false;
        }

        if (opts->argc == opts->size)
        {
          opts->size += 5;
          opts->argv  = realloc(opts->argv, sizeof(LG_RendererOptValue) * opts->size);
        }

        opts->argv[opts->argc].opt   = opt;
        opts->argv[opts->argc].value = strdup(value);
        ++opts->argc;
        continue;
      }

      case 'a':
        params.autoResize = true;
        continue;

      case 'n':
        params.allowResize = false;
        continue;

      case 'r':
        params.keepAspect = false;
        continue;

      case 'd':
        params.borderless = true;
        continue;

      case 'F':
        params.fullscreen = true;
        continue;

      case 'x':
        params.center = false;
        params.x = atoi(optarg);
        continue;

      case 'y':
        params.center = false;
        params.y = atoi(optarg);
        continue;

      case 'w':
        params.w = atoi(optarg);
        continue;

      case 'b':
        params.h = atoi(optarg);
        continue;

      case 'Q':
        params.ignoreQuit = true;
        continue;

      case 'S':
        params.allowScreensaver = false;
        continue;

      case 'G':
        params.grabKeyboard = false;
        continue;

      case 'm':
        params.escapeKey = atoi(optarg);
        continue;

      case 'q':
        params.disableAlerts = true;
        continue;

      case 't':
        free(params.windowTitle);
        params.windowTitle = strdup(optarg);
        continue;

      case 'l':
        doLicense();
        return false;
    }
    break;
  }

  if (optind != argc)
  {
    fprintf(stderr, "A non option was supplied\n");
    doHelp(argv[0]);
    return false;
  }

  return true;
}

void config_free()
{
  free(params.shmFile    );
  free(params.spiceHost  );
  free(params.windowTitle);

  for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
  {
    RendererOpts * opts = &params.rendererOpts[i];
    for(unsigned int j = 0; j < opts->argc; ++j)
      free(opts->argv[j].value);
    free(opts->argv);
  }
}

static bool load(const char * configFile)
{
  config_t cfg;
  int itmp;
  const char *stmp;

  config_init(&cfg);
  if (!config_read_file(&cfg, configFile))
  {
    DEBUG_ERROR("Config file error %s:%d - %s",
      config_error_file(&cfg),
      config_error_line(&cfg),
      config_error_text(&cfg)
    );
    return false;
  }

  config_setting_t * global = config_lookup(&cfg, "global");
  if (global)
  {
    if (config_setting_lookup_string(global, "shmFile", &stmp))
    {
      free(params.shmFile);
      params.shmFile = strdup(stmp);
    }

    if (config_setting_lookup_int(global, "shmSize", &itmp))
      params.shmSize = itmp * 1024 * 1024;

    if (config_setting_lookup_string(global, "forceRenderer", &stmp))
    {
      bool ok = false;
      for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
        if (strcasecmp(LG_Renderers[i]->get_name(), stmp) == 0)
        {
          params.forceRenderer      = true;
          params.forceRendererIndex = i;
          ok = true;
          break;
        }

      if (!ok)
      {
        DEBUG_ERROR("No such renderer: %s", stmp);
        config_destroy(&cfg);
        return false;
      }
    }

    if (config_setting_lookup_bool(global, "scaleMouseInput" , &itmp)) params.scaleMouseInput  = (itmp != 0);
    if (config_setting_lookup_bool(global, "hideMouse"       , &itmp)) params.hideMouse        = (itmp != 0);
    if (config_setting_lookup_bool(global, "showFPS"         , &itmp)) params.showFPS          = (itmp != 0);
    if (config_setting_lookup_bool(global, "autoResize"      , &itmp)) params.autoResize       = (itmp != 0);
    if (config_setting_lookup_bool(global, "allowResize"     , &itmp)) params.allowResize      = (itmp != 0);
    if (config_setting_lookup_bool(global, "keepAspect"      , &itmp)) params.keepAspect       = (itmp != 0);
    if (config_setting_lookup_bool(global, "borderless"      , &itmp)) params.borderless       = (itmp != 0);
    if (config_setting_lookup_bool(global, "fullScreen"      , &itmp)) params.fullscreen       = (itmp != 0);
    if (config_setting_lookup_bool(global, "ignoreQuit"      , &itmp)) params.ignoreQuit       = (itmp != 0);
    if (config_setting_lookup_bool(global, "allowScreensaver", &itmp)) params.allowScreensaver = (itmp != 0);
    if (config_setting_lookup_bool(global, "disableAlerts"   , &itmp)) params.disableAlerts    = (itmp != 0);

    if (config_setting_lookup_int(global, "x", &params.x)) params.center = false;
    if (config_setting_lookup_int(global, "y", &params.y)) params.center = false;

    if (config_setting_lookup_int(global, "w", &itmp))
    {
      if (itmp < 1)
      {
        DEBUG_ERROR("Invalid window width, must be greater then 1px");
        config_destroy(&cfg);
        return false;
      }
      params.w = (unsigned int)itmp;
    }

    if (config_setting_lookup_int(global, "h", &itmp))
    {
      if (itmp < 1)
      {
        DEBUG_ERROR("Invalid window height, must be greater then 1px");
        config_destroy(&cfg);
        return false;
      }
      params.h = (unsigned int)itmp;
    }

    if (config_setting_lookup_int(global, "fpsLimit", &itmp))
    {
      if (itmp < 1)
      {
        DEBUG_ERROR("Invalid FPS limit, must be greater then 0");
        config_destroy(&cfg);
        return false;
      }
      params.fpsLimit = (unsigned int)itmp;
    }

    if (config_setting_lookup_int(global, "escapeKey", &itmp))
    {
      if (itmp <= SDL_SCANCODE_UNKNOWN || itmp > SDL_SCANCODE_APP2)
      {
        DEBUG_ERROR("Invalid capture key value, see https://wiki.libsdl.org/SDLScancodeLookup");
        config_destroy(&cfg);
        return false;
      }
      params.escapeKey = (SDL_Scancode)itmp;
    }

    if (config_setting_lookup_string(global, "windowTitle", &stmp))
    {
      free(params.windowTitle);
      params.windowTitle = strdup(stmp);
    }

  }

  config_setting_t * spice = config_lookup(&cfg, "spice");
  if (spice)
  {
    if (config_setting_lookup_bool(spice, "useInput", &itmp))
      params.useSpiceInput = (itmp != 0);

    if (config_setting_lookup_bool(spice, "useClipboard", &itmp))
      params.useSpiceClipboard = (itmp != 0);

    if (config_setting_lookup_bool(spice, "clipboardToVM", &itmp))
      params.clipboardToVM = (itmp != 0);

    if (config_setting_lookup_bool(spice, "clipboardToLocal", &itmp))
      params.clipboardToLocal = (itmp != 0);

    if (config_setting_lookup_string(spice, "host", &stmp))
    {
      free(params.spiceHost);
      params.spiceHost = strdup(stmp);
    }

    if (config_setting_lookup_int(spice, "port", &itmp))
    {
      if (itmp < 0 || itmp > 65535)
      {
        DEBUG_ERROR("Invalid spice port");
        config_destroy(&cfg);
        return false;
      }
      params.spicePort = itmp;
    }
  }

  for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
  {
    const LG_Renderer * r     = LG_Renderers[i];
    RendererOpts      * opts  = &params.rendererOpts[i];
    config_setting_t  * group = config_lookup(&cfg, r->get_name());
    if (!group)
      continue;

    for(unsigned int j = 0; j < r->option_count; ++j)
    {
      const char * name = r->options[j].name;
      if (!config_setting_lookup_string(group, name, &stmp))
        continue;

      if (r->options[j].validator && !r->options[j].validator(stmp))
      {
        DEBUG_ERROR("Renderer \"%s\" reported invalid value for option \"%s\"", r->get_name(), name);
        config_destroy(&cfg);
        return false;
      }

      if (opts->argc == opts->size)
      {
        opts->size += 5;
        opts->argv  = realloc(opts->argv, sizeof(LG_RendererOptValue) * opts->size);
      }

      opts->argv[opts->argc].opt   = &r->options[j];
      opts->argv[opts->argc].value = strdup(stmp);
      ++opts->argc;
    }
  }

  config_destroy(&cfg);
  return true;
}

static void doHelp(char * app)
{
  char x[8], y[8];
  snprintf(x, sizeof(x), "%d", params.x);
  snprintf(y, sizeof(y), "%d", params.y);

  fprintf(stderr,
    "\n"
    "Looking Glass Client\n"
    "Usage: %s [OPTION]...\n"
    "Example: %s -h\n"
    "\n"
    "  -h        Print out this help\n"
    "\n"
    "  -C PATH    Specify an additional configuration file to load\n"
    "  -f PATH    Specify the path to the shared memory file [current: %s]\n"
    "  -L SIZE    Specify the size in MB of the shared memory file (0 = detect) [current: %d]\n"
    "\n"
    "  -s FEATURE Disable spice feature (specify multiple times for each feature)\n"
    "\n"
    "               ALL                Disable the spice client entirely\n"
    "               INPUT              Disable spice keyboard & mouse input\n"
    "               CLIPBOARD          Disable spice clipboard support\n"
    "               CLIPBOARD_TO_VM    Disable local clipboard to VM sync\n"
    "               CLIPBOARD_TO_LOCAL Disable VM clipboard to local sync\n"
    "\n"
    "  -c HOST    Specify the spice host or UNIX socket [current: %s]\n"
    "  -p PORT    Specify the spice port or 0 for UNIX socket [current: %d]\n"
    "  -j         Disable cursor position scaling\n"
    "  -M         Don't hide the host cursor\n"
    "\n"
    "  -K         Set the FPS limit [current: %d]\n"
    "  -k         Enable FPS display\n"
    "  -g NAME    Force the use of a specific renderer\n"
    "  -o OPTION  Specify a renderer option (ie: opengl:vsync=0)\n"
    "             Alternatively specify \"list\" to list all renderers and their options\n"
    "\n"
    "  -a         Auto resize the window to the guest\n"
    "  -n         Don't allow the window to be manually resized\n"
    "  -r         Don't maintain the aspect ratio\n"
    "  -d         Borderless mode\n"
    "  -F         Borderless fullscreen mode\n"
    "  -x XPOS    Initial window X position [current: %s]\n"
    "  -y YPOS    Initial window Y position [current: %s]\n"
    "  -w WIDTH   Initial window width [current: %u]\n"
    "  -b HEIGHT  Initial window height [current: %u]\n"
    "  -Q         Ignore requests to quit (ie: Alt+F4)\n"
    "  -S         Disable the screensaver\n"
    "  -G         Don't capture the keyboard in capture mode\n"
    "  -m CODE    Specify the escape key [current: %u (%s)]\n"
    "             See https://wiki.libsdl.org/SDLScancodeLookup for valid values\n"
    "  -q         Disable alert messages [current: %s]\n"
    "  -t TITLE   Use a custom title for the main window\n"
    "\n"
    "  -l         License information\n"
    "\n",
    app,
    app,
    params.shmFile,
    params.shmSize,
    params.spiceHost,
    params.spicePort,
    params.fpsLimit,
    params.center ? "center" : x,
    params.center ? "center" : y,
    params.w,
    params.h,
    params.escapeKey,
    SDL_GetScancodeName(params.escapeKey),
    params.disableAlerts ? "disabled" : "enabled"
  );
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