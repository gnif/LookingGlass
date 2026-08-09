/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

#include "clipboard.h"
#include "x11.h"
#include "atoms.h"

#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "app.h"
#include "common/array.h"
#include "common/debug.h"
#include "common/locking.h"

struct X11ClipboardRead
{
  Window              window;
  LG_ClipboardRequest request;
  LG_ClipboardData    type;
  bool                incremental;
  uint8_t           * buffer;
  size_t              size;
  size_t              capacity;
};

struct X11ClipboardState
{
  LG_Lock          lock;
  Atom             aCurSelection;
  Atom             aTypes[LG_CLIPBOARD_DATA_NONE];
  Window           targetsWindow;
  LG_ClipboardData type;
  bool             haveRequest;

  struct X11ClipboardRead read;
};

static const char * atomTypes[] =
{
  "UTF8_STRING",
  "image/png",
  "image/bmp",
  "image/tiff",
  "image/jpeg"
};

static struct X11ClipboardState x11cb;

// forwards
static void x11CBSelectionRequest(const XSelectionRequestEvent e);
static void x11CBSelectionClear(const XSelectionClearEvent e);
static void x11CBSelectionIncr(const XPropertyEvent e);
static void x11CBSelectionNotify(const XSelectionEvent e);
static void x11CBXFixesSelectionNotify(const XFixesSelectionNotifyEvent e);
static LG_ClipboardRequest cancelReadNL(void);
static void clearTargetsNL(void);

bool x11CBEventThread(const XEvent * xe)
{
  switch(xe->type)
  {
    case SelectionRequest:
      x11CBSelectionRequest(xe->xselectionrequest);
      return true;

    case SelectionClear:
      x11CBSelectionClear(xe->xselectionclear);
      return true;

    case SelectionNotify:
      x11CBSelectionNotify(xe->xselection);
      return true;

    case PropertyNotify:
      if (xe->xproperty.state != PropertyNewValue)
        break;

      if (xe->xproperty.atom == x11atoms.SEL_DATA)
      {
        x11CBSelectionIncr(xe->xproperty);
        return true;
      }
      break;

    default:
      if (xe->type == x11.eventBase + XFixesSelectionNotify)
      {
        XFixesSelectionNotifyEvent * sne = (XFixesSelectionNotifyEvent *)xe;
        x11CBXFixesSelectionNotify(*sne);
        return true;
      }
      break;
  }

  return false;
}

