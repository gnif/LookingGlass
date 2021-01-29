/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2021 Geoffrey McRae <geoff@hostfission.com>
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

#include "interface/displayserver.h"

#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/scrnsaver.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xinerama.h>

#include <GL/glx.h>
#include <GL/glxext.h>

#ifdef ENABLE_EGL
#include <EGL/eglext.h>
#include "egl_dynprocs.h"
#endif

#include "app.h"
#include "common/debug.h"
#include "common/thread.h"

#define _NET_WM_STATE_REMOVE 0
#define _NET_WM_STATE_ADD    1
#define _NET_WM_STATE_TOGGLE 2

struct X11DSState
{
  Display *     display;
  Window        window;
  XVisualInfo * visual;
  int        xinputOp;

  LGThread * eventThread;

  int pointerDev;
  int keyboardDev;

  bool pointerGrabbed;
  bool keyboardGrabbed;
  bool entered;
  bool focused;
  bool fullscreen;

  struct Rect   rect;
  struct Border border;

  Cursor blankCursor;
  Cursor squareCursor;

  Atom aNetReqFrameExtents;
  Atom aNetFrameExtents;
  Atom aNetWMState;
  Atom aNetWMStateFullscreen;
  Atom aNetWMWindowType;
  Atom aNetWMWindowTypeNormal;

  // clipboard members
  Atom             aSelection;
  Atom             aCurSelection;
  Atom             aTargets;
  Atom             aSelData;
  Atom             aIncr;
  Atom             aTypes[LG_CLIPBOARD_DATA_NONE];
  LG_ClipboardData type;
  bool             haveRequest;

  bool         incrStart;
  unsigned int lowerBound;

  // XFixes vars
  int eventBase;
  int errorBase;
};

static const char * atomTypes[] =
{
  "UTF8_STRING",
  "image/png",
  "image/bmp",
  "image/tiff",
  "image/jpeg"
};

static struct X11DSState x11;

// forwards
static void x11CBSelectionRequest(const XSelectionRequestEvent e);
static void x11CBSelectionClear(const XSelectionClearEvent e);
static void x11CBSelectionIncr(const XPropertyEvent e);
static void x11CBSelectionNotify(const XSelectionEvent e);
static void x11CBXFixesSelectionNotify(const XFixesSelectionNotifyEvent e);

