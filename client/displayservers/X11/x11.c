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

#include "interface/displayserver.h"

#include "x11.h"
#include "atoms.h"
#include "clipboard.h"

#include <string.h>
#include <unistd.h>

#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/scrnsaver.h>
#include <X11/extensions/Xinerama.h>

#include <GL/glx.h>
#include <GL/glxext.h>

#ifdef ENABLE_EGL
#include <EGL/eglext.h>
#include "egl_dynprocs.h"
#endif

#include "app.h"
#include "common/debug.h"
#include "common/time.h"
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
static void x11GenericEvent(XGenericEventCookie *cookie);

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

static bool x11Init(const LG_DSInitParams params)
{
  XIDeviceInfo *devinfo;
  int count;
  int event, error;

  memset(&x11, 0, sizeof(x11));
  x11.xValuator = -1;
  x11.yValuator = -1;
  x11.display = XOpenDisplay(NULL);

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

  Atom wmState[3] = {0};
  int wmStateCount = 0;

  if (params.fullscreen)
  {
    x11.fullscreen = true;
    wmState[wmStateCount++] = x11atoms._NET_WM_STATE_FULLSCREEN;
  }

  if (params.maximize)
  {
    wmState[wmStateCount++] = x11atoms._NET_WM_STATE_MAXIMIZED_HORZ;
    wmState[wmStateCount++] = x11atoms._NET_WM_STATE_MAXIMIZED_VERT;
  }

  if (wmStateCount)
  {
    XChangeProperty(
      x11.display,
      x11.window,
      x11atoms._NET_WM_STATE,
      XA_ATOM,
      32,
      PropModeReplace,
      (unsigned char *)&wmState,
      wmStateCount
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

  XIFreeDeviceInfo(devinfo);

  XQueryExtension(x11.display, "XInputExtension", &x11.xinputOp, &event, &error);
  XIEventMask eventmask;
  unsigned char mask[XIMaskLen(XI_LASTEVENT)] = { 0 };

  eventmask.deviceid = XIAllMasterDevices;
  eventmask.mask_len = sizeof(mask);
  eventmask.mask     = mask;

  XISetMask(mask, XI_FocusIn   );
  XISetMask(mask, XI_FocusOut  );
  XISetMask(mask, XI_Enter     );
  XISetMask(mask, XI_Leave     );
  XISetMask(mask, XI_Motion    );
  XISetMask(mask, XI_KeyPress  );
  XISetMask(mask, XI_KeyRelease);

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

  /* create the blank cursor */
  {
    static char data[] = { 0x00 };
    XColor dummy;
    Pixmap temp = XCreateBitmapFromData(x11.display, x11.window, data, 1, 1);
    x11.blankCursor = XCreatePixmapCursor(x11.display, temp, temp,
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

    x11.squareCursor = XCreatePixmapCursor(x11.display, img, msk,
        &colors[0], &colors[1], 1, 1);

    XFreePixmap(x11.display, img);
    XFreePixmap(x11.display, msk);
  }

  /* default to the square cursor */
  XDefineCursor(x11.display, x11.window, x11.squareCursor);

  XMapWindow(x11.display, x11.window);
  XFlush(x11.display);

  if (!lgCreateThread("X11EventThread", x11EventThread, NULL, &x11.eventThread))
  {
    DEBUG_ERROR("Failed to create the x11 event thread");
    return false;
  }

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
}

static void x11Free(void)
{
  lgJoinThread(x11.eventThread, NULL);

  if (x11.window)
    XDestroyWindow(x11.display, x11.window);

  XFreeCursor(x11.display, x11.squareCursor);
  XFreeCursor(x11.display, x11.blankCursor);
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
  fd_set in_fds;
  const int fd = ConnectionNumber(x11.display);

  while(app_isRunning())
  {
    if (!XPending(x11.display))
    {
      FD_ZERO(&in_fds);
      FD_SET(fd, &in_fds);
      struct timeval tv =
      {
        .tv_usec = 250000,
        .tv_sec  = 0
      };

      int ret = select(fd + 1, &in_fds, NULL, NULL, &tv);
      if (ret == 0 || !XPending(x11.display))
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

      case GenericEvent:
      {
        XGenericEventCookie *cookie = (XGenericEventCookie*)&xe.xcookie;
        XGetEventData(x11.display, cookie);
        x11GenericEvent(cookie);
        XFreeEventData(x11.display, cookie);
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
          Atom type;
          int fmt;
          unsigned long num, bytes;
          unsigned char *data;

          if (XGetWindowProperty(x11.display, x11.window,
              x11atoms._NET_WM_STATE, 0, ~0L, False, AnyPropertyType,
              &type, &fmt, &num, &bytes, &data) != Success)
            break;

          bool fullscreen = false;
          for(int i = 0; i < num; ++i)
          {
            Atom prop = ((Atom *)data)[i];
            if (prop == x11atoms._NET_WM_STATE_FULLSCREEN)
              fullscreen = true;
          }

          x11.fullscreen = fullscreen;
          XFree(data);
          break;
        }

        if (xe.xproperty.atom == x11atoms._NET_FRAME_EXTENTS)
        {
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

  return 0;
}

static void x11GenericEvent(XGenericEventCookie *cookie)
{
  static int button_state = 0;

  if (cookie->extension != x11.xinputOp)
    return;

  switch(cookie->evtype)
  {
    case XI_FocusIn:
    {
      if (x11.focused)
        return;

      XIFocusOutEvent *xie = cookie->data;
      if (xie->mode != XINotifyNormal &&
          xie->mode != XINotifyWhileGrabbed &&
          xie->mode != XINotifyUngrab)
        return;

      x11.focused = true;
      app_updateCursorPos(xie->event_x, xie->event_y);
      app_handleFocusEvent(true);
      return;
    }

    case XI_FocusOut:
    {
      if (!x11.focused)
        return;

      XIFocusOutEvent *xie = cookie->data;
      if (xie->mode != XINotifyNormal &&
          xie->mode != XINotifyWhileGrabbed &&
          xie->mode != XINotifyGrab)
        return;

      app_updateCursorPos(xie->event_x, xie->event_y);
      app_handleFocusEvent(false);
      x11.focused = false;
      return;
    }

    case XI_Enter:
    {
      XIEnterEvent *xie = cookie->data;
      if (x11.entered || xie->event != x11.window)
        return;

      app_updateCursorPos(xie->event_x, xie->event_y);
      app_handleEnterEvent(true);
      x11.entered = true;
      return;
    }

    case XI_Leave:
    {
      XILeaveEvent *xie = cookie->data;
      if (!x11.entered || xie->event != x11.window ||
          button_state != 0 || app_isCaptureMode())
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
      app_handleKeyPress(device->detail - 8);
      return;
    }

    case XI_KeyRelease:
    {
      if (!x11.focused || x11.keyboardGrabbed)
        return;

      XIDeviceEvent *device = cookie->data;
      app_handleKeyRelease(device->detail - 8);
      return;
    }

    case XI_RawKeyPress:
    {
      if (!x11.focused)
        return;

      XIRawEvent *raw = cookie->data;
      app_handleKeyPress(raw->detail - 8);
      return;
    }

    case XI_RawKeyRelease:
    {
      if (!x11.focused)
        return;

      XIRawEvent *raw = cookie->data;
      app_handleKeyRelease(raw->detail - 8);
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
  eglSwapBuffers(display, surface);
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
  glXSwapIntervalEXT(x11.display, x11.window, interval);
}

static void x11GLSwapBuffers(void)
{
  glXSwapBuffers(x11.display, x11.window);
}
#endif

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

static void x11ShowPointer(bool show)
{
  if (show)
    XDefineCursor(x11.display, x11.window, x11.squareCursor);
  else
    XDefineCursor(x11.display, x11.window, x11.blankCursor);
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

  Status ret = XIGrabDevice(
      x11.display,
      x11.pointerDev,
      x11.window,
      CurrentTime,
      None,
      XIGrabModeAsync,
      XIGrabModeAsync,
      XINoOwnerEvents,
      &mask);

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
  .guestPointerUpdated = x11GuestPointerUpdated,
  .showPointer         = x11ShowPointer,
  .grabPointer         = x11GrabPointer,
  .ungrabPointer       = x11UngrabPointer,
  .capturePointer      = x11CapturePointer,
  .uncapturePointer    = x11UncapturePointer,
  .grabKeyboard        = x11GrabKeyboard,
  .ungrabKeyboard      = x11UngrabKeyboard,
  .warpPointer         = x11WarpPointer,
  .realignPointer      = x11RealignPointer,
  .isValidPointerPos   = x11IsValidPointerPos,
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
