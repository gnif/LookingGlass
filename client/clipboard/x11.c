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

#include "lg-clipboard.h"
#include "debug.h"

struct state
{
  Display             * display;
  Window                window;
  Atom                  aSelection;
  Atom                  aTargets;
  Atom                  aTypes[LG_CLIPBOARD_DATA_MAX];
  LG_ClipboardRequestFn requestFn;
  LG_ClipboardData      type;
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

static bool x11_cb_init(SDL_SysWMinfo * wminfo)
{
  // final sanity check
  if (wminfo->subsystem != SDL_SYSWM_X11)
  {
    DEBUG_ERROR("wrong subsystem");
    return false;
  }

  this = (struct state *)malloc(sizeof(struct state));
  memset(this, 0, sizeof(struct state));

  this->display    = wminfo->info.x11.display;
  this->window     = wminfo->info.x11.window;
  this->aSelection = XInternAtom(this->display, "CLIPBOARD", False);
  this->aTargets   = XInternAtom(this->display, "TARGETS"  , False);

  for(int i = 0; i < LG_CLIPBOARD_DATA_MAX; ++i)
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
    for(int i = 0; i < LG_CLIPBOARD_DATA_MAX; ++i)
      if (this->aTypes[i] == e.xselectionrequest.target && this->type == i)
      {
        // request the data
        this->requestFn(x11_cb_reply_fn, s);
        return;
      }

    DEBUG_INFO("Unable to copy \"%s\" to \"%s\" type",
        atomTypes[this->type],
        XGetAtomName(this->display, e.xselectionrequest.target));

    // report no data
    s->xselection.property = None;
    XSendEvent(this->display, e.xselectionrequest.requestor, 0, 0, s);
    XFlush(this->display);
  }

  if (e.type == SelectionNotify)
  {
    DEBUG_WARN("FIXME: SelectionNotify");
    return;
  }

  if (e.type == SelectionClear)
  {
    DEBUG_WARN("FIXME: SelectionClear");
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

const LG_Clipboard LGC_X11 =
{
  .getName = x11_cb_getName,
  .init    = x11_cb_init,
  .free    = x11_cb_free,
  .wmevent = x11_cb_wmevent,
  .notice  = x11_cb_notice
};