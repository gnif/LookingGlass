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

#include "clipboard.h"
#include "x11.h"
#include "atoms.h"

#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "app.h"
#include "common/debug.h"

struct X11ClipboardState
{
  Atom             aCurSelection;
  Atom             aTypes[LG_CLIPBOARD_DATA_NONE];
  LG_ClipboardData type;
  bool             haveRequest;

  bool         incrStart;
  unsigned int lowerBound;
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

bool x11CBEventThread(const XEvent xe)
{
  switch(xe.type)
  {
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
      if (xe.xproperty.state != PropertyNewValue)
        break;

      if (xe.xproperty.atom == x11atoms.SEL_DATA)
      {
        if (x11cb.lowerBound == 0)
          return true;

        x11CBSelectionIncr(xe.xproperty);
        return true;
      }
      break;

    default:
      if (xe.type == x11.eventBase + XFixesSelectionNotify)
      {
        XFixesSelectionNotifyEvent * sne = (XFixesSelectionNotifyEvent *)&xe;
        x11CBXFixesSelectionNotify(*sne);
        return true;
      }
      break;
  }

  return false;
}

bool x11CBInit()
{
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

  if (!x11cb.haveRequest)
    goto nodata;

  // target list requested
  if (e.target == x11atoms.TARGETS)
  {
    Atom targets[2];
    targets[0] = x11atoms.TARGETS;
    targets[1] = x11cb.aTypes[x11cb.type];

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
    if (x11cb.aTypes[i] == e.target && x11cb.type == i)
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
  if (e.selection != x11atoms.CLIPBOARD)
    return;

  x11cb.aCurSelection = BadValue;
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
      x11atoms.INCR,
      &type,
      &format,
      &itemCount,
      &after,
      &data) != Success)
  {
    DEBUG_INFO("GetProp Failed");
    app_clipboardNotifySize(LG_CLIPBOARD_DATA_NONE, 0);
    goto out;
  }

  LG_ClipboardData dataType;
  for(dataType = 0; dataType < LG_CLIPBOARD_DATA_NONE; ++dataType)
    if (x11cb.aTypes[dataType] == type)
      break;

  if (dataType == LG_CLIPBOARD_DATA_NONE)
  {
    DEBUG_WARN("clipboard data (%s) not in a supported format",
        XGetAtomName(x11.display, type));

    x11cb.lowerBound = 0;
    app_clipboardNotifySize(LG_CLIPBOARD_DATA_NONE, 0);
    goto out;
  }

  if (x11cb.incrStart)
  {
    app_clipboardNotifySize(dataType, x11cb.lowerBound);
    x11cb.incrStart = false;
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
    app_clipboardNotifySize(LG_CLIPBOARD_DATA_NONE, 0);
    goto out;
  }

  app_clipboardData(dataType, data, itemCount);
  x11cb.lowerBound -= itemCount;

out:
  if (data)
    XFree(data);
}

static void x11CBXFixesSelectionNotify(const XFixesSelectionNotifyEvent e)
{
  // check if the selection is valid and it isn't ourself
  if (e.selection != x11atoms.CLIPBOARD ||
      e.owner == x11.window || e.owner == 0)
  {
    return;
  }

  // remember which selection we are working with
  x11cb.aCurSelection = e.selection;
  XConvertSelection(
      x11.display,
      e.selection,
      x11atoms.TARGETS,
      x11atoms.TARGETS,
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
    app_clipboardNotifySize(LG_CLIPBOARD_DATA_NONE, 0);
    goto out;
  }

  if (type == x11atoms.INCR)
  {
    x11cb.incrStart  = true;
    x11cb.lowerBound = *(unsigned int *)data;
    goto out;
  }

  // the target list
  if (e.property == x11atoms.TARGETS)
  {
    // the format is 32-bit and we must have data
    // this is technically incorrect however as it's
    // an array of padded 64-bit values
    if (!data || format != 32)
      goto out;

    int typeCount = 0;
    LG_ClipboardData types[itemCount];

    // see if we support any of the targets listed
    const uint64_t * targets = (const uint64_t *)data;
    for(unsigned long i = 0; i < itemCount; ++i)
    {
      for(int n = 0; n < LG_CLIPBOARD_DATA_NONE; ++n)
        if (x11cb.aTypes[n] == targets[i])
          types[typeCount++] = n;
    }

    app_clipboardNotifyTypes(types, typeCount);
    goto out;
  }

  if (e.property == x11atoms.SEL_DATA)
  {
    LG_ClipboardData dataType;
    for(dataType = 0; dataType < LG_CLIPBOARD_DATA_NONE; ++dataType)
      if (x11cb.aTypes[dataType] == type)
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

void x11CBNotice(LG_ClipboardData type)
{
  x11cb.haveRequest = true;
  x11cb.type        = type;
  XSetSelectionOwner(x11.display, x11atoms.CLIPBOARD, x11.window, CurrentTime);
  XFlush(x11.display);
}

void x11CBRelease(void)
{
  x11cb.haveRequest = false;
  XSetSelectionOwner(x11.display, x11atoms.CLIPBOARD, None, CurrentTime);
  XFlush(x11.display);
}

void x11CBRequest(LG_ClipboardData type)
{
  if (x11cb.aCurSelection == BadValue)
    return;

  XConvertSelection(
      x11.display,
      x11cb.aCurSelection,
      x11cb.aTypes[type],
      x11atoms.SEL_DATA,
      x11.window,
      CurrentTime);
}