static void x11SetFullscreen(bool fs);
static int  x11EventThread(void * unused);
static void x11GenericEvent(XGenericEventCookie *cookie);

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
      GLX_DEPTH_SIZE    , 24,
      GLX_STENCIL_SIZE  , 0,
      GLX_RED_SIZE      , 8,
      GLX_GREEN_SIZE    , 8,
      GLX_BLUE_SIZE     , 8,
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

  x11.aNetReqFrameExtents =
    XInternAtom(x11.display, "_NET_REQUEST_FRAME_EXTENTS", True);
  x11.aNetFrameExtents =
    XInternAtom(x11.display, "_NET_FRAME_EXTENTS", True);
  x11.aNetWMState =
    XInternAtom(x11.display, "_NET_WM_STATE", True);
  x11.aNetWMStateFullscreen =
    XInternAtom(x11.display, "_NET_WM_STATE_FULLSCREEN", True);
  x11.aNetWMWindowType =
    XInternAtom(x11.display, "_NET_WM_WINDOW_TYPE", True);
  x11.aNetWMWindowTypeNormal =
    XInternAtom(x11.display, "_NET_WM_WINDOW_TYPE_NORMAL", True);

    XChangeProperty(
      x11.display,
      x11.window,
      x11.aNetWMWindowType,
      XA_CARDINAL,
      32,
      PropModeReplace,
      (unsigned char *)&x11.aNetWMWindowTypeNormal,
      1
    );

  if (params.fullscreen)
  {
    XChangeProperty(
      x11.display,
      x11.window,
      x11.aNetWMState,
      XA_CARDINAL,
      32,
      PropModeReplace,
      (unsigned char *)&x11.aNetWMStateFullscreen,
      1
    );
  }

  if (x11.aNetReqFrameExtents)
  {
    XEvent reqevent =
    {
      .xclient =
      {
        .type         = ClientMessage,
        .window       = x11.window,
        .format       = 32,
        .message_type = x11.aNetReqFrameExtents
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

  bool havePointer  = false;
  bool haveKeyboard = false;
  for(int i = 0; i < count; ++i)
  {
    /* look for the master pointing device */
    if (!havePointer && devinfo[i].use == XIMasterPointer)
      for(int j = 0; j < devinfo[i].num_classes; ++j)
      {
        XIAnyClassInfo *cdevinfo =
          (XIAnyClassInfo *)(devinfo[i].classes[j]);
        if (cdevinfo->type == XIValuatorClass)
        {
          havePointer = true;
          x11.pointerDev = devinfo[i].deviceid;
          break;
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

  Atom NETWM_BYPASS_COMPOSITOR = XInternAtom(x11.display,
      "NETWM_BYPASS_COMPOSITOR", False);

  unsigned long value = 1;
  XChangeProperty(
    x11.display,
    x11.window,
    NETWM_BYPASS_COMPOSITOR,
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
  if (prop != LG_DS_MAX_MULTISAMPLE)
    return false;

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

    switch(xe.type)
    {
      case DestroyNotify:
        if (xe.xdestroywindow.display == x11.display &&
            xe.xdestroywindow.window == x11.window)
        {
          x11.window = 0;
          app_handleCloseEvent();
        }
        break;

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
          app_handleResizeEvent(x11.rect.w, x11.rect.h, border);
        }
        else
          app_handleResizeEvent(x11.rect.w, x11.rect.h, x11.border);
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

      // clipboard events
      case SelectionRequest:
        x11CBSelectionRequest(xe.xselectionrequest);
        break;

      case SelectionClear:
        x11CBSelectionClear(xe.xselectionclear);
        break;

      case SelectionNotify:
        x11CBSelectionNotify(xe.xselection);
        break;

      case PropertyNotify:
        if (xe.xproperty.display != x11.display      ||
            xe.xproperty.window  != x11.window       ||
            xe.xproperty.state   != PropertyNewValue)
          break;

        if (xe.xproperty.atom == x11.aNetWMState)
        {
          Atom type;
          int fmt;
          unsigned long num, bytes;
          unsigned char *data;

          if (XGetWindowProperty(x11.display, x11.window,
              x11.aNetWMState, 0, ~0L, False, AnyPropertyType,
              &type, &fmt, &num, &bytes, &data) != Success)
            break;

          bool fullscreen = false;
          for(int i = 0; i < num; ++i)
          {
            Atom prop = ((Atom *)data)[i];
            if (prop == x11.aNetWMStateFullscreen)
              fullscreen = true;
          }

          x11.fullscreen = fullscreen;
          XFree(data);
          break;
        }

        if (xe.xproperty.atom == x11.aNetFrameExtents)
        {
          Atom type;
          int fmt;
          unsigned long num, bytes;
          unsigned char *data;

          if (XGetWindowProperty(x11.display, x11.window,
                x11.aNetFrameExtents, 0, 4, False, AnyPropertyType,
                &type, &fmt, &num, &bytes, &data) != Success)
              break;

          if (num >= 4)
          {
            long *cardinal = (long *)data;
            x11.border.left   = cardinal[0];
            x11.border.right  = cardinal[1];
            x11.border.top    = cardinal[2];
            x11.border.bottom = cardinal[3];
            app_handleResizeEvent(x11.rect.w, x11.rect.h, x11.border);
          }

          XFree(data);
          break;
        }

        if (xe.xproperty.atom == x11.aSelData)
        {
          if (x11.lowerBound == 0)
            break;

          x11CBSelectionIncr(xe.xproperty);
          break;
        }

        break;

      default:
        if (xe.type == x11.eventBase + XFixesSelectionNotify)
        {
          XFixesSelectionNotifyEvent * sne = (XFixesSelectionNotifyEvent *)&xe;
          x11CBXFixesSelectionNotify(*sne);
        }
        break;
    }
  }

  return 0;
}

static void x11GenericEvent(XGenericEventCookie *cookie)
{
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
      if (x11.entered)
        return;

      XIEnterEvent *xie = cookie->data;
      app_updateCursorPos(xie->event_x, xie->event_y);
      app_handleEnterEvent(true);
      x11.entered = true;
      return;
    }

    case XI_Leave:
    {
      if (!x11.entered)
        return;

      XILeaveEvent *xie = cookie->data;
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

      prev_time   = raw->time;
      prev_detail = raw->detail;

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

      prev_time   = raw->time;
      prev_detail = raw->detail;

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
      double raw_axis[2];
      double axis[2];

      /* select the active validators for the X & Y axis */
      double *valuator = raw->valuators.values;
      double *r_value  = raw->raw_values;
      int    count     = 0;
      for(int i = 0; i < raw->valuators.mask_len * 8; ++i)
      {
        if (XIMaskIsSet(raw->valuators.mask, i))
        {
          raw_axis[count] = *r_value;
          axis    [count] = *valuator;
          ++count;

          if (count == 2)
            break;

          ++valuator;
          ++r_value;
        }
      }

      /* filter out scroll wheel and other events */
      if (count < 2)
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

  if (strstr(early_exts, "EGL_KHR_platform_base") != NULL &&
      g_egl_dynProcs.eglGetPlatformDisplay)
  {
    DEBUG_INFO("Using eglGetPlatformDisplay");
    ret = g_egl_dynProcs.eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR,
        x11.display, NULL);
  }
  else if (strstr(early_exts, "EGL_EXT_platform_base") != NULL &&
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

static void x11EGLSwapBuffers(EGLDisplay display, EGLSurface surface)
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
      .message_type = x11.aNetWMState,
      .format       = 32,
      .window       = x11.window,
      .data.l       = {
        fs ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE,
        x11.aNetWMStateFullscreen,
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

static bool x11CBInit()
{
  x11.aSelection    = XInternAtom(x11.display, "CLIPBOARD"  , False);
  x11.aTargets      = XInternAtom(x11.display, "TARGETS"    , False);
  x11.aSelData      = XInternAtom(x11.display, "SEL_DATA"   , False);
  x11.aIncr         = XInternAtom(x11.display, "INCR"       , False);
  x11.aCurSelection = BadValue;

  for(int i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
  {
    x11.aTypes[i] = XInternAtom(x11.display, atomTypes[i], False);
    if (x11.aTypes[i] == BadAlloc || x11.aTypes[i] == BadValue)
    {
      DEBUG_ERROR("failed to get atom for type: %s", atomTypes[i]);
      return false;
    }
  }

  // use xfixes to get clipboard change notifications
  if (!XFixesQueryExtension(x11.display, &x11.eventBase, &x11.errorBase))
  {
    DEBUG_ERROR("failed to initialize xfixes");
    return false;
  }

  XFixesSelectSelectionInput(x11.display, x11.window,
      XA_PRIMARY, XFixesSetSelectionOwnerNotifyMask);
  XFixesSelectSelectionInput(x11.display, x11.window,
      x11.aSelection, XFixesSetSelectionOwnerNotifyMask);

  return true;
}

static void x11CBReplyFn(void * opaque, LG_ClipboardData type,
    uint8_t * data, uint32_t size)
{
  XEvent *s = (XEvent *)opaque;

  XChangeProperty(
      x11.display          ,
      s->xselection.requestor,
      s->xselection.property ,
      s->xselection.target   ,
      8,
      PropModeReplace,
      data,
      size);

  XSendEvent(x11.display, s->xselection.requestor, 0, 0, s);
  XFlush(x11.display);
  free(s);
}

static void x11CBSelectionRequest(const XSelectionRequestEvent e)
{
  XEvent * s = (XEvent *)malloc(sizeof(XEvent));
  s->xselection.type      = SelectionNotify;
  s->xselection.requestor = e.requestor;
  s->xselection.selection = e.selection;
  s->xselection.target    = e.target;
  s->xselection.property  = e.property;
  s->xselection.time      = e.time;

  if (!x11.haveRequest)
    goto nodata;

  // target list requested
  if (e.target == x11.aTargets)
  {
    Atom targets[2];
    targets[0] = x11.aTargets;
    targets[1] = x11.aTypes[x11.type];

    XChangeProperty(
        e.display,
        e.requestor,
        e.property,
        XA_ATOM,
        32,
        PropModeReplace,
        (unsigned char*)targets,
        sizeof(targets) / sizeof(Atom));

    goto send;
  }

  // look to see if we can satisfy the data type
  for(int i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
    if (x11.aTypes[i] == e.target && x11.type == i)
    {
      // request the data
      app_clipboardRequest(x11CBReplyFn, s);
      return;
    }

nodata:
  // report no data
  s->xselection.property = None;

send:
  XSendEvent(x11.display, e.requestor, 0, 0, s);
  XFlush(x11.display);
  free(s);
}

static void x11CBSelectionClear(const XSelectionClearEvent e)
{
  if (e.selection != XA_PRIMARY && e.selection != x11.aSelection)
    return;

  x11.aCurSelection = BadValue;
  app_clipboardRelease();
  return;
}

static void x11CBSelectionIncr(const XPropertyEvent e)
{
  Atom type;
  int format;
  unsigned long itemCount, after;
  unsigned char *data;

  if (XGetWindowProperty(
      e.display,
      e.window,
      e.atom,
      0, ~0L, // start and length
      True,   // delete the property
      x11.aIncr,
      &type,
      &format,
      &itemCount,
      &after,
      &data) != Success)
  {
    DEBUG_INFO("GetProp Failed");
    app_clipboardNotify(LG_CLIPBOARD_DATA_NONE, 0);
    goto out;
  }

  LG_ClipboardData dataType;
  for(dataType = 0; dataType < LG_CLIPBOARD_DATA_NONE; ++dataType)
    if (x11.aTypes[dataType] == type)
      break;

  if (dataType == LG_CLIPBOARD_DATA_NONE)
  {
    DEBUG_WARN("clipboard data (%s) not in a supported format",
        XGetAtomName(x11.display, type));

    x11.lowerBound = 0;
    app_clipboardNotify(LG_CLIPBOARD_DATA_NONE, 0);
    goto out;
  }

  if (x11.incrStart)
  {
    app_clipboardNotify(dataType, x11.lowerBound);
    x11.incrStart = false;
  }

  XFree(data);
  data = NULL;

  if (XGetWindowProperty(
      e.display,
      e.window,
      e.atom,
      0, ~0L, // start and length
      True,   // delete the property
      type,
      &type,
      &format,
      &itemCount,
      &after,
      &data) != Success)
  {
    DEBUG_ERROR("XGetWindowProperty Failed");
    app_clipboardNotify(LG_CLIPBOARD_DATA_NONE, 0);
    goto out;
  }

  app_clipboardData(dataType, data, itemCount);
  x11.lowerBound -= itemCount;

out:
  if (data)
    XFree(data);
}

static void x11CBXFixesSelectionNotify(const XFixesSelectionNotifyEvent e)
{
  // check if the selection is valid and it isn't ourself
  if ((e.selection != XA_PRIMARY && e.selection != x11.aSelection) ||
      e.owner == x11.window || e.owner == 0)
  {
    return;
  }

  // remember which selection we are working with
  x11.aCurSelection = e.selection;
  XConvertSelection(
      x11.display,
      e.selection,
      x11.aTargets,
      x11.aTargets,
      x11.window,
      CurrentTime);

  return;
}

static void x11CBSelectionNotify(const XSelectionEvent e)
{
  if (e.property == None)
    return;

  Atom type;
  int format;
  unsigned long itemCount, after;
  unsigned char *data;

  if (XGetWindowProperty(
      e.display,
      e.requestor,
      e.property,
      0, ~0L, // start and length
      True  , // delete the property
      AnyPropertyType,
      &type,
      &format,
      &itemCount,
      &after,
      &data) != Success)
  {
    app_clipboardNotify(LG_CLIPBOARD_DATA_NONE, 0);
    goto out;
  }

  if (type == x11.aIncr)
  {
    x11.incrStart  = true;
    x11.lowerBound = *(unsigned int *)data;
    goto out;
  }

  // the target list
  if (e.property == x11.aTargets)
  {
    // the format is 32-bit and we must have data
    // this is technically incorrect however as it's
    // an array of padded 64-bit values
    if (!data || format != 32)
      goto out;

    // see if we support any of the targets listed
    const uint64_t * targets = (const uint64_t *)data;
    for(unsigned long i = 0; i < itemCount; ++i)
    {
      for(int n = 0; n < LG_CLIPBOARD_DATA_NONE; ++n)
        if (x11.aTypes[n] == targets[i])
        {
          // we have a match, so send the notification
          app_clipboardNotify(n, 0);
          goto out;
        }
    }

    // no matches
    app_clipboardNotify(LG_CLIPBOARD_DATA_NONE, 0);
    goto out;
  }

  if (e.property == x11.aSelData)
  {
    LG_ClipboardData dataType;
    for(dataType = 0; dataType < LG_CLIPBOARD_DATA_NONE; ++dataType)
      if (x11.aTypes[dataType] == type)
        break;

    if (dataType == LG_CLIPBOARD_DATA_NONE)
    {
      DEBUG_WARN("clipboard data (%s) not in a supported format",
          XGetAtomName(x11.display, type));
      goto out;
    }

    app_clipboardData(dataType, data, itemCount);
    goto out;
  }

out:
  if (data)
    XFree(data);
}

static void x11CBNotice(LG_ClipboardData type)
{
  x11.haveRequest = true;
  x11.type        = type;
  XSetSelectionOwner(x11.display, XA_PRIMARY      , x11.window, CurrentTime);
  XSetSelectionOwner(x11.display, x11.aSelection, x11.window, CurrentTime);
  XFlush(x11.display);
}

static void x11CBRelease(void)
{
  x11.haveRequest = false;
  XSetSelectionOwner(x11.display, XA_PRIMARY      , None, CurrentTime);
  XSetSelectionOwner(x11.display, x11.aSelection, None, CurrentTime);
  XFlush(x11.display);
}

static void x11CBRequest(LG_ClipboardData type)
{
  if (x11.aCurSelection == BadValue)
    return;

  XConvertSelection(
      x11.display,
      x11.aCurSelection,
      x11.aTypes[type],
      x11.aSelData,
      x11.window,
      CurrentTime);
}

struct LG_DisplayServerOps LGDS_X11 =
{
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
  .showPointer        = x11ShowPointer,
  .grabPointer        = x11GrabPointer,
  .ungrabPointer      = x11UngrabPointer,
  .grabKeyboard       = x11GrabKeyboard,
  .ungrabKeyboard     = x11UngrabKeyboard,
  .warpPointer        = x11WarpPointer,
  .realignPointer     = x11RealignPointer,
  .isValidPointerPos  = x11IsValidPointerPos,
  .inhibitIdle        = x11InhibitIdle,
  .uninhibitIdle      = x11UninhibitIdle,
  .wait               = x11Wait,
  .setWindowSize      = x11SetWindowSize,
  .setFullscreen      = x11SetFullscreen,
  .getFullscreen      = x11GetFullscreen,

  .cbInit    = x11CBInit,
  .cbNotice  = x11CBNotice,
  .cbRelease = x11CBRelease,
  .cbRequest = x11CBRequest
};
