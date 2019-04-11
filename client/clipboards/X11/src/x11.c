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

#include "interface/clipboard.h"
#include "common/debug.h"

#include <X11/extensions/Xfixes.h>

struct state
{
  Display             * display;
  Window                window;
  Atom                  aSelection;
  Atom                  aCurSelection;
  Atom                  aTargets;
  Atom                  aSelData;
  Atom                  aIncr;
  Atom                  aTypes[LG_CLIPBOARD_DATA_NONE];
  LG_ClipboardReleaseFn releaseFn;
  LG_ClipboardRequestFn requestFn;
  LG_ClipboardNotifyFn  notifyFn;
  LG_ClipboardDataFn    dataFn;
  LG_ClipboardData      type;

  // XFixes vars
  int eventBase;
  int errorBase;
};

static struct state * this = NULL;

static const char * atomTypes[] =
{
  "UTF8_STRING",
  "image/png",
  "image/bmp",
  "image/tiff",
  "image/jpeg"
};

static const char * x11_cb_getName()
{
  return "X11";
}

static bool x11_cb_init(
    SDL_SysWMinfo         * wminfo,
    LG_ClipboardReleaseFn   releaseFn,
    LG_ClipboardNotifyFn    notifyFn,
    LG_ClipboardDataFn      dataFn)
{
  // final sanity check
  if (wminfo->subsystem != SDL_SYSWM_X11)
  {
    DEBUG_ERROR("wrong subsystem");
    return false;
  }

  this = (struct state *)malloc(sizeof(struct state));
  memset(this, 0, sizeof(struct state));

  this->display       = wminfo->info.x11.display;
  this->window        = wminfo->info.x11.window;
  this->aSelection    = XInternAtom(this->display, "CLIPBOARD", False);
  this->aTargets      = XInternAtom(this->display, "TARGETS"  , False);
  this->aSelData      = XInternAtom(this->display, "SEL_DATA" , False);
  this->aIncr         = XInternAtom(this->display, "INCR"     , False);
  this->aCurSelection = BadValue;
  this->releaseFn     = releaseFn;
  this->notifyFn      = notifyFn;
  this->dataFn        = dataFn;

  for(int i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
  {
    this->aTypes[i] = XInternAtom(this->display, atomTypes[i], False);
    if (this->aTypes[i] == BadAlloc || this->aTypes[i] == BadValue)
    {
      DEBUG_ERROR("failed to get atom for type: %s", atomTypes[i]);
      free(this);
      this = NULL;
      return false;
    }
  }

  // we need the raw X events
  SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);

  // use xfixes to get clipboard change notifications
  if (!XFixesQueryExtension(this->display, &this->eventBase, &this->errorBase))
  {
    DEBUG_ERROR("failed to initialize xfixes");
    free(this);
    this = NULL;
    return false;
  }

  XFixesSelectSelectionInput(this->display, this->window, XA_PRIMARY      , XFixesSetSelectionOwnerNotifyMask);
  XFixesSelectSelectionInput(this->display, this->window, this->aSelection, XFixesSetSelectionOwnerNotifyMask);

  return true;
}

static void x11_cb_free()
{
  free(this);
  this = NULL;
}

static void x11_cb_reply_fn(void * opaque, LG_ClipboardData type, uint8_t * data, uint32_t size)
{
  XEvent *s = (XEvent *)opaque;

  XChangeProperty(
      this->display          ,
      s->xselection.requestor,
      s->xselection.property ,
      s->xselection.target   ,
      8,
      PropModeReplace,
      data,
      size);

  XSendEvent(this->display, s->xselection.requestor, 0, 0, s);
  XFlush(this->display);
  free(s);
}