bool x11CBInit(void)
{
  LG_LOCK_INIT(x11cb.lock);
  x11cb.aCurSelection = BadValue;
  for(int i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
  {
    x11cb.aTypes[i] = XInternAtom(x11.display, atomTypes[i], False);
    if (x11cb.aTypes[i] == BadAlloc || x11cb.aTypes[i] == BadValue)
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
      x11atoms.CLIPBOARD, XFixesSetSelectionOwnerNotifyMask);

  return true;
}

static void x11CBReplyFn(void * opaque, LG_ClipboardData type,
    const uint8_t * data, uint32_t size)
{
  XEvent *s = (XEvent *)opaque;

  if (type == LG_CLIPBOARD_DATA_NONE)
    s->xselection.property = None;
  else
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
  XEvent * s = malloc(sizeof(*s));
  if (!s)
  {
    DEBUG_ERROR("out of memory");
    return;
  }

  s->xselection.type      = SelectionNotify;
  s->xselection.requestor = e.requestor;
  s->xselection.selection = e.selection;
  s->xselection.target    = e.target;
  s->xselection.property  = e.property;
  s->xselection.time      = e.time;

  LG_LOCK(x11cb.lock);
  const bool haveRequest = x11cb.haveRequest;
  const LG_ClipboardData requestType = x11cb.type;
  LG_UNLOCK(x11cb.lock);

  if (!haveRequest)
    goto nodata;

  // target list requested
  if (e.target == x11atoms.TARGETS)
  {
    Atom targets[2];
    targets[0] = x11atoms.TARGETS;
    targets[1] = x11cb.aTypes[requestType];

    XChangeProperty(
      e.display,
      e.requestor,
      e.property,
      XA_ATOM,
      32,
      PropModeReplace,
      (unsigned char*)targets,
      ARRAY_LENGTH(targets)
    );

    goto send;
  }

  // look to see if we can satisfy the data type
  for(int i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
    if (x11cb.aTypes[i] == e.target && requestType == i)
    {
      // request the data
      if (app_clipboardRequest(requestType, x11CBReplyFn, s))
        return;
      goto nodata;
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
  (void)e;
}

/* x11cb.lock must be held. */
static struct X11ClipboardRead clearReadNL(void)
{
  const struct X11ClipboardRead read = x11cb.read;
  x11cb.read = (struct X11ClipboardRead) { 0 };
  if (read.window)
    XDestroyWindow(x11.display, read.window);
  return read;
}

/* x11cb.lock must be held. */
static LG_ClipboardRequest cancelReadNL(void)
{
  const struct X11ClipboardRead read = clearReadNL();
  free(read.buffer);
  return read.request;
}

/* x11cb.lock must be held. */
static void clearTargetsNL(void)
{
  if (!x11cb.targetsWindow)
    return;

  XDestroyWindow(x11.display, x11cb.targetsWindow);
  x11cb.targetsWindow = 0;
}

static void x11CBSelectionIncr(const XPropertyEvent e)
{
  Atom type;
  int format;
  unsigned long itemCount;
  unsigned long after;
  unsigned char * data = NULL;

  LG_LOCK(x11cb.lock);
  if (!x11cb.read.window || !x11cb.read.incremental ||
      e.window != x11cb.read.window ||
      e.atom != x11atoms.SEL_DATA)
  {
    LG_UNLOCK(x11cb.lock);
    return;
  }

  if (XGetWindowProperty(
      e.display,
      e.window,
      e.atom,
      0, ~0L, // start and length
      True,   // delete the property
      AnyPropertyType,
      &type,
      &format,
      &itemCount,
      &after,
      &data) != Success)
  {
    DEBUG_ERROR("XGetWindowProperty Failed");
    const struct X11ClipboardRead read = clearReadNL();
    LG_UNLOCK(x11cb.lock);
    free(read.buffer);
    app_clipboardAbort(read.request);
    return;
  }

  LG_ClipboardData dataType;
  for(dataType = 0; dataType < LG_CLIPBOARD_DATA_NONE; ++dataType)
    if (x11cb.aTypes[dataType] == type)
      break;

  if ((itemCount && !data) || dataType == LG_CLIPBOARD_DATA_NONE ||
      dataType != x11cb.read.type || format != 8)
  {
    DEBUG_WARN("Invalid incremental clipboard data");
    const struct X11ClipboardRead read = clearReadNL();
    LG_UNLOCK(x11cb.lock);
    if (data)
      XFree(data);
    free(read.buffer);
    app_clipboardAbort(read.request);
    return;
  }

  if (itemCount == 0)
  {
    const struct X11ClipboardRead read = clearReadNL();
    LG_UNLOCK(x11cb.lock);
    if (data)
      XFree(data);
    app_clipboardData(
        read.request, read.type, read.buffer, read.size);
    free(read.buffer);
    return;
  }

  if (itemCount > SIZE_MAX - x11cb.read.size)
  {
    const struct X11ClipboardRead read = clearReadNL();
    LG_UNLOCK(x11cb.lock);
    XFree(data);
    free(read.buffer);
    app_clipboardAbort(read.request);
    return;
  }

  const size_t required = x11cb.read.size + itemCount;
  if (required > x11cb.read.capacity)
  {
    size_t capacity = x11cb.read.capacity;
    while (capacity < required)
    {
      if (capacity > SIZE_MAX / 2)
      {
        capacity = required;
        break;
      }
      capacity *= 2;
    }

    uint8_t * buffer = realloc(x11cb.read.buffer, capacity);
    if (!buffer)
    {
      const struct X11ClipboardRead read = clearReadNL();
      LG_UNLOCK(x11cb.lock);
      XFree(data);
      free(read.buffer);
      app_clipboardAbort(read.request);
      return;
    }
    x11cb.read.buffer   = buffer;
    x11cb.read.capacity = capacity;
  }

  memcpy(x11cb.read.buffer + x11cb.read.size, data, itemCount);
  x11cb.read.size += itemCount;
  LG_UNLOCK(x11cb.lock);
  if (data)
    XFree(data);
}

static void x11CBXFixesSelectionNotify(const XFixesSelectionNotifyEvent e)
{
  if (e.selection != x11atoms.CLIPBOARD || e.owner == x11.window)
    return;

  LG_LOCK(x11cb.lock);
  XGrabServer(x11.display);
  if (XGetSelectionOwner(x11.display, x11atoms.CLIPBOARD) != e.owner)
  {
    XUngrabServer(x11.display);
    XFlush(x11.display);
    LG_UNLOCK(x11cb.lock);
    return;
  }

  const LG_ClipboardRequest oldRequest = x11cb.read.window ?
    cancelReadNL() : LG_CLIPBOARD_REQUEST_INVALID;
  clearTargetsNL();

  if (e.owner == 0)
  {
    x11cb.aCurSelection = BadValue;
    XUngrabServer(x11.display);
    XFlush(x11.display);
    LG_UNLOCK(x11cb.lock);
    if (oldRequest != LG_CLIPBOARD_REQUEST_INVALID)
      app_clipboardAbort(oldRequest);
    app_clipboardRelease();
    return;
  }

  // remember which selection we are working with
  x11cb.aCurSelection = e.selection;
  x11cb.targetsWindow = XCreateSimpleWindow(
      x11.display, x11.window, 0, 0, 1, 1, 0, 0, 0);
  if (!x11cb.targetsWindow)
  {
    x11cb.aCurSelection = BadValue;
    XUngrabServer(x11.display);
    XFlush(x11.display);
    LG_UNLOCK(x11cb.lock);
    if (oldRequest != LG_CLIPBOARD_REQUEST_INVALID)
      app_clipboardAbort(oldRequest);
    app_clipboardRelease();
    return;
  }

  XConvertSelection(
      x11.display,
      e.selection,
      x11atoms.TARGETS,
      x11atoms.TARGETS,
      x11cb.targetsWindow,
      CurrentTime);
  XUngrabServer(x11.display);
  XFlush(x11.display);
  LG_UNLOCK(x11cb.lock);

  if (oldRequest != LG_CLIPBOARD_REQUEST_INVALID)
    app_clipboardAbort(oldRequest);
}

static void x11CBSelectionNotify(const XSelectionEvent e)
{
  Atom type;
  int format;
  unsigned long itemCount;
  unsigned long after;
  unsigned char * data = NULL;

  LG_LOCK(x11cb.lock);
  const bool targetReply = x11cb.targetsWindow &&
    e.requestor == x11cb.targetsWindow && e.target == x11atoms.TARGETS;
  if (targetReply)
  {
    if (e.property == None)
    {
      clearTargetsNL();
      LG_UNLOCK(x11cb.lock);
      app_clipboardRelease();
      return;
    }

    if (XGetWindowProperty(
        e.display,
        e.requestor,
        e.property,
        0, ~0L, // start and length
        True,   // delete the property
        AnyPropertyType,
        &type,
        &format,
        &itemCount,
        &after,
        &data) != Success)
    {
      clearTargetsNL();
      LG_UNLOCK(x11cb.lock);
      app_clipboardRelease();
      return;
    }
    clearTargetsNL();
    LG_UNLOCK(x11cb.lock);

    // the format is 32-bit and we must have data
    // this is technically incorrect however as it's
    // an array of padded 64-bit values
    if (!data || format != 32)
    {
      app_clipboardRelease();
      goto out;
    }

    int typeCount = 0;
    LG_ClipboardData types[LG_CLIPBOARD_DATA_NONE];

    // see if we support any of the targets listed
    const Atom * targets = (const Atom *)data;
    for(int n = 0; n < LG_CLIPBOARD_DATA_NONE; ++n)
      for(unsigned long i = 0; i < itemCount; ++i)
        if (x11cb.aTypes[n] == targets[i])
        {
          types[typeCount++] = n;
          break;
        }

    app_clipboardNotifyTypes(types, typeCount);
    goto out;
  }
  LG_UNLOCK(x11cb.lock);

  LG_LOCK(x11cb.lock);
  if (!x11cb.read.window || e.requestor != x11cb.read.window)
  {
    LG_UNLOCK(x11cb.lock);
    return;
  }

  if (e.property == None)
  {
    const LG_ClipboardRequest request = cancelReadNL();
    LG_UNLOCK(x11cb.lock);
    app_clipboardAbort(request);
    return;
  }

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
    const LG_ClipboardRequest request = cancelReadNL();
    LG_UNLOCK(x11cb.lock);
    app_clipboardAbort(request);
    return;
  }

  if (type == x11atoms.INCR)
  {
    if (!data || format != 32 || itemCount < 1)
    {
      const LG_ClipboardRequest request = cancelReadNL();
      LG_UNLOCK(x11cb.lock);
      if (data)
        XFree(data);
      app_clipboardAbort(request);
      return;
    }

    size_t capacity = *(const unsigned long *)data;
    if (capacity < 4096)
      capacity = 4096;
    if (capacity > 1048576)
      capacity = 1048576;
    x11cb.read.buffer = malloc(capacity);
    if (!x11cb.read.buffer)
    {
      const LG_ClipboardRequest request = cancelReadNL();
      LG_UNLOCK(x11cb.lock);
      XFree(data);
      app_clipboardAbort(request);
      return;
    }
    x11cb.read.capacity    = capacity;
    x11cb.read.incremental = true;
    LG_UNLOCK(x11cb.lock);
    XFree(data);
    return;
  }

  LG_ClipboardData dataType;
  for(dataType = 0; dataType < LG_CLIPBOARD_DATA_NONE; ++dataType)
    if (x11cb.aTypes[dataType] == type)
      break;

  const LG_ClipboardRequest request = x11cb.read.request;
  const bool valid = dataType != LG_CLIPBOARD_DATA_NONE &&
    dataType == x11cb.read.type && format == 8;
  cancelReadNL();
  LG_UNLOCK(x11cb.lock);

  if (!valid)
  {
    DEBUG_WARN("Invalid clipboard data");
    app_clipboardAbort(request);
    goto out;
  }

  app_clipboardData(request, dataType, data, itemCount);

out:
  if (data)
    XFree(data);
}

void x11CBNotice(LG_ClipboardData type)
{
  LG_LOCK(x11cb.lock);
  const LG_ClipboardRequest oldRequest = x11cb.read.window ?
    cancelReadNL() : LG_CLIPBOARD_REQUEST_INVALID;
  clearTargetsNL();
  x11cb.aCurSelection  = BadValue;
  x11cb.haveRequest    = true;
  x11cb.type           = type;
  XSetSelectionOwner(x11.display, x11atoms.CLIPBOARD, x11.window, CurrentTime);
  XFlush(x11.display);
  LG_UNLOCK(x11cb.lock);
  if (oldRequest != LG_CLIPBOARD_REQUEST_INVALID)
    app_clipboardAbort(oldRequest);
}

void x11CBRelease(void)
{
  LG_LOCK(x11cb.lock);
  x11cb.haveRequest = false;
  XGrabServer(x11.display);
  if (XGetSelectionOwner(x11.display, x11atoms.CLIPBOARD) == x11.window)
    XSetSelectionOwner(
        x11.display, x11atoms.CLIPBOARD, None, CurrentTime);
  XUngrabServer(x11.display);
  XFlush(x11.display);
  LG_UNLOCK(x11cb.lock);
}

void x11CBRequest(LG_ClipboardRequest request, LG_ClipboardData type)
{
  if (request == LG_CLIPBOARD_REQUEST_INVALID ||
      type < LG_CLIPBOARD_DATA_TEXT || type >= LG_CLIPBOARD_DATA_NONE)
    return;

  LG_LOCK(x11cb.lock);
  const LG_ClipboardRequest oldRequest = x11cb.read.window ?
    cancelReadNL() : LG_CLIPBOARD_REQUEST_INVALID;
  if (x11cb.aCurSelection == BadValue)
  {
    LG_UNLOCK(x11cb.lock);
    if (oldRequest != LG_CLIPBOARD_REQUEST_INVALID)
      app_clipboardAbort(oldRequest);
    app_clipboardAbort(request);
    return;
  }

  const Window window = XCreateSimpleWindow(
      x11.display, x11.window, 0, 0, 1, 1, 0, 0, 0);
  if (!window)
  {
    LG_UNLOCK(x11cb.lock);
    if (oldRequest != LG_CLIPBOARD_REQUEST_INVALID)
      app_clipboardAbort(oldRequest);
    app_clipboardAbort(request);
    return;
  }

  XSelectInput(x11.display, window, PropertyChangeMask);
  x11cb.read = (struct X11ClipboardRead)
  {
    .window  = window,
    .request = request,
    .type    = type,
  };
  XConvertSelection(
      x11.display,
      x11cb.aCurSelection,
      x11cb.aTypes[type],
      x11atoms.SEL_DATA,
      window,
      CurrentTime);
  XFlush(x11.display);
  LG_UNLOCK(x11cb.lock);

  if (oldRequest != LG_CLIPBOARD_REQUEST_INVALID)
    app_clipboardAbort(oldRequest);
}
