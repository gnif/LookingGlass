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

#include "interface/displayserver.h"

#include "x11.h"
#include "atoms.h"
#include "clipboard.h"
#include "resources/icondata.h"

#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>

#include <X11/extensions/XInput2.h>
#include <X11/extensions/scrnsaver.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xpresent.h>
#include <X11/Xcursor/Xcursor.h>

#include <xkbcommon/xkbcommon.h>

#include <GL/glx.h>
#include <GL/glxext.h>

#ifdef ENABLE_EGL
#include <EGL/eglext.h>
#include "egl_dynprocs.h"
#include "eglutil.h"
#endif

#include "app.h"
#include "common/debug.h"
#include "common/time.h"
#include "common/event.h"
#include "util.h"

#define _NET_WM_STATE_REMOVE 0
#define _NET_WM_STATE_ADD    1
#define _NET_WM_STATE_TOGGLE 2

struct X11DSState x11;

struct MwmHints
{
  unsigned long flags;
  unsigned long functions;
  unsigned long decorations;
  long          input_mode;
  unsigned long status;
};

enum {
  MWM_HINTS_FUNCTIONS   = (1L << 0),
  MWM_HINTS_DECORATIONS = (1L << 1),

  MWM_FUNC_ALL          = (1L << 0),
  MWM_FUNC_RESIZE       = (1L << 1),
  MWM_FUNC_MOVE         = (1L << 2),
  MWM_FUNC_MINIMIZE     = (1L << 3),
  MWM_FUNC_MAXIMIZE     = (1L << 4),
  MWM_FUNC_CLOSE        = (1L << 5)
};

// forwards
static void x11SetFullscreen(bool fs);
static int  x11EventThread(void * unused);
static void x11XInputEvent(XGenericEventCookie *cookie);
static void x11XPresentEvent(XGenericEventCookie *cookie);
static void x11GrabPointer(void);

static void x11DoPresent(uint64_t msc)
{
  static bool startup = true;
  if (startup)
  {
    XPresentPixmap(
      x11.display,
      x11.window,
      x11.presentPixmap,
      x11.presentSerial++,
      x11.presentRegion, // valid
      x11.presentRegion, // update
      0,                 // x_off,
      0,                 // y_off,
      None,              // target_crtc
      None,              // wait_fence
      None,              // idle_fence
      PresentOptionNone, // options
      0,                 // target_msc,
      0,                 // divisor,
      0,                 // remainder,
      NULL,              // notifies
      0                  // nnotifies
    );
    startup = false;
    return;
  }

  static bool first   = true;
  static uint64_t lastMsc = 0;

  uint64_t refill;
  if (!first)
  {
    const uint64_t delta = (lastMsc >= msc) ?
      lastMsc - msc :
      ~0ULL - msc + lastMsc;

    if (delta > 50)
      return;

    refill = 50 - delta;
  }
  else
  {
    refill  = 50;
    first   = false;
    lastMsc = msc;
  }

  if (refill < 25)
    return;

  for(int i = 0; i < refill; ++i)
  {
    XPresentPixmap(
      x11.display,
      x11.window,
      x11.presentPixmap,
      x11.presentSerial++,
      x11.presentRegion, // valid
      x11.presentRegion, // update
      0,                 // x_off,
      0,                 // y_off,
      None,              // target_crtc
      None,              // wait_fence
      None,              // idle_fence
      PresentOptionNone, // options
      ++lastMsc,         // target_msc,
      0,                 // divisor,
      0,                 // remainder,
      NULL,              // notifies
      0                  // nnotifies
    );
  }
}

static void x11Setup(void)
{
}

static bool x11Probe(void)
{
  return getenv("DISPLAY") != NULL;
}

static bool x11EarlyInit(void)
{
  XInitThreads();
  return true;
}

static void x11CheckEWMHSupport(void)
{
  if (x11atoms._NET_SUPPORTING_WM_CHECK == None ||
      x11atoms._NET_SUPPORTED == None)
    return;

  Atom type;
  int fmt;
  unsigned long num, bytes;
  unsigned char *data;

  if (XGetWindowProperty(x11.display, DefaultRootWindow(x11.display),
      x11atoms._NET_SUPPORTING_WM_CHECK, 0, ~0L, False, XA_WINDOW,
      &type, &fmt, &num, &bytes, &data) != Success || !data)
    goto out;

  Window * windowFromRoot = (Window *)data;

  if (XGetWindowProperty(x11.display, *windowFromRoot,
      x11atoms._NET_SUPPORTING_WM_CHECK, 0, ~0L, False, XA_WINDOW,
      &type, &fmt, &num, &bytes, &data) != Success || !data)
    goto out_root;

  Window * windowFromChild = (Window *)data;
  if (*windowFromChild != *windowFromRoot)
    goto out_child;

  if (XGetWindowProperty(x11.display, DefaultRootWindow(x11.display),
      x11atoms._NET_SUPPORTED, 0, ~0L, False, AnyPropertyType,
      &type, &fmt, &num, &bytes, &data) != Success || !data)
    goto out_child;

  Atom * supported = (Atom *)data;
  unsigned long supportedCount = num;

  if (XGetWindowProperty(x11.display, *windowFromRoot,
      x11atoms._NET_WM_NAME, 0, ~0L, False, AnyPropertyType,
      &type, &fmt, &num, &bytes, &data) != Success || !data)
    goto out_supported;

  char * wmName = (char *)data;

  for(unsigned long i = 0; i < supportedCount; ++i)
  {
    if (supported[i] == x11atoms._NET_WM_STATE_FOCUSED)
      x11.ewmhHasFocusEvent = true;
  }

  DEBUG_INFO("EWMH-complient window manager detected: %s", wmName);
  x11.ewmhSupport = true;


  XFree(wmName);
out_supported:
  XFree(supported);
out_child:
  XFree(windowFromChild);
out_root:
  XFree(windowFromRoot);
out:
  return;
}

static bool x11Init(const LG_DSInitParams params)
{
  XIDeviceInfo *devinfo;
  int count;
  int event, error;

  memset(&x11, 0, sizeof(x11));
  x11.xValuator = -1;
  x11.yValuator = -1;
  x11.display = XOpenDisplay(NULL);
  x11.jitRender = params.jitRender;

  XSetWindowAttributes swa =
  {
    .event_mask =
      StructureNotifyMask |
      PropertyChangeMask |
      ExposureMask
  };
  unsigned long swaMask = CWEventMask;

#ifdef ENABLE_OPENGL
  if (params.opengl)
  {
    GLint glXAttribs[] =
    {
      GLX_RGBA,
      GLX_DOUBLEBUFFER  ,
      GLX_DEPTH_SIZE    , 24,
      GLX_STENCIL_SIZE  , 0,
      GLX_RED_SIZE      , 8,
      GLX_GREEN_SIZE    , 8,
      GLX_BLUE_SIZE     , 8,
      GLX_DEPTH_SIZE    , 0,
      GLX_SAMPLE_BUFFERS, 0,
      GLX_SAMPLES       , 0,
      None
    };

    x11.visual = glXChooseVisual(x11.display,
        XDefaultScreen(x11.display), glXAttribs);

    if (!x11.visual)
    {
      DEBUG_ERROR("glXChooseVisual failed");
      goto fail_display;
    }

    swa.colormap = XCreateColormap(x11.display, XDefaultRootWindow(x11.display),
        x11.visual->visual, AllocNone);
    swaMask |= CWColormap;
  }
#endif

  x11.window = XCreateWindow(
      x11.display,
      XDefaultRootWindow(x11.display),
      params.x, params.y,
      params.w, params.h,
      0,
      x11.visual ? x11.visual->depth  : CopyFromParent,
      InputOutput,
      x11.visual ? x11.visual->visual : CopyFromParent,
      swaMask,
      &swa);

  if (!x11.window)
  {
    DEBUG_ERROR("XCreateWindow failed");
    goto fail_display;
  }

  XStoreName(x11.display, x11.window, params.title);

  XClassHint hint =
  {
    .res_name  = strdup(params.title),
    .res_class = strdup("looking-glass-client")
  };
  XSetClassHint(x11.display, x11.window, &hint);
  free(hint.res_name);
  free(hint.res_class);

  XSizeHints *xsh = XAllocSizeHints();
  if (params.center)
  {
    xsh->flags      |= PWinGravity;
    xsh->win_gravity = 5; //Center
  }

  if (!params.resizable)
  {
    xsh->flags      |= PMinSize | PMaxSize;
    xsh->min_width   = params.w;
    xsh->max_width   = params.w;
    xsh->min_height  = params.h;
    xsh->max_height  = params.h;
  }

  XSetWMNormalHints(x11.display, x11.window, xsh);
  XFree(xsh);

  X11AtomsInit();
  XSetWMProtocols(x11.display, x11.window, &x11atoms.WM_DELETE_WINDOW, 1);

  // check for Extended Window Manager Hints support
  x11CheckEWMHSupport();

  if (params.borderless)
  {
    if (x11atoms._MOTIF_WM_HINTS)
    {
      const struct MwmHints hints =
      {
        .flags       = MWM_HINTS_DECORATIONS,
        .decorations = 0
      };

      XChangeProperty(
        x11.display,
        x11.window,
        x11atoms._MOTIF_WM_HINTS,
        x11atoms._MOTIF_WM_HINTS,
        32,
        PropModeReplace,
        (unsigned char *)&hints,
        5
      );
    }
    else
    {
      // fallback to making a utility window, not ideal but better then nothing
      XChangeProperty(
        x11.display,
        x11.window,
        x11atoms._NET_WM_WINDOW_TYPE,
        XA_ATOM,
        32,
        PropModeReplace,
        (unsigned char *)&x11atoms._NET_WM_WINDOW_TYPE_UTILITY,
        1
      );
    }
  }
  else
  {
    XChangeProperty(
      x11.display,
      x11.window,
      x11atoms._NET_WM_WINDOW_TYPE,
      XA_ATOM,
      32,
      PropModeReplace,
      (unsigned char *)&x11atoms._NET_WM_WINDOW_TYPE_NORMAL,
      1
    );
  }

  if (params.maximize)
  {
    Atom wmState[2] =
    {
      x11atoms._NET_WM_STATE_MAXIMIZED_HORZ,
      x11atoms._NET_WM_STATE_MAXIMIZED_VERT
    };

    XChangeProperty(
      x11.display,
      x11.window,
      x11atoms._NET_WM_STATE,
      XA_ATOM,
      32,
      PropModeReplace,
      (unsigned char *)&wmState,
      2
    );
  }

  if (x11atoms._NET_REQUEST_FRAME_EXTENTS)
  {
    XEvent reqevent =
    {
      .xclient =
      {
        .type         = ClientMessage,
        .window       = x11.window,
        .format       = 32,
        .message_type = x11atoms._NET_REQUEST_FRAME_EXTENTS
      }
    };

    XSendEvent(x11.display, DefaultRootWindow(x11.display), False,
        SubstructureNotifyMask | SubstructureRedirectMask, &reqevent);
  }

  int major = 2;
  int minor = 0;
  if (XIQueryVersion(x11.display, &major, &minor) != Success)
  {
    DEBUG_ERROR("Failed to query the XInput version");
    return false;
  }

  DEBUG_INFO("X11 XInput %d.%d in use", major, minor);

  devinfo = XIQueryDevice(x11.display, XIAllDevices, &count);
  if (!devinfo)
  {
    DEBUG_ERROR("XIQueryDevice failed");
    goto fail_window;
  }

  Atom rel_x = XInternAtom(x11.display, "Rel X", True);
  Atom rel_y = XInternAtom(x11.display, "Rel Y", True);

  bool havePointer  = false;
  bool haveKeyboard = false;
  for(int i = 0; i < count; ++i)
  {
    /* look for the master pointing device */
    if (!havePointer && devinfo[i].use == XIMasterPointer)
    {
      for(int j = 0; j < devinfo[i].num_classes; ++j)
      {
        XIAnyClassInfo *cdevinfo =
          (XIAnyClassInfo *)(devinfo[i].classes[j]);
        if (cdevinfo->type == XIValuatorClass)
        {
          XIValuatorClassInfo *vdevinfo = (XIValuatorClassInfo *)cdevinfo;
          if (vdevinfo->label == rel_x || (!vdevinfo->label &&
              vdevinfo->number == 0 && vdevinfo->mode == XIModeRelative))
            x11.xValuator = vdevinfo->number;
          else if (vdevinfo->label == rel_y || (!vdevinfo->label &&
              vdevinfo->number == 1 && vdevinfo->mode == XIModeRelative))
            x11.yValuator = vdevinfo->number;
        }
      }

      if (x11.xValuator >= 0 && x11.yValuator >= 0)
      {
        havePointer = true;
        x11.pointerDev = devinfo[i].deviceid;
      }
    }

    /* look for the master keyboard device */
    if (!haveKeyboard && devinfo[i].use == XIMasterKeyboard)
      for(int j = 0; j < devinfo[i].num_classes; ++j)
      {
        XIAnyClassInfo *cdevinfo =
          (XIAnyClassInfo *)(devinfo[i].classes[j]);
        if (cdevinfo->type == XIKeyClass)
        {
          haveKeyboard = true;
          x11.keyboardDev = devinfo[i].deviceid;
          break;
        }
      }
  }

  if (!havePointer)
  {
    DEBUG_ERROR("Failed to find the master pointing device");
    XIFreeDeviceInfo(devinfo);
    goto fail_window;
  }

  if (!haveKeyboard)
  {
    DEBUG_ERROR("Failed to find the master keyboard device");
    XIFreeDeviceInfo(devinfo);
    goto fail_window;
  }

  XDisplayKeycodes(x11.display, &x11.minKeycode, &x11.maxKeycode);
  x11.keysyms = XGetKeyboardMapping(x11.display, x11.minKeycode,
      x11.maxKeycode - x11.minKeycode, &x11.symsPerKeycode);

  XIFreeDeviceInfo(devinfo);

  XQueryExtension(x11.display, "XInputExtension", &x11.xinputOp, &event, &error);
  XIEventMask eventmask;
  unsigned char mask[XIMaskLen(XI_LASTEVENT)] = { 0 };

  eventmask.deviceid = XIAllMasterDevices;
  eventmask.mask_len = sizeof(mask);
  eventmask.mask     = mask;

  XISetMask(mask, XI_FocusIn );
  XISetMask(mask, XI_FocusOut);

  XISetMask(mask, XI_Enter        );
  XISetMask(mask, XI_Leave        );
  XISetMask(mask, XI_Motion       );
  XISetMask(mask, XI_KeyPress     );
  XISetMask(mask, XI_KeyRelease   );
  XISetMask(mask, XI_ButtonPress  );
  XISetMask(mask, XI_ButtonRelease);

  if (XISelectEvents(x11.display, x11.window, &eventmask, 1) != Success)
  {
    XFree(mask);
    DEBUG_ERROR("Failed to select the xinput events");
    goto fail_window;
  }

  unsigned long value = 1;
  XChangeProperty(
    x11.display,
    x11.window,
    x11atoms._NET_WM_BYPASS_COMPOSITOR,
    XA_CARDINAL,
    32,
    PropModeReplace,
    (unsigned char *)&value,
    1
  );

  XChangeProperty(
    x11.display,
    x11.window,
    x11atoms._NET_WM_ICON,
    XA_CARDINAL,
    32,
    PropModeReplace,
    (unsigned char *)icondata,
    sizeof(icondata) / sizeof(icondata[0])
  );

  /* create the blank cursor */
  {
    static char data[] = { 0x00 };
    XColor dummy;
    Pixmap temp = XCreateBitmapFromData(x11.display, x11.window, data, 1, 1);
    x11.cursors[LG_POINTER_NONE] = XCreatePixmapCursor(x11.display, temp, temp,
        &dummy, &dummy, 0, 0);
    XFreePixmap(x11.display, temp);
  }

  /* create the square cursor */
  {
    static char data[] = { 0x07, 0x05, 0x07 };
    static char mask[] = { 0xff, 0xff, 0xff };

    Colormap cmap = DefaultColormap(x11.display, DefaultScreen(x11.display));
    XColor colors[2] =
    {
      { .pixel = BlackPixelOfScreen(DefaultScreenOfDisplay(x11.display)) },
      { .pixel = WhitePixelOfScreen(DefaultScreenOfDisplay(x11.display)) }
    };

    XQueryColors(x11.display, cmap, colors, 2);

    Pixmap img = XCreateBitmapFromData(x11.display, x11.window, data, 3, 3);
    Pixmap msk = XCreateBitmapFromData(x11.display, x11.window, mask, 3, 3);

    x11.cursors[LG_POINTER_SQUARE] = XCreatePixmapCursor(x11.display, img, msk,
        &colors[0], &colors[1], 1, 1);

    XFreePixmap(x11.display, img);
    XFreePixmap(x11.display, msk);
  }

  /* initialize the rest of the cursors */
  const char * cursorLookup[LG_POINTER_COUNT] = {
    NULL               , // LG_POINTER_NONE
    NULL               , // LG_POINTER_SQUARE
    "left_ptr"         , // LG_POINTER_ARROW
    "text"             , // LG_POINTER_INPUT
    "move"             , // LG_POINTER_MOVE
    "ns-resize"        , // LG_POINTER_RESIZE_NS
    "ew-resize"        , // LG_POINTER_RESIZE_EW
    "nesw-resize"      , // LG_POINTER_RESIZE_NESW
    "nwse-resize"      , // LG_POINTER_RESIZE_NWSE
    "hand"             , // LG_POINTER_HAND
    "not-allowed"      , // LG_POINTER_NOT_ALLOWED
  };

  const char * fallbackLookup[LG_POINTER_COUNT] = {
    NULL               , // LG_POINTER_NONE
    NULL               , // LG_POINTER_SQUARE
    "left_ptr"         , // LG_POINTER_ARROW
    "xterm"            , // LG_POINTER_INPUT
    "fluer"            , // LG_POINTER_MOVE
    "sb_v_double_arrow", // LG_POINTER_RESIZE_NS
    "sb_h_double_arrow", // LG_POINTER_RESIZE_EW
    "sizing"           , // LG_POINTER_RESIZE_NESW
    "sizing"           , // LG_POINTER_RESIZE_NWSE
    "hand2"            , // LG_POINTER_HAND
    "X_cursor"         , // LG_POINTER_NOT_ALLOWED
  };

  for(int i = 0; i < LG_POINTER_COUNT; ++i)
  {
    if (!cursorLookup[i])
      continue;
    x11.cursors[i] = XcursorLibraryLoadCursor(x11.display, cursorLookup[i]);
    if (!x11.cursors[i])
      x11.cursors[i] = XcursorLibraryLoadCursor(x11.display, fallbackLookup[i]);
  }

  /* default to the square cursor */
  XDefineCursor(x11.display, x11.window, x11.cursors[LG_POINTER_SQUARE]);

  if (x11.jitRender)
  {
    x11.frameEvent = lgCreateEvent(true, 0);
    XPresentQueryExtension(x11.display, &x11.xpresentOp, &event, &error);
    XPresentSelectInput(x11.display, x11.window, PresentCompleteNotifyMask);
    x11.presentPixmap = XCreatePixmap(x11.display, x11.window, 1, 1, 24);
    x11.presentRegion = XFixesCreateRegion(x11.display, &(XRectangle){0}, 1);
  }

  XMapWindow(x11.display, x11.window);
  XFlush(x11.display);

  if (!params.center)
    XMoveWindow(x11.display, x11.window, params.x, params.y);

  if (params.fullscreen)
    x11SetFullscreen(true);

  XSetLocaleModifiers(""); // Load XMODIFIERS
  x11.xim = XOpenIM(x11.display, 0, 0, 0);

  if (!x11.xim)
  {
    // disable IME
    XSetLocaleModifiers("@im=none");
    x11.xim = XOpenIM(x11.display, 0, 0, 0);
  }

  if (x11.xim)
  {
    x11.xic = XCreateIC(
      x11.xim,
      XNInputStyle,   XIMPreeditNothing | XIMStatusNothing,
      XNClientWindow, x11.window,
      XNFocusWindow,  x11.window,
      NULL
    );
  }
  else
    DEBUG_WARN("Failed to initialize X Input Method");

  if (x11.xic)
  {
    XSetICFocus(x11.xic);
    XSelectInput(x11.display, x11.window, StructureNotifyMask | ExposureMask |
        PropertyChangeMask | KeyPressMask);
  }
  else
    DEBUG_WARN("Failed to initialize X Input Context, typing will not work");

  if (!lgCreateThread("X11EventThread", x11EventThread, NULL, &x11.eventThread))
  {
    DEBUG_ERROR("Failed to create the x11 event thread");
    goto fail_window;
  }

  if (x11.jitRender)
    x11DoPresent(0);

  return true;

fail_window:
  XDestroyWindow(x11.display, x11.window);

fail_display:
  XCloseDisplay(x11.display);

  return false;
}

static void x11Startup(void)
{
}

static void x11Shutdown(void)
{
  if (x11.jitRender)
    lgSignalEvent(x11.frameEvent);
}

static void x11Free(void)
{
  lgJoinThread(x11.eventThread, NULL);

  if (x11.jitRender)
  {
    lgFreeEvent(x11.frameEvent);
    XFreePixmap(x11.display, x11.presentPixmap);
    XFixesDestroyRegion(x11.display, x11.presentRegion);
  }

  if (x11.window)
    XDestroyWindow(x11.display, x11.window);

  for(int i = 0; i < LG_POINTER_COUNT; ++i)
    if (x11.cursors[i])
      XFreeCursor(x11.display, x11.cursors[i]);

  if (x11.keysyms)
    XFree(x11.keysyms);

  XCloseDisplay(x11.display);
}

static bool x11GetProp(LG_DSProperty prop, void *ret)
{
  switch (prop)
  {
    case LG_DS_WARP_SUPPORT:
      *(enum LG_DSWarpSupport*)ret = LG_DS_WARP_SCREEN;
      return true;

    case LG_DS_MAX_MULTISAMPLE:
    {
      Display * dpy = XOpenDisplay(NULL);
      if (!dpy)
        return false;

      XVisualInfo queryTemplate;
      queryTemplate.screen = 0;

      int visualCount;
      int maxSamples = -1;
      XVisualInfo * visuals = XGetVisualInfo(dpy, VisualScreenMask,
          &queryTemplate, &visualCount);

      for (int i = 0; i < visualCount; i++)
      {
        XVisualInfo * visual = &visuals[i];

        int res, supportsGL;
        // Some GLX visuals do not use GL, and these must be ignored in our search.
        if ((res = glXGetConfig(dpy, visual, GLX_USE_GL, &supportsGL)) != 0 ||
            !supportsGL)
          continue;

        int sampleBuffers, samples;
        if ((res = glXGetConfig(dpy, visual, GLX_SAMPLE_BUFFERS, &sampleBuffers)) != 0)
          continue;

        // Will be 1 if this visual supports multisampling
        if (sampleBuffers != 1)
          continue;

        if ((res = glXGetConfig(dpy, visual, GLX_SAMPLES, &samples)) != 0)
          continue;

        // Track the largest number of samples supported
        if (samples > maxSamples)
          maxSamples = samples;
      }

      XFree(visuals);
      XCloseDisplay(dpy);

      *(int*)ret = maxSamples;
      return true;
    }

    default:
      return true;
  }
}

static int x11EventThread(void * unused)
{
  int epollfd = epoll_create1(0);
  if (epollfd == -1)
  {
    DEBUG_ERROR("epolld_create1 failure");
    return 0;
  }

  struct epoll_event ev = { .events = EPOLLIN };
  const int fd = ConnectionNumber(x11.display);
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == -1)
  {
    close(epollfd);
    DEBUG_ERROR("epoll_ctl failed");
    return 0;
  }

  while(app_isRunning())
  {
    const uint64_t lastWMEvent = atomic_load(&x11.lastWMEvent);
    if (x11.invalidateAll && microtime() - lastWMEvent > 100000UL)
    {
      x11.invalidateAll = false;
      app_invalidateWindow(true);
    }

    if (!XPending(x11.display))
    {
      struct epoll_event events[1];
      int nfds = epoll_wait(epollfd, events, 1, 100);
      if (nfds == -1)
      {
        if (errno == EINTR)
          continue;

        close(epollfd);
        DEBUG_ERROR("epoll_wait failure");
        return 0;
      }

      if (nfds == 0 || !XPending(x11.display))
        continue;
    }

    XEvent xe;
    XNextEvent(x11.display, &xe);

    // call the clipboard handling code
    if (x11CBEventThread(xe))
      continue;

    switch(xe.type)
    {
      case ClientMessage:
        if (xe.xclient.data.l[0] == x11atoms.WM_DELETE_WINDOW)
          app_handleCloseEvent();
        continue;

      case ConfigureNotify:
      {
        atomic_store(&x11.lastWMEvent, microtime());

        int x, y;

        /* the window may have been re-parented so we need to translate to
         * ensure we get the screen top left position of the window */
        Window child;
        XTranslateCoordinates(
            x11.display,
            x11.window,
            DefaultRootWindow(x11.display),
            0, 0,
            &x,
            &y,
            &child);

        x11.rect.x = x;
        x11.rect.y = y;
        x11.rect.w = xe.xconfigure.width;
        x11.rect.h = xe.xconfigure.height;

        app_updateWindowPos(x, y);

        if (x11.fullscreen)
        {
          struct Border border = {0};
          app_handleResizeEvent(x11.rect.w, x11.rect.h, 1, border);
        }
        else
          app_handleResizeEvent(x11.rect.w, x11.rect.h, 1, x11.border);
        break;
      }

      case Expose:
      {
        atomic_store(&x11.lastWMEvent, microtime());
        x11.invalidateAll = true;
        break;
      }

      case GenericEvent:
      {
        XGenericEventCookie *cookie = (XGenericEventCookie*)&xe.xcookie;
        if (cookie->extension == x11.xinputOp)
        {
          XGetEventData(x11.display, cookie);
          x11XInputEvent(cookie);
          XFreeEventData(x11.display, cookie);
        }
        else if (cookie->extension == x11.xpresentOp)
        {
          XGetEventData(x11.display, cookie);
          x11XPresentEvent(cookie);
          XFreeEventData(x11.display, cookie);
        }
        break;
      }

      case PropertyNotify:

        // ignore property events that are not for us
        if (xe.xproperty.display != x11.display      ||
            xe.xproperty.window  != x11.window       ||
            xe.xproperty.state   != PropertyNewValue)
          continue;

        if (xe.xproperty.atom == x11atoms._NET_WM_STATE)
        {
          atomic_store(&x11.lastWMEvent, microtime());

          Atom type;
          int fmt;
          unsigned long num, bytes;
          unsigned char *data;

          if (XGetWindowProperty(x11.display, x11.window,
              x11atoms._NET_WM_STATE, 0, ~0L, False, AnyPropertyType,
              &type, &fmt, &num, &bytes, &data) != Success)
            break;

          bool fullscreen = false;
          bool focused    = false;
          for(unsigned long i = 0; i < num; ++i)
          {
            Atom prop = ((Atom *)data)[i];
            if (prop == x11atoms._NET_WM_STATE_FULLSCREEN)
              fullscreen = true;
            else if (prop == x11atoms._NET_WM_STATE_FOCUSED)
              focused = true;
          }

          if (x11.ewmhHasFocusEvent && x11.focused != focused)
          {
            x11.focused = focused;
            app_handleFocusEvent(focused);
          }

          x11.fullscreen = fullscreen;
          XFree(data);
          break;
        }

        if (xe.xproperty.atom == x11atoms._NET_FRAME_EXTENTS)
        {
          atomic_store(&x11.lastWMEvent, microtime());

          Atom type;
          int fmt;
          unsigned long num, bytes;
          unsigned char *data;

          if (XGetWindowProperty(x11.display, x11.window,
                x11atoms._NET_FRAME_EXTENTS, 0, 4, False, AnyPropertyType,
                &type, &fmt, &num, &bytes, &data) != Success)
              break;

          if (num >= 4)
          {
            long *cardinal = (long *)data;
            x11.border.left   = cardinal[0];
            x11.border.right  = cardinal[1];
            x11.border.top    = cardinal[2];
            x11.border.bottom = cardinal[3];
            app_handleResizeEvent(x11.rect.w, x11.rect.h, 1, x11.border);
          }

          XFree(data);
          break;
        }
        break;
    }
  }

  close(epollfd);
  return 0;
}

static enum Modifiers keySymToModifier(KeySym sym)
{
  switch (sym)
  {
    case XK_Control_L: return MOD_CTRL_LEFT;
    case XK_Control_R: return MOD_CTRL_RIGHT;
    case XK_Shift_L:   return MOD_SHIFT_LEFT;
    case XK_Shift_R:   return MOD_SHIFT_RIGHT;
    case XK_Alt_L:     return MOD_ALT_LEFT;
    case XK_Alt_R:     return MOD_ALT_RIGHT;
    case XK_Super_L:   return MOD_SUPER_LEFT;
    case XK_Super_R:   return MOD_SUPER_RIGHT;
    default:           return -1;
  }
}

static void updateModifiers(void)
{
  app_handleKeyboardModifiers(
    x11.modifiers[MOD_CTRL_LEFT] || x11.modifiers[MOD_CTRL_RIGHT],
    x11.modifiers[MOD_SHIFT_LEFT] || x11.modifiers[MOD_SHIFT_RIGHT],
    x11.modifiers[MOD_ALT_LEFT] || x11.modifiers[MOD_ALT_RIGHT],
    x11.modifiers[MOD_SUPER_LEFT] || x11.modifiers[MOD_SUPER_RIGHT]
  );
}

static void setFocus(bool focused, double x, double y)
{
  if (x11.focused == focused)
    return;

  x11.focused = focused;
  app_updateCursorPos(x, y);
  app_handleFocusEvent(focused);
}

static int getCharcode(int detail)
{
  if (detail < x11.minKeycode || detail > x11.maxKeycode)
    return 0;

  KeySym sym = x11.keysyms[(detail - x11.minKeycode) *
      x11.symsPerKeycode];
  sym = xkb_keysym_to_upper(sym);
  return xkb_keysym_to_utf32(sym);
}

static void x11XInputEvent(XGenericEventCookie *cookie)
{
  static int button_state = 0;

  switch(cookie->evtype)
  {
    case XI_FocusIn:
    {
      XIFocusOutEvent *xie = cookie->data;
      if (x11.ewmhHasFocusEvent)
      {
        // if meta ungrab for move/resize
        if (xie->mode == XINotifyUngrab)
          setFocus(true, xie->event_x, xie->event_y);
        return;
      }

      atomic_store(&x11.lastWMEvent, microtime());
      if (x11.focused)
        return;

      if (xie->mode != XINotifyNormal &&
          xie->mode != XINotifyWhileGrabbed &&
          xie->mode != XINotifyUngrab)
        return;


      setFocus(true, xie->event_x, xie->event_y);
      return;
    }

    case XI_FocusOut:
    {
      XIFocusOutEvent *xie = cookie->data;
      if (x11.ewmhHasFocusEvent)
      {
        // if meta grab for move/resize
        if (xie->mode == XINotifyGrab)
          setFocus(false, xie->event_x, xie->event_y);
        return;
      }

      atomic_store(&x11.lastWMEvent, microtime());
      if (!x11.focused)
        return;

      if (xie->mode != XINotifyNormal &&
          xie->mode != XINotifyWhileGrabbed &&
          xie->mode != XINotifyGrab)
        return;

      setFocus(false, xie->event_x, xie->event_y);
      return;
    }

    case XI_Enter:
    {
      atomic_store(&x11.lastWMEvent, microtime());
      XIEnterEvent *xie = cookie->data;
      if (x11.entered || xie->event != x11.window ||
          xie->mode != XINotifyNormal)
        return;

      app_updateCursorPos(xie->event_x, xie->event_y);
      app_handleEnterEvent(true);
      x11.entered = true;
      return;
    }

    case XI_Leave:
    {
      atomic_store(&x11.lastWMEvent, microtime());
      XILeaveEvent *xie = cookie->data;

      if (!x11.entered || xie->event != x11.window ||
          button_state != 0 || app_isCaptureMode() ||
          xie->mode == NotifyGrab)
        return;

      app_updateCursorPos(xie->event_x, xie->event_y);
      app_handleEnterEvent(false);
      x11.entered = false;
      return;
    }

    case XI_KeyPress:
    {
      if (!x11.focused || x11.keyboardGrabbed)
        return;

      XIDeviceEvent *device = cookie->data;
      app_handleKeyPress(device->detail - x11.minKeycode,
          getCharcode(device->detail));

      if (!x11.xic || !app_isOverlayMode())
        return;

      char buffer[128];
      KeySym sym;
      Status status;
      int count;
      XKeyPressedEvent ev = {
        .display = x11.display,
        .window  = x11.window,
        .type    = KeyPress,
        .keycode = device->detail,
        .state   = device->mods.effective,
      };

      count = Xutf8LookupString(x11.xic, &ev, buffer, sizeof(buffer),
        &sym, &status);

      if (status == XBufferOverflow || count >= sizeof(buffer))
      {
        DEBUG_WARN("Typing too many characters at once, ignoring");
        return;
      }

      if (status == XLookupChars || status == XLookupBoth)
      {
        buffer[count] = '\0';
        app_handleKeyboardTyped(buffer);
      }

      if (status == XLookupKeySym || status == XLookupBoth)
      {
        int modifier = keySymToModifier(sym);
        if (modifier >= 0)
        {
          x11.modifiers[modifier] = true;
          updateModifiers();
        }
      }
      return;
    }

    case XI_KeyRelease:
    {
      if (!x11.focused || x11.keyboardGrabbed)
        return;

      XIDeviceEvent *device = cookie->data;
      app_handleKeyRelease(device->detail - x11.minKeycode,
          getCharcode(device->detail));

      if (!x11.xic || !app_isOverlayMode())
        return;

      XKeyPressedEvent ev = {
        .display = x11.display,
        .window  = x11.window,
        .type    = KeyRelease,
        .keycode = device->detail,
        .state   = device->mods.effective,
      };
      KeySym sym = XLookupKeysym(&ev, 0);
      int modifier = keySymToModifier(sym);

      if (modifier >= 0)
      {
        x11.modifiers[modifier] = false;
        updateModifiers();
      }
      return;
    }

    case XI_RawKeyPress:
    {
      if (!x11.focused)
        return;

      XIRawEvent *raw = cookie->data;
      app_handleKeyPress(raw->detail - x11.minKeycode,
          getCharcode(raw->detail));
      return;
    }

    case XI_RawKeyRelease:
    {
      if (!x11.focused)
        return;

      XIRawEvent *raw = cookie->data;
      app_handleKeyRelease(raw->detail - x11.minKeycode,
          getCharcode(raw->detail));
      return;
    }

    case XI_ButtonPress:
    {
      if (!x11.focused || !x11.entered)
        return;

      XIDeviceEvent *device = cookie->data;
      if (device->detail == 4)
        app_handleWheelMotion(-0.5);
      else if (device->detail == 5)
        app_handleWheelMotion(0.5);
      else
        app_handleButtonPress(
            device->detail > 5 ? device->detail - 2 : device->detail);

      return;
    }

    case XI_ButtonRelease:
    {
      if (!x11.focused || !x11.entered)
        return;

      XIDeviceEvent *device = cookie->data;
      app_handleButtonRelease(device->detail);
      return;
    }

    case XI_RawButtonPress:
    {
      if (!x11.focused || !x11.entered)
        return;

      XIRawEvent *raw = cookie->data;

      /* filter out duplicate events */
      static Time         prev_time   = 0;
      static unsigned int prev_detail = 0;
      if (raw->time == prev_time && raw->detail == prev_detail)
        return;

      prev_time     = raw->time;
      prev_detail   = raw->detail;
      button_state |= (1 << raw->detail);

      app_handleButtonPress(
          raw->detail > 5 ? raw->detail - 2 : raw->detail);
      return;
    }

    case XI_RawButtonRelease:
    {
      if (!x11.focused || !x11.entered)
        return;

      XIRawEvent *raw = cookie->data;

      /* filter out duplicate events */
      static Time         prev_time   = 0;
      static unsigned int prev_detail = 0;
      if (raw->time == prev_time && raw->detail == prev_detail)
        return;

      prev_time     = raw->time;
      prev_detail   = raw->detail;
      button_state &= ~(1 << raw->detail);

      app_handleButtonRelease(
          raw->detail > 5 ? raw->detail - 2 : raw->detail);
      return;
    }

    case XI_Motion:
    {
      XIDeviceEvent *device = cookie->data;
      app_updateCursorPos(device->event_x, device->event_y);

      if (!x11.pointerGrabbed)
        app_handleMouseRelative(0.0, 0.0, 0.0, 0.0);
      return;
    }

    case XI_RawMotion:
    {
      if (!x11.focused || !x11.entered)
        return;

      XIRawEvent *raw = cookie->data;
      double raw_axis[2] = { 0 };
      double axis[2] = { 0 };

      /* select the active validators for the X & Y axis */
      double *valuator = raw->valuators.values;
      double *r_value  = raw->raw_values;
      bool   has_axes  = false;
      for(int i = 0; i < raw->valuators.mask_len * 8; ++i)
      {
        if (XIMaskIsSet(raw->valuators.mask, i))
        {
          if (i == x11.xValuator)
          {
            raw_axis[0] = *r_value;
            axis    [0] = *valuator;
            has_axes = true;
          }
          else if (i == x11.yValuator)
          {
            raw_axis[1] = *r_value;
            axis    [1] = *valuator;
            has_axes = true;
          }

          ++valuator;
          ++r_value;
        }
      }

      /* filter out events with no axis data */
      if (!has_axes)
        return;

      /* filter out duplicate events */
      static Time   prev_time    = 0;
      static double prev_axis[2] = {0};
      if (raw->time == prev_time &&
          axis[0] == prev_axis[0] &&
          axis[1] == prev_axis[1])
        return;

      prev_time = raw->time;
      prev_axis[0] = axis[0];
      prev_axis[1] = axis[1];

      app_handleMouseRelative(axis[0], axis[1], raw_axis[0], raw_axis[1]);
      return;
    }
  }
}

#include <math.h>

static void x11XPresentEvent(XGenericEventCookie *cookie)
{
  switch(cookie->evtype)
  {
    case PresentCompleteNotify:
    {
      XPresentCompleteNotifyEvent * e = cookie->data;
      x11DoPresent(e->msc);
      atomic_store(&x11.presentMsc, e->msc);
      atomic_store(&x11.presentUst, e->ust);
      lgSignalEvent(x11.frameEvent);
      break;
    }
  }
}

#ifdef ENABLE_EGL
static EGLDisplay x11GetEGLDisplay(void)
{
  EGLDisplay ret;
  const char *early_exts = eglQueryString(NULL, EGL_EXTENSIONS);

  if (util_hasGLExt(early_exts, "EGL_KHR_platform_base") &&
      g_egl_dynProcs.eglGetPlatformDisplay)
  {
    DEBUG_INFO("Using eglGetPlatformDisplay");
    ret = g_egl_dynProcs.eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR,
        x11.display, NULL);
  }
  else if (util_hasGLExt(early_exts, "EGL_EXT_platform_base") &&
      g_egl_dynProcs.eglGetPlatformDisplayEXT)
  {
    DEBUG_INFO("Using eglGetPlatformDisplayEXT");
    ret = g_egl_dynProcs.eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_KHR,
        x11.display, NULL);
  }
  else
  {
    DEBUG_INFO("Using eglGetDisplay");
    ret = eglGetDisplay(x11.display);
  }

  return ret;
}

static EGLNativeWindowType x11GetEGLNativeWindow(void)
{
  return (EGLNativeWindowType)x11.window;
}

static void x11EGLSwapBuffers(EGLDisplay display, EGLSurface surface,
    const struct Rect * damage, int count)
{
  static struct SwapWithDamageData data = {0};
  if (!data.init)
    swapWithDamageInit(&data, display);

  swapWithDamage(&data, display, surface, damage, count);
}
#endif

#ifdef ENABLE_OPENGL
static LG_DSGLContext x11GLCreateContext(void)
{
  return (LG_DSGLContext)
    glXCreateContext(x11.display, x11.visual, NULL, GL_TRUE);
}

static void x11GLDeleteContext(LG_DSGLContext context)
{
  glXDestroyContext(x11.display, (GLXContext)context);
}

static void x11GLMakeCurrent(LG_DSGLContext context)
{
  glXMakeCurrent(x11.display, x11.window, (GLXContext)context);
}

static void x11GLSetSwapInterval(int interval)
{
  static PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT = NULL;
  if (!glXSwapIntervalEXT)
  {
    glXSwapIntervalEXT = (PFNGLXSWAPINTERVALEXTPROC) glXGetProcAddressARB(
        (const GLubyte *) "glXSwapIntervalEXT");
    if (!glXSwapIntervalEXT)
      DEBUG_FATAL("Failed to load glXSwapIntervalEXT");
  }
  glXSwapIntervalEXT(x11.display, x11.window, interval);
}

static void x11GLSwapBuffers(void)
{
  glXSwapBuffers(x11.display, x11.window);
}
#endif

static bool x11WaitFrame(void)
{
  /* wait until we are woken up by the present event */
  lgWaitEvent(x11.frameEvent, TIMEOUT_INFINITE);

#define WARMUP_TIME 3000000 //2s
#define CALIBRATION_COUNT 400
  const uint64_t ust = atomic_load(&x11.presentUst);
  const uint64_t msc = atomic_load(&x11.presentMsc);

  static bool warmup = true;
  if (warmup)
  {
    static uint64_t expire = 0;
    if (!expire)
    {
      DEBUG_INFO("Warming up...");
      expire = ust + WARMUP_TIME;
    }

    if (ust < expire)
      return false;

    warmup = false;
    DEBUG_INFO("Warmup done, doing calibration...");
  }

  /* calibrate a delay to push our presentation time forward as far as
   * possible without skipping frames */
  static int      calibrate = 0;
  static uint64_t lastts    = 0;
  static uint64_t lastmsc   = 0;
  static uint64_t delay     = 0;

  uint64_t deltamsc = 0;
  if (lastts)
  {
    uint64_t deltats = ust - lastts;
    deltamsc = msc - lastmsc;

    if (calibrate == 0)
    {
      if (!delay)
        delay = deltats / 2;
      else
      {
        /* increase the delay until we see a skip */
        if (deltamsc < 2)
          delay += 100;
        else
        {
          delay -= 100;
          ++calibrate;
          deltamsc = 0;
        }
      }
    }
    else
    {
      if (calibrate < CALIBRATION_COUNT)
      {
        /* every skip we back off the delay */
        if (deltamsc > 1)
        {
          /* prevent underflow */
          if (delay - 100 < delay)
          {
            delay -= 100;
            calibrate = 1;
            deltamsc = 0;
          }
          else
          {
            /* if underflow, we are simply too slow, no delay */
            delay = 0;
            calibrate = CALIBRATION_COUNT;
          }
        }

        /* if we have finished, print out the delay */
        if (++calibrate == CALIBRATION_COUNT)
        {
          /* delays shorter then 1ms are unmaintainable */
          if (delay < 1000)
          {
            delay = 0;
            DEBUG_INFO("Calibration done, no delay required");
          }
          else
            DEBUG_INFO("Calibration done, delay = %lu us", delay);
        }
      }
    }
  }

  lastts  = ust;
  lastmsc = msc;

  /* adjustments if we are still seeing odd skips */
  const uint64_t lastWMEvent = atomic_load(&x11.lastWMEvent);
  const uint64_t eventDelta = ust > lastWMEvent ? ust - lastWMEvent : 0;

  static int      skipCount = 0;
  static uint64_t lastSkipTime = 0;

  if (skipCount > 0 && ust - lastSkipTime > 1000000UL)
    skipCount = 0;

  if (delay > 0 && deltamsc > 1 && eventDelta > 1000000UL)
  {
    /* only adjust if the last skip was less then 1s ago */
    const bool flag = ust - lastSkipTime < 1000000UL;
    lastSkipTime = ust;

    if (flag && ++skipCount > 10)
    {
      if (delay - 500 < delay)
      {
        delay -= 500;
        DEBUG_INFO("Excessing skipping detected, reduced calibration delay to %lu us", delay);
        skipCount = 0;
      }
      else
      {
        delay = 0;
        DEBUG_WARN("Excessive skipping detected, calibration delay disabled");
      }
    }
  }

  if (delay)
  {
    struct timespec ts = { .tv_nsec = delay * 1000 };
    while(nanosleep(&ts, &ts)) {};
  }

  /* force rendering until we have finished calibration so we can take into
   * account how long it takes for the scene to render */
  if (calibrate < CALIBRATION_COUNT)
    return true;

  return false;
}

static void x11StopWaitFrame(void)
{
  lgSignalEvent(x11.frameEvent);
}

static void x11GuestPointerUpdated(double x, double y, double localX, double localY)
{
  if (app_isCaptureMode() || !x11.entered)
    return;

  // avoid running too often
  static uint64_t last_warp = 0;
  uint64_t now = microtime();
  if (now - last_warp < 10000)
    return;
  last_warp = now;

  XIWarpPointer(
      x11.display,
      x11.pointerDev,
      None,
      x11.window,
      0, 0, 0, 0,
      localX, localY);

  XSync(x11.display, False);
}

static void x11SetPointer(LG_DSPointer pointer)
{
  XDefineCursor(x11.display, x11.window, x11.cursors[pointer]);
}

static void x11PrintGrabError(const char * type, int dev, Status ret)
{
  const char * errStr;
  switch(ret)
  {
    case AlreadyGrabbed : errStr = "AlreadyGrabbed" ; break;
    case GrabNotViewable: errStr = "GrabNotViewable"; break;
    case GrabFrozen     : errStr = "GrabFrozen"     ; break;
    case GrabInvalidTime: errStr = "GrabInvalidTime"; break;
    default:
      errStr = "Unknown";
      break;
  }

  DEBUG_ERROR("XIGrabDevice failed for %s dev %d with 0x%x (%s)",
      type, dev, ret, errStr);

}

static void x11GrabPointer(void)
{
  if (x11.pointerGrabbed)
    return;

  unsigned char mask_bits[XIMaskLen(XI_LASTEVENT)] = { 0 };
  XIEventMask mask = {
    x11.pointerDev,
    sizeof(mask_bits),
    mask_bits
  };

  XISetMask(mask.mask, XI_RawButtonPress  );
  XISetMask(mask.mask, XI_RawButtonRelease);
  XISetMask(mask.mask, XI_RawMotion       );
  XISetMask(mask.mask, XI_Motion          );
  XISetMask(mask.mask, XI_Enter           );
  XISetMask(mask.mask, XI_Leave           );

  Status ret;
  for(int retry = 0; retry < 10; ++retry)
  {
    ret = XIGrabDevice(
      x11.display,
      x11.pointerDev,
      x11.window,
      CurrentTime,
      None,
      XIGrabModeAsync,
      XIGrabModeAsync,
      XINoOwnerEvents,
      &mask);

    // on some WMs (i3) for an unknown reason the first grab attempt when
    // switching to a desktop that has LG on it fails with GrabFrozen, however
    // adding as short delay seems to resolve the issue.
    if (ret == GrabFrozen && retry < 9)
    {
      usleep(100000);
      continue;
    }

    break;
  }

  if (ret != Success)
  {
    x11PrintGrabError("pointer", x11.pointerDev, ret);
    return;
  }

  x11.pointerGrabbed = true;
}

static void x11UngrabPointer(void)
{
  if (!x11.pointerGrabbed)
    return;

  XIUngrabDevice(x11.display, x11.pointerDev, CurrentTime);
  XSync(x11.display, False);

  x11.pointerGrabbed = false;
}

static void x11CapturePointer(void)
{
  x11GrabPointer();
}

static void x11UncapturePointer(void)
{
  /* we need to ungrab the pointer on the following conditions when exiting capture mode:
   *   - if the format is invalid as we do not know where the guest cursor is,
   *     which breaks edge detection as the cursor can not be warped out of the
   *     window when we release it.
   *   - if the user has opted to use captureInputOnly mode.
   */
  if (!app_isFormatValid() || app_isCaptureOnlyMode())
    x11UngrabPointer();
}

static void x11GrabKeyboard(void)
{
  if (x11.keyboardGrabbed)
    return;

  unsigned char mask_bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
  XIEventMask mask = {
    x11.keyboardDev,
    sizeof(mask_bits),
    mask_bits
  };

  XISetMask(mask.mask, XI_RawKeyPress  );
  XISetMask(mask.mask, XI_RawKeyRelease);

  Status ret = XIGrabDevice(
      x11.display,
      x11.keyboardDev,
      x11.window,
      CurrentTime,
      None,
      XIGrabModeAsync,
      XIGrabModeAsync,
      XINoOwnerEvents,
      &mask);

  if (ret != Success)
  {
    x11PrintGrabError("keyboard", x11.keyboardDev, ret);
    return;
  }

  x11.keyboardGrabbed = true;
}

static void x11UngrabKeyboard(void)
{
  if (!x11.keyboardGrabbed)
    return;

  XIUngrabDevice(x11.display, x11.keyboardDev, CurrentTime);
  XSync(x11.display, False);

  x11.keyboardGrabbed = false;
}

static void x11WarpPointer(int x, int y, bool exiting)
{
  XIWarpPointer(
      x11.display,
      x11.pointerDev,
      None,
      x11.window,
      0, 0, 0, 0,
      x, y);

  XSync(x11.display, False);
}

static void x11RealignPointer(void)
{
  app_handleMouseRelative(0.0, 0.0, 0.0, 0.0);
}

static bool x11IsValidPointerPos(int x, int y)
{
  int screens;
  XineramaScreenInfo *xinerama = XineramaQueryScreens(x11.display, &screens);

  if(!xinerama)
    return true;

  bool ret = false;
  for(int i = 0; i < screens; ++i)
    if (x >= xinerama[i].x_org && x < xinerama[i].x_org + xinerama[i].width &&
        y >= xinerama[i].y_org && y < xinerama[i].y_org + xinerama[i].height)
    {
      ret = true;
      break;
    }

  XFree(xinerama);
  return ret;
}

static void x11RequestActivation(void)
{
  XEvent e =
  {
    .xclient = {
      .type         = ClientMessage,
      .send_event   = true,
      .message_type = x11atoms._NET_WM_STATE,
      .format       = 32,
      .window       = x11.window,
      .data.l       = {
        _NET_WM_STATE_ADD,
        x11atoms._NET_WM_STATE_DEMANDS_ATTENTION,
        0
      }
    }
  };

  XSendEvent(x11.display, DefaultRootWindow(x11.display), False,
      SubstructureNotifyMask | SubstructureRedirectMask, &e);
}

static void x11InhibitIdle(void)
{
  XScreenSaverSuspend(x11.display, true);
}

static void x11UninhibitIdle(void)
{
  XScreenSaverSuspend(x11.display, false);
}

static void x11Wait(unsigned int time)
{
  usleep(time * 1000U);
}

static void x11SetWindowSize(int w, int h)
{
  XResizeWindow(x11.display, x11.window, w, h);
}

static void x11SetFullscreen(bool fs)
{
  if (x11.fullscreen == fs)
    return;

  XEvent e =
  {
    .xclient = {
      .type         = ClientMessage,
      .send_event   = true,
      .message_type = x11atoms._NET_WM_STATE,
      .format       = 32,
      .window       = x11.window,
      .data.l       = {
        fs ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE,
        x11atoms._NET_WM_STATE_FULLSCREEN,
        0
      }
    }
  };

  XSendEvent(x11.display, DefaultRootWindow(x11.display), False,
      SubstructureNotifyMask | SubstructureRedirectMask, &e);
}

static bool x11GetFullscreen(void)
{
  return x11.fullscreen;
}

static void x11Minimize(void)
{
  XIconifyWindow(x11.display, x11.window, XDefaultScreen(x11.display));
}

struct LG_DisplayServerOps LGDS_X11 =
{
  .setup              = x11Setup,
  .probe              = x11Probe,
  .earlyInit          = x11EarlyInit,
  .init               = x11Init,
  .startup            = x11Startup,
  .shutdown           = x11Shutdown,
  .free               = x11Free,
  .getProp            = x11GetProp,
#ifdef ENABLE_EGL
  .getEGLDisplay      = x11GetEGLDisplay,
  .getEGLNativeWindow = x11GetEGLNativeWindow,
  .eglSwapBuffers     = x11EGLSwapBuffers,
#endif
#ifdef ENABLE_OPENGL
  .glCreateContext    = x11GLCreateContext,
  .glDeleteContext    = x11GLDeleteContext,
  .glMakeCurrent      = x11GLMakeCurrent,
  .glSetSwapInterval  = x11GLSetSwapInterval,
  .glSwapBuffers      = x11GLSwapBuffers,
#endif
  .waitFrame           = x11WaitFrame,
  .stopWaitFrame       = x11StopWaitFrame,
  .guestPointerUpdated = x11GuestPointerUpdated,
  .setPointer          = x11SetPointer,
  .grabPointer         = x11GrabPointer,
  .ungrabPointer       = x11UngrabPointer,
  .capturePointer      = x11CapturePointer,
  .uncapturePointer    = x11UncapturePointer,
  .grabKeyboard        = x11GrabKeyboard,
  .ungrabKeyboard      = x11UngrabKeyboard,
  .warpPointer         = x11WarpPointer,
  .realignPointer      = x11RealignPointer,
  .isValidPointerPos   = x11IsValidPointerPos,
  .requestActivation   = x11RequestActivation,
  .inhibitIdle         = x11InhibitIdle,
  .uninhibitIdle       = x11UninhibitIdle,
  .wait                = x11Wait,
  .setWindowSize       = x11SetWindowSize,
  .setFullscreen       = x11SetFullscreen,
  .getFullscreen       = x11GetFullscreen,
  .minimize            = x11Minimize,

  .cbInit    = x11CBInit,
  .cbNotice  = x11CBNotice,
  .cbRelease = x11CBRelease,
  .cbRequest = x11CBRequest
};