static void x11_cb_wmevent(SDL_SysWMmsg * msg)
{
  XEvent e = msg->msg.x11.event;

  if (e.type == SelectionRequest)
  {
    XEvent * s = (XEvent *)malloc(sizeof(XEvent));
    s->xselection.type      = SelectionNotify;
    s->xselection.requestor = e.xselectionrequest.requestor;
    s->xselection.selection = e.xselectionrequest.selection;
    s->xselection.target    = e.xselectionrequest.target;
    s->xselection.property  = e.xselectionrequest.property;
    s->xselection.time      = e.xselectionrequest.time;

    if (!this->requestFn)
    {
      s->xselection.property = None;
      XSendEvent(this->display, e.xselectionrequest.requestor, 0, 0, s);
      XFlush(this->display);
      free(s);
      return;
    }

    // target list requested
    if (e.xselectionrequest.target == this->aTargets)
    {
      Atom targets[2];
      targets[0] = this->aTargets;
      targets[1] = this->aTypes[this->type];

      XChangeProperty(
          e.xselectionrequest.display,
          e.xselectionrequest.requestor,
          e.xselectionrequest.property,
          XA_ATOM,
          32,
          PropModeReplace,
					(unsigned char*)targets,
          sizeof(targets) / sizeof(Atom));

      XSendEvent(this->display, e.xselectionrequest.requestor, 0, 0, s);
      XFlush(this->display);
      free(s);
      return;
    }

    // look to see if we can satisfy the data type
    for(int i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
      if (this->aTypes[i] == e.xselectionrequest.target && this->type == i)
      {
        // request the data
        this->requestFn(x11_cb_reply_fn, s);
        return;
      }

    // report no data
    s->xselection.property = None;
    XSendEvent(this->display, e.xselectionrequest.requestor, 0, 0, s);
    XFlush(this->display);
  }

  if (e.type == SelectionClear && (
        e.xselectionclear.selection == XA_PRIMARY ||
        e.xselectionclear.selection == this->aSelection)
     )
  {
    this->aCurSelection = BadValue;
    this->releaseFn();
    return;
  }

  // if someone selected data
  if (e.type == this->eventBase + XFixesSelectionNotify)
  {
    XFixesSelectionNotifyEvent * sne = (XFixesSelectionNotifyEvent *)&e;

    // check if the selection is valid and it isn't ourself
    if (
        (sne->selection != XA_PRIMARY && sne->selection != this->aSelection) ||
        sne->owner == this->window ||
        sne->owner == 0
       )
    {
      return;
    }

    // remember which selection we are working with
    this->aCurSelection = sne->selection;
    XConvertSelection(
        this->display,
        sne->selection,
        this->aTargets,
        this->aTargets,
        this->window,
        CurrentTime);
    return;
  }

  if (e.type == SelectionNotify)
  {
    if (e.xselection.property == None)
      return;

    Atom type;
    int format;
    unsigned long itemCount, after;
    unsigned char *data;

    XGetWindowProperty(
        this->display,
        this->window,
        e.xselection.property,
        0, ~0L, // start and length
        True  , // delete the property
        AnyPropertyType,
        &type,
        &format,
        &itemCount,
        &after,
        &data);

    // the target list
    if (e.xselection.property == this->aTargets)
    {
      // the format is 32-bit and we must have data
      // this is technically incorrect however as it's
      // an array of padded 64-bit values
      if (!data || format != 32)
      {
        if (data)
          XFree(data);
        return;
      }

      // see if we support any of the targets listed
      const uint64_t * targets = (const uint64_t *)data;
      for(unsigned long i = 0; i < itemCount; ++i)
      {
        for(int n = 0; n < LG_CLIPBOARD_DATA_NONE; ++n)
          if (this->aTypes[n] == targets[i])
          {
            // we have a match, so send the notification
            this->notifyFn(n);
            XFree(data);
            return;
          }
      }

      // no matches
      this->notifyFn(LG_CLIPBOARD_DATA_NONE);
      XFree(data);
      return;
    }

    if (format == this->aIncr)
    {
      DEBUG_WARN("fixme: large paste buffers are not yet supported");
      XFree(data);
      return;
    }

    for(int i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
      if (this->aTypes[i] == type)
      {
        this->dataFn(i, data, itemCount);
        XFree(data);
        return;
      }

    DEBUG_WARN("clipboard data (%s) not in a supported format", XGetAtomName(this->display, type));
    XFree(data);
    return;
  }
}

static void x11_cb_notice(LG_ClipboardRequestFn requestFn, LG_ClipboardData type)
{
  this->requestFn = requestFn;
  this->type      = type;
  XSetSelectionOwner(this->display, XA_PRIMARY      , this->window, CurrentTime);
  XSetSelectionOwner(this->display, this->aSelection, this->window, CurrentTime);
  XFlush(this->display);
}

static void x11_cb_release()
{
  this->requestFn = NULL;
  XSetSelectionOwner(this->display, XA_PRIMARY      , None, CurrentTime);
  XSetSelectionOwner(this->display, this->aSelection, None, CurrentTime);
  XFlush(this->display);
}

static void x11_cb_request(LG_ClipboardData type)
{
  if (this->aCurSelection == BadValue)
    return;

  XConvertSelection(
      this->display,
      this->aCurSelection,
      this->aTypes[type],
      this->aSelData,
      this->window,
      CurrentTime);
}

const LG_Clipboard LGC_X11 =
{
  .getName = x11_cb_getName,
  .init    = x11_cb_init,
  .free    = x11_cb_free,
  .wmevent = x11_cb_wmevent,
  .notice  = x11_cb_notice,
  .release = x11_cb_release,
  .request = x11_cb_request
};