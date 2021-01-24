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

#include <SDL2/SDL.h>
#include <X11/Xlib.h>
#include <GL/glx.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <X11/extensions/XInput2.h>
#include <X11/extensions/scrnsaver.h>
#include <X11/extensions/Xfixes.h>

#include "app.h"
#include "common/debug.h"

struct X11DSState
{
  Display * display;
  Window    window;
  int       xinputOp;

  int pointerDev;
  int keyboardDev;

  bool pointerGrabbed;
  bool keyboardGrabbed;
  bool entered;
  bool focused;

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

static bool x11Init(SDL_SysWMinfo * info)
{
  XIDeviceInfo *devinfo;
  int count;
  int event, error;

  memset(&x11, 0, sizeof(x11));
  x11.display = info->info.x11.display;
  x11.window  = info->info.x11.window;

  int major = 2;
  int minor = 3;
  if (XIQueryVersion(x11.display, &major, &minor) != Success)
  {
    DEBUG_ERROR("Failed to query the XInput version");
    return false;
  }

  DEBUG_INFO("X11 XInput %d.%d in use", major, minor);

  XQueryExtension(x11.display, "XInputExtension", &x11.xinputOp, &event, &error);

  int num_masks;
  XIEventMask * mask = XIGetSelectedEvents(x11.display, x11.window, &num_masks);
  if (!mask)
  {
    DEBUG_ERROR("Failed to get the XInput event mask");
    return false;
  }

  for(int i = 0; i < num_masks; ++i)
  {
    XISetMask(mask[i].mask, XI_Motion    );
    XISetMask(mask[i].mask, XI_FocusIn   );
    XISetMask(mask[i].mask, XI_FocusOut  );
    XISetMask(mask[i].mask, XI_Enter     );
    XISetMask(mask[i].mask, XI_Leave     );
    XISetMask(mask[i].mask, XI_KeyPress  );
    XISetMask(mask[i].mask, XI_KeyRelease);
  }

  if (XISelectEvents(x11.display, x11.window, mask, num_masks) != Success)
  {
    XFree(mask);
    DEBUG_ERROR("Failed to select the xinput events");
    return false;
  }

  XFree(mask);

  devinfo = XIQueryDevice(x11.display, XIAllDevices, &count);
  if (!devinfo)
  {
    DEBUG_ERROR("XIQueryDevice failed");
    return false;
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
    return false;
  }

  if (!haveKeyboard)
  {
    DEBUG_ERROR("Failed to find the master keyboard device");
    XIFreeDeviceInfo(devinfo);
    return false;
  }

  XIFreeDeviceInfo(devinfo);

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

  return true;
}

static void x11Startup(void)
{
}

static void x11Shutdown(void)
{
}

static void x11Free(void)
{
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
    if ((res = glXGetConfig(dpy, visual, GLX_USE_GL, &supportsGL)) != 0 || !supportsGL)
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

static bool x11EventFilter(SDL_Event * event)
{
  /* prevent the default processing for the following events */
  switch(event->type)
  {
    case SDL_WINDOWEVENT:
      switch(event->window.event)
      {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_CLOSE:
        case SDL_WINDOWEVENT_FOCUS_GAINED:
        case SDL_WINDOWEVENT_FOCUS_LOST:
        case SDL_WINDOWEVENT_ENTER:
        case SDL_WINDOWEVENT_LEAVE:
          return true;
      }
      return false;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
      return true;
  }

  if (event->type != SDL_SYSWMEVENT)
    return false;

  XEvent xe = event->syswm.msg->msg.x11.event;
  switch(xe.type)
  {
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

      app_updateWindowPos(x, y);
      app_handleResizeEvent(xe.xconfigure.width, xe.xconfigure.height);
      return true;
    }

    case GenericEvent:
    {
      XGenericEventCookie *cookie = (XGenericEventCookie*)&xe.xcookie;
      if (cookie->extension != x11.xinputOp)
        return false;

      switch(cookie->evtype)
      {
        case XI_FocusIn:
        {
          if (x11.focused)
            return true;

          XIFocusOutEvent *xie = cookie->data;
          if (xie->mode != XINotifyNormal &&
              xie->mode != XINotifyWhileGrabbed &&
              xie->mode != XINotifyUngrab)
            return true;

          x11.focused = true;
          app_updateCursorPos(xie->event_x, xie->event_y);
          app_handleFocusEvent(true);
          return true;
        }

        case XI_FocusOut:
        {
          if (!x11.focused)
            return true;

          XIFocusOutEvent *xie = cookie->data;
          if (xie->mode != XINotifyNormal &&
              xie->mode != XINotifyWhileGrabbed &&
              xie->mode != XINotifyGrab)
            return true;

          app_updateCursorPos(xie->event_x, xie->event_y);
          app_handleFocusEvent(false);
          x11.focused = false;
          return true;
        }

        case XI_Enter:
        {
          if (x11.entered)
            return true;

          XIEnterEvent *xie = cookie->data;
          app_updateCursorPos(xie->event_x, xie->event_y);
          app_handleWindowEnter();
          x11.entered = true;
          return true;
        }

        case XI_Leave:
        {
          if (!x11.entered)
            return true;

          XILeaveEvent *xie = cookie->data;
          app_updateCursorPos(xie->event_x, xie->event_y);
          app_handleWindowLeave();
          x11.entered = false;
          return true;
        }

        case XI_KeyPress:
        {
          if (!x11.focused || x11.keyboardGrabbed)
            return true;

          XIDeviceEvent *device = cookie->data;
          app_handleKeyPress(device->detail - 8);
          return true;
        }

        case XI_KeyRelease:
        {
          if (!x11.focused || x11.keyboardGrabbed)
            return true;

          XIDeviceEvent *device = cookie->data;
          app_handleKeyRelease(device->detail - 8);
          return true;
        }

        case XI_RawKeyPress:
        {
          if (!x11.focused)
            return true;

          XIRawEvent *raw = cookie->data;
          app_handleKeyPress(raw->detail - 8);
          return true;
        }

        case XI_RawKeyRelease:
        {
          if (!x11.focused)
            return true;

          XIRawEvent *raw = cookie->data;
          app_handleKeyRelease(raw->detail - 8);
          return true;
        }

        case XI_RawButtonPress:
        {
          if (!x11.focused || !x11.entered)
            return true;

          XIRawEvent *raw = cookie->data;

          /* filter out duplicate events */
          static Time         prev_time   = 0;
          static unsigned int prev_detail = 0;
          if (raw->time == prev_time && raw->detail == prev_detail)
            return true;

          prev_time   = raw->time;
          prev_detail = raw->detail;

          app_handleButtonPress(
              raw->detail > 5 ? raw->detail - 2 : raw->detail);
          return true;
        }

        case XI_RawButtonRelease:
        {
          if (!x11.focused || !x11.entered)
            return true;

          XIRawEvent *raw = cookie->data;

          /* filter out duplicate events */
          static Time         prev_time   = 0;
          static unsigned int prev_detail = 0;
          if (raw->time == prev_time && raw->detail == prev_detail)
            return true;

          prev_time   = raw->time;
          prev_detail = raw->detail;

          app_handleButtonRelease(
              raw->detail > 5 ? raw->detail - 2 : raw->detail);
          return true;
        }

        case XI_Motion:
        {
          XIDeviceEvent *device = cookie->data;
          app_updateCursorPos(device->event_x, device->event_y);

          if (!x11.pointerGrabbed)
            app_handleMouseNormal(0.0, 0.0);
          return true;
        }

        case XI_RawMotion:
        {
          if (!x11.focused || !x11.entered)
            return true;

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
            return true;

          /* filter out duplicate events */
          static Time   prev_time    = 0;
          static double prev_axis[2] = {0};
          if (raw->time == prev_time &&
              axis[0] == prev_axis[0] &&
              axis[1] == prev_axis[1])
            return true;

          prev_time = raw->time;
          prev_axis[0] = axis[0];
          prev_axis[1] = axis[1];

          if (app_cursorIsGrabbed())
          {
            if (app_cursorWantsRaw())
              app_handleMouseGrabbed(raw_axis[0], raw_axis[1]);
            else
              app_handleMouseGrabbed(axis[0], axis[1]);
          }
          else
            if (app_cursorInWindow())
              app_handleMouseNormal(axis[0], axis[1]);

          return true;
        }
      }

      return false;
    }

    // clipboard events
    case SelectionRequest:
      x11CBSelectionRequest(xe.xselectionrequest);
      return true;

    case SelectionClear:
      x11CBSelectionClear(xe.xselectionclear);
      return true;

    case SelectionNotify:
      x11CBSelectionNotify(xe.xselection);
      return true;

    case PropertyNotify:
      if (xe.xproperty.display != x11.display    ||
          xe.xproperty.window  != x11.window     ||
          xe.xproperty.atom    != x11.aSelData   ||
          xe.xproperty.state   != PropertyNewValue ||
          x11.lowerBound    == 0)
        return false;

      x11CBSelectionIncr(xe.xproperty);
      return true;

    default:
      if (xe.type == x11.eventBase + XFixesSelectionNotify)
      {
        XFixesSelectionNotifyEvent * sne = (XFixesSelectionNotifyEvent *)&xe;
        x11CBXFixesSelectionNotify(*sne);
        return true;
      }
      return false;
  }
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

  unsigned char mask_bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
  XIEventMask mask = {
    XIAllMasterDevices,
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
    XIAllMasterDevices,
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

static void x11InhibitIdle(void)
{
  XScreenSaverSuspend(x11.display, true);
}

static void x11UninhibitIdle(void)
{
  XScreenSaverSuspend(x11.display, false);
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
  .subsystem      = SDL_SYSWM_X11,
  .init           = x11Init,
  .startup        = x11Startup,
  .shutdown       = x11Shutdown,
  .free           = x11Free,
  .getProp        = x11GetProp,
  .eventFilter    = x11EventFilter,
  .grabPointer    = x11GrabPointer,
  .ungrabPointer  = x11UngrabPointer,
  .grabKeyboard   = x11GrabKeyboard,
  .ungrabKeyboard = x11UngrabKeyboard,
  .warpPointer    = x11WarpPointer,

  .inhibitIdle   = x11InhibitIdle,
  .uninhibitIdle = x11UninhibitIdle,

  .cbInit    = x11CBInit,
  .cbNotice  = x11CBNotice,
  .cbRelease = x11CBRelease,
  .cbRequest = x11CBRequest
};
