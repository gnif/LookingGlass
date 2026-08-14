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

#include <limits.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "core/clipboard.h"
#include "core/clipboard_files.h"
#include "common/array.h"
#include "common/debug.h"
#include "common/KVMFRClipboard.h"
#include "common/locking.h"

struct X11ClipboardRead
{
  Window              window;
  LG_ClipboardRequest request;
  LG_ClipboardData    type;
  bool                incremental;
  bool                begun;
  bool                blocked;
  bool                calling;
  bool                readyPending;
  bool                endPending;
  bool                propertyPending;
  bool                pendingMore;
  uint64_t            sizeHint;
  uint64_t            offset;
  long                propertyOffset;
  unsigned long       pendingUnits;
  uint8_t           * pending;
  size_t              pendingSize;
};

struct X11ClipboardWrite
{
  struct X11ClipboardWrite * next;
  XEvent                     event;
  LG_ClipboardRequest        request;
  LG_ClipboardData           type;
  uint64_t                   offset;
  bool                       begun;
  bool                       blocked;
  bool                       ready;
  bool                       endPending;
  bool                       creating;
  bool                       file;
  uint8_t                  * fileData;
  size_t                     fileSize;
  uint64_t                   filePresentation;
};

struct X11ClipboardFileImport
{
  Window        window;
  Atom          target;
  const char  * mime;
  long          propertyOffset;
  uint8_t     * data;
  size_t        size;
  size_t        capacity;
  bool          incremental;
};

struct X11ClipboardState
{
  LG_Lock                       lock;
  Atom                          aCurSelection;
  Atom                          aTypes[LG_CLIPBOARD_DATA_NONE];
  Atom                          aFileGnome;
  Atom                          aFileKde;
  Window                        targetsWindow;
  LG_ClipboardData              type;
  bool                          haveRequest;
  uint64_t                      filePresentation;

  struct X11ClipboardRead       read;
  struct X11ClipboardWrite    * writes;
  struct X11ClipboardFileImport fileImport;
};

static const char * atomTypes[] =
{
  "UTF8_STRING",
  "image/png",
  "image/bmp",
  "image/tiff",
  "image/jpeg",
  "text/uri-list",
};

static const char fileGnomeMime[] = "x-special/gnome-copied-files";
static const char fileKdeMime[]   = "application/x-kde-cutselection";
static const char fileUriMime[]   = "text/uri-list";

static struct X11ClipboardState x11cb;

// forwards
static void x11CBSelectionRequest(const XSelectionRequestEvent e);
static void x11CBSelectionClear(const XSelectionClearEvent e);
static void x11CBSelectionIncr(const XPropertyEvent e);
static void x11CBSelectionNotify(const XSelectionEvent e);
static void x11CBXFixesSelectionNotify(const XFixesSelectionNotifyEvent e);
static LG_ClipboardRequest cancelReadNL(bool abort);
static bool writeLinkedNL(const struct X11ClipboardWrite * write);
static void writeRemoveNL(struct X11ClipboardWrite * write);
static bool advanceReadPropertyNL(unsigned long units);
static void clearTargetsNL(void);
static void clearFileImportNL(void);
static struct X11ClipboardWrite * takeFileWritesNL(void);
static struct X11ClipboardWrite * takeFileWritesForWindowNL(Window window);
static void cancelFileWrites(
    struct X11ClipboardWrite * writes, bool terminate);
static void x11CBFileImportIncr(const XPropertyEvent e);

static const char * fileMimeForAtom(Atom atom)
{
  if (atom == x11cb.aTypes[LG_CLIPBOARD_DATA_FILES])
    return fileUriMime;
  if (atom == x11cb.aFileGnome)
    return fileGnomeMime;
  if (atom == x11cb.aFileKde)
    return fileKdeMime;
  return NULL;
}

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

    case DestroyNotify:
    {
      LG_LOCK(x11cb.lock);
      struct X11ClipboardWrite * writes =
        takeFileWritesForWindowNL(xe->xdestroywindow.window);
      LG_UNLOCK(x11cb.lock);
      if (!writes)
        return false;
      cancelFileWrites(writes, false);
      return true;
    }

    case PropertyNotify:
      if (xe->xproperty.state == PropertyNewValue &&
          xe->xproperty.atom == x11atoms.SEL_DATA)
      {
        LG_LOCK(x11cb.lock);
        const bool fileImport = x11cb.fileImport.window &&
          xe->xproperty.window == x11cb.fileImport.window;
        LG_UNLOCK(x11cb.lock);
        if (fileImport)
          x11CBFileImportIncr(xe->xproperty);
        else
          x11CBSelectionIncr(xe->xproperty);
        return true;
      }
      if (xe->xproperty.state == PropertyDelete)
      {
        LG_ClipboardRequest request = LG_CLIPBOARD_REQUEST_INVALID;
        LG_LOCK(x11cb.lock);
        struct X11ClipboardWrite * write;
        for (write = x11cb.writes; write; write = write->next)
          if (xe->xproperty.window == write->event.xselection.requestor &&
              xe->xproperty.atom   == write->event.xselection.property)
            break;
        if (write)
        {
          if (write->file)
          {
            const size_t remaining = write->fileSize - (size_t)write->offset;
            if (remaining)
            {
              const size_t chunk =
                remaining > KVMFR_CLIPBOARD_REPRESENTATION_BYTES ?
                  KVMFR_CLIPBOARD_REPRESENTATION_BYTES : remaining;
              XChangeProperty(x11.display,
                  write->event.xselection.requestor,
                  write->event.xselection.property,
                  write->event.xselection.target, 8, PropModeReplace,
                  write->fileData + (size_t)write->offset, (int)chunk);
              write->offset += chunk;
              XFlush(x11.display);
              LG_UNLOCK(x11cb.lock);
              return true;
            }

            XChangeProperty(x11.display,
                write->event.xselection.requestor,
                write->event.xselection.property,
                write->event.xselection.target, 8, PropModeReplace,
                NULL, 0);
            XFlush(x11.display);
            if (write->event.xselection.target != x11cb.aFileKde)
              lgClipboardFiles_remotePresentationDelivered(
                  write->filePresentation);
            const uint64_t presentation = write->filePresentation;
            writeRemoveNL(write);
            LG_UNLOCK(x11cb.lock);
            lgClipboardFiles_remotePresentationRelease(presentation);
            free(write->fileData);
            free(write);
            return true;
          }

          write->ready = true;
          if (write->blocked)
          {
            write->blocked = false;
            request = write->request;
          }
        }
        LG_UNLOCK(x11cb.lock);
        if (write)
        {
          if (request != LG_CLIPBOARD_REQUEST_INVALID)
            lgClipboard_requestReady(request);
          return true;
        }
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
  x11cb.read.request  = LG_CLIPBOARD_REQUEST_INVALID;
  x11cb.writes        = NULL;
  for(int i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
  {
    x11cb.aTypes[i] = XInternAtom(x11.display, atomTypes[i], False);
    if (x11cb.aTypes[i] == BadAlloc || x11cb.aTypes[i] == BadValue)
    {
      DEBUG_ERROR("failed to get atom for type: %s", atomTypes[i]);
      return false;
    }
  }

  x11cb.aFileGnome = XInternAtom(x11.display, fileGnomeMime, False);
  x11cb.aFileKde   = XInternAtom(x11.display, fileKdeMime, False);
  if (x11cb.aFileGnome == BadAlloc || x11cb.aFileGnome == BadValue ||
      x11cb.aFileKde == BadAlloc || x11cb.aFileKde == BadValue)
  {
    DEBUG_ERROR("failed to get clipboard file atoms");
    return false;
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

static void x11CBWriteSend(XEvent * event)
{
  XSendEvent(x11.display, event->xselection.requestor, 0, 0, event);
  XFlush(x11.display);
}

static LG_ClipboardResult x11CBWriteBegin(void * opaque,
    LG_ClipboardData type, uint64_t sizeHint)
{
  struct X11ClipboardWrite * write = opaque;
  LG_LOCK(x11cb.lock);
  if (!writeLinkedNL(write) ||
      write->request == LG_CLIPBOARD_REQUEST_INVALID ||
      write->begun || type != write->type)
  {
    LG_UNLOCK(x11cb.lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  const unsigned long hint = sizeHint == LG_CLIPBOARD_SIZE_UNKNOWN ? 0 :
    (sizeHint > ULONG_MAX ? ULONG_MAX : (unsigned long)sizeHint);
  XChangeProperty(x11.display, write->event.xselection.requestor,
      write->event.xselection.property, x11atoms.INCR, 32,
      PropModeReplace, (const unsigned char *)&hint, 1);
  XSelectInput(x11.display, write->event.xselection.requestor,
      PropertyChangeMask);
  write->begun = true;
  write->ready = false;
  XEvent event = write->event;
  LG_UNLOCK(x11cb.lock);
  x11CBWriteSend(&event);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult x11CBWriteChunk(void * opaque,
    uint64_t offset, const void * data, size_t size)
{
  struct X11ClipboardWrite * write = opaque;
  LG_LOCK(x11cb.lock);
  if (!writeLinkedNL(write) ||
      write->request == LG_CLIPBOARD_REQUEST_INVALID || !write->begun ||
      write->endPending || !data || !size)
  {
    LG_UNLOCK(x11cb.lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (!write->ready)
  {
    write->blocked = true;
    LG_UNLOCK(x11cb.lock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }
  if (offset != write->offset ||
      size > KVMFR_CLIPBOARD_REPRESENTATION_BYTES)
  {
    LG_UNLOCK(x11cb.lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  XChangeProperty(x11.display, write->event.xselection.requestor,
      write->event.xselection.property, write->event.xselection.target,
      8, PropModeReplace, data, (int)size);
  write->ready   = false;
  write->blocked = false;
  write->offset  += size;
  LG_UNLOCK(x11cb.lock);
  XFlush(x11.display);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult x11CBWriteEnd(void * opaque, uint64_t finalSize)
{
  struct X11ClipboardWrite * write = opaque;
  LG_LOCK(x11cb.lock);
  if (!writeLinkedNL(write) ||
      write->request == LG_CLIPBOARD_REQUEST_INVALID || !write->begun)
  {
    LG_UNLOCK(x11cb.lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (!write->ready)
  {
    write->blocked    = true;
    write->endPending = true;
    LG_UNLOCK(x11cb.lock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }
  if (finalSize != write->offset)
  {
    LG_UNLOCK(x11cb.lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  const XEvent event = write->event;
  const bool destroy = !write->creating;
  writeRemoveNL(write);
  LG_UNLOCK(x11cb.lock);
  XChangeProperty(x11.display, event.xselection.requestor,
      event.xselection.property, event.xselection.target, 8,
      PropModeReplace, NULL, 0);
  XFlush(x11.display);
  if (destroy)
    free(write);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static void x11CBWriteCancel(void * opaque,
    LG_ClipboardCancelReason reason)
{
  struct X11ClipboardWrite * write = opaque;
  (void)reason;
  LG_LOCK(x11cb.lock);
  if (!writeLinkedNL(write))
  {
    LG_UNLOCK(x11cb.lock);
    return;
  }
  const XEvent event = write->event;
  const bool begun = write->begun;
  const bool destroy = !write->creating;
  writeRemoveNL(write);
  LG_UNLOCK(x11cb.lock);
  if (begun)
  {
    XChangeProperty(x11.display, event.xselection.requestor,
        event.xselection.property, event.xselection.target, 8,
        PropModeReplace, NULL, 0);
    XFlush(x11.display);
  }
  else
  {
    XEvent reply = event;
    reply.xselection.property = None;
    x11CBWriteSend(&reply);
  }
  if (destroy)
    free(write);
}

static const LG_ClipboardStreamOps x11CBWriteStream =
{
  .begin  = x11CBWriteBegin,
  .chunk  = x11CBWriteChunk,
  .end    = x11CBWriteEnd,
  .cancel = x11CBWriteCancel,
};

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
    Atom targets[4];
    targets[0] = x11atoms.TARGETS;
    targets[1] = x11cb.aTypes[requestType];
    int count = 2;
    if (requestType == LG_CLIPBOARD_DATA_FILES)
    {
      targets[count++] = x11cb.aFileGnome;
      targets[count++] = x11cb.aFileKde;
    }

    XChangeProperty(
      e.display,
      e.requestor,
      e.property,
      XA_ATOM,
      32,
      PropModeReplace,
      (unsigned char*)targets,
      count
    );

    goto send;
  }

  const char * fileMime = fileMimeForAtom(e.target);
  if (requestType == LG_CLIPBOARD_DATA_FILES && fileMime)
  {
    char * data = NULL;
    size_t size = 0;
    uint64_t writePresentation = 0;
    LG_LOCK(x11cb.lock);
    const uint64_t presentation = x11cb.filePresentation;
    bool available = x11cb.haveRequest &&
      x11cb.type == LG_CLIPBOARD_DATA_FILES && presentation;
    if (available)
    {
      writePresentation =
        lgClipboardFiles_remotePresentationAcquire();
      available = writePresentation == presentation &&
        lgClipboardFiles_getRemotePresentation(writePresentation,
          fileMime, &data, &size);
    }
    LG_UNLOCK(x11cb.lock);
    if (!available)
    {
      free(data);
      if (writePresentation)
        lgClipboardFiles_remotePresentationRelease(writePresentation);
      goto nodata;
    }

    struct X11ClipboardWrite * write = calloc(1, sizeof(*write));
    if (!write)
    {
      free(data);
      lgClipboardFiles_remotePresentationRelease(writePresentation);
      DEBUG_ERROR("out of memory");
      goto nodata;
    }
    write->event            = *s;
    write->request          = LG_CLIPBOARD_REQUEST_INVALID;
    write->type             = LG_CLIPBOARD_DATA_FILES;
    write->file             = true;
    write->fileData         = (uint8_t *)data;
    write->fileSize         = size;
    write->filePresentation = writePresentation;
    write->begun            = true;

    LG_LOCK(x11cb.lock);
    bool duplicate = false;
    for (struct X11ClipboardWrite * current = x11cb.writes;
        current; current = current->next)
    {
      duplicate = current->event.xselection.requestor == e.requestor &&
        current->event.xselection.property            == e.property;
      if (duplicate)
        break;
    }
    if (!duplicate && x11cb.haveRequest &&
        x11cb.type == LG_CLIPBOARD_DATA_FILES &&
        x11cb.filePresentation == presentation)
    {
      write->next  = x11cb.writes;
      x11cb.writes = write;
      const unsigned long hint = size > UINT32_MAX ?
        UINT32_MAX : (unsigned long)size;
      XChangeProperty(x11.display, e.requestor, e.property,
          x11atoms.INCR, 32, PropModeReplace,
          (const unsigned char *)&hint, 1);
      XSelectInput(x11.display, e.requestor,
          PropertyChangeMask | StructureNotifyMask);
    }
    else
      duplicate = true;
    LG_UNLOCK(x11cb.lock);

    if (duplicate)
    {
      free(write->fileData);
      lgClipboardFiles_remotePresentationRelease(
          write->filePresentation);
      free(write);
      goto nodata;
    }

    x11CBWriteSend(s);
    free(s);
    return;
  }

  // look to see if we can satisfy the data type
  for(int i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
    if (x11cb.aTypes[i] == e.target && requestType == i)
    {
      struct X11ClipboardWrite * write = calloc(1, sizeof(*write));
      if (!write)
      {
        DEBUG_ERROR("out of memory");
        goto nodata;
      }
      write->event    = *s;
      write->request  = LG_CLIPBOARD_REQUEST_INVALID;
      write->type     = requestType;
      write->creating = true;

      LG_LOCK(x11cb.lock);
      bool duplicate = false;
      for (struct X11ClipboardWrite * current = x11cb.writes;
          current; current = current->next)
      {
        duplicate = current->event.xselection.requestor == e.requestor &&
          current->event.xselection.property            == e.property;
        if (duplicate)
          break;
      }
      if (!duplicate)
      {
        write->next   = x11cb.writes;
        x11cb.writes = write;
      }
      LG_UNLOCK(x11cb.lock);
      if (duplicate)
      {
        free(write);
        goto nodata;
      }

      const bool result = lgClipboard_requestStream(requestType,
          &x11CBWriteStream, write, &write->request);
      LG_LOCK(x11cb.lock);
      const bool linked = writeLinkedNL(write);
      if (linked)
      {
        write->creating = false;
        if (!result)
          writeRemoveNL(write);
      }
      else
        write->creating = false;
      LG_UNLOCK(x11cb.lock);

      if (result && linked)
      {
        free(s);
        return;
      }
      free(write);
      if (!linked)
      {
        free(s);
        return;
      }
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
  if (e.selection != x11atoms.CLIPBOARD)
    return;

  LG_LOCK(x11cb.lock);
  if (XGetSelectionOwner(x11.display, x11atoms.CLIPBOARD) == x11.window)
  {
    LG_UNLOCK(x11cb.lock);
    return;
  }
  x11cb.haveRequest = false;
  const uint64_t presentation = x11cb.filePresentation;
  x11cb.filePresentation = 0;
  LG_UNLOCK(x11cb.lock);

  if (presentation)
    lgClipboardFiles_remotePresentationRelease(presentation);
}

/* x11cb.lock must be held. */
static struct X11ClipboardRead clearReadNL(void)
{
  const struct X11ClipboardRead read = x11cb.read;
  x11cb.read = (struct X11ClipboardRead)
  {
    .request = LG_CLIPBOARD_REQUEST_INVALID,
  };
  if (read.window)
    XDestroyWindow(x11.display, read.window);
  return read;
}

/* x11cb.lock must be held. */
static LG_ClipboardRequest cancelReadNL(bool abort)
{
  const struct X11ClipboardRead read = clearReadNL();
  free(read.pending);
  return abort ? read.request : LG_CLIPBOARD_REQUEST_INVALID;
}

/* x11cb.lock must be held. */
static bool writeLinkedNL(const struct X11ClipboardWrite * write)
{
  for (const struct X11ClipboardWrite * current = x11cb.writes;
      current; current = current->next)
    if (current == write)
      return true;
  return false;
}

/* x11cb.lock must be held and write must be linked. */
static void writeRemoveNL(struct X11ClipboardWrite * write)
{
  struct X11ClipboardWrite ** current = &x11cb.writes;
  while (*current != write)
    current = &(*current)->next;
  *current = write->next;
  write->next = NULL;
}

/* x11cb.lock must be held. */
static bool advanceReadPropertyNL(unsigned long units)
{
  if (units > LONG_MAX ||
      x11cb.read.propertyOffset > LONG_MAX - (long)units)
    return false;
  x11cb.read.propertyOffset += (long)units;
  return true;
}

/* x11cb.lock must be held. */
static void clearTargetsNL(void)
{
  if (!x11cb.targetsWindow)
    return;

  XDestroyWindow(x11.display, x11cb.targetsWindow);
  x11cb.targetsWindow = 0;
}

/* x11cb.lock must be held. */
static struct X11ClipboardFileImport takeFileImportNL(void)
{
  const struct X11ClipboardFileImport import = x11cb.fileImport;
  x11cb.fileImport = (struct X11ClipboardFileImport) { 0 };
  if (import.window)
    XDestroyWindow(x11.display, import.window);
  return import;
}

/* x11cb.lock must be held. */
static void clearFileImportNL(void)
{
  const struct X11ClipboardFileImport import = takeFileImportNL();
  free(import.data);
}

/* x11cb.lock must be held. */
static struct X11ClipboardWrite * takeFileWritesNL(void)
{
  struct X11ClipboardWrite * result = NULL;
  struct X11ClipboardWrite ** link = &x11cb.writes;
  while (*link)
  {
    struct X11ClipboardWrite * write = *link;
    if (!write->file)
    {
      link = &write->next;
      continue;
    }

    *link = write->next;
    write->next = result;
    result = write;
  }
  return result;
}

/* x11cb.lock must be held. */
static struct X11ClipboardWrite * takeFileWritesForWindowNL(Window window)
{
  struct X11ClipboardWrite * result = NULL;
  struct X11ClipboardWrite ** link = &x11cb.writes;
  while (*link)
  {
    struct X11ClipboardWrite * write = *link;
    if (!write->file ||
        write->event.xselection.requestor != window)
    {
      link = &write->next;
      continue;
    }

    *link = write->next;
    write->next = result;
    result = write;
  }
  return result;
}

static void cancelFileWrites(
    struct X11ClipboardWrite * writes, bool terminate)
{
  const bool flush = terminate && writes;
  while (writes)
  {
    struct X11ClipboardWrite * next = writes->next;
    if (terminate)
      XChangeProperty(x11.display, writes->event.xselection.requestor,
          writes->event.xselection.property,
          writes->event.xselection.target, 8, PropModeReplace, NULL, 0);
    lgClipboardFiles_remotePresentationRelease(
        writes->filePresentation);
    free(writes->fileData);
    free(writes);
    writes = next;
  }
  if (flush)
    XFlush(x11.display);
}

static bool fileImportGrowNL(size_t wanted)
{
  if (wanted <= x11cb.fileImport.capacity)
    return true;
  size_t capacity = x11cb.fileImport.capacity ?
    x11cb.fileImport.capacity : 4096U;
  while (capacity < wanted)
  {
    if (capacity > SIZE_MAX / 2U)
    {
      capacity = wanted;
      break;
    }
    capacity *= 2U;
  }
  uint8_t * data = realloc(x11cb.fileImport.data, capacity);
  if (!data)
    return false;
  x11cb.fileImport.data     = data;
  x11cb.fileImport.capacity = capacity;
  return true;
}

static bool startFileImport(Atom target)
{
  const char * mime = fileMimeForAtom(target);
  if (!mime)
    return false;

  LG_LOCK(x11cb.lock);
  clearFileImportNL();
  if (x11cb.aCurSelection == BadValue)
  {
    LG_UNLOCK(x11cb.lock);
    return false;
  }

  const Window window = XCreateSimpleWindow(
      x11.display, x11.window, 0, 0, 1, 1, 0, 0, 0);
  if (!window)
  {
    LG_UNLOCK(x11cb.lock);
    return false;
  }

  x11cb.fileImport = (struct X11ClipboardFileImport)
  {
    .window = window,
    .target = target,
    .mime   = mime,
  };
  XSelectInput(x11.display, window, PropertyChangeMask);
  XConvertSelection(x11.display, x11cb.aCurSelection, target,
      x11atoms.SEL_DATA, window, CurrentTime);
  XFlush(x11.display);
  LG_UNLOCK(x11cb.lock);
  return true;
}

static void finishFileImport(struct X11ClipboardFileImport import,
    const void * data, size_t size)
{
  if (!lgClipboardFiles_setLocal(import.mime, data, size))
  {
    lgClipboardFiles_clearLocal();
    lgClipboard_release();
  }
  free(import.data);
}

static bool x11CBFileImportSelectionNotify(const XSelectionEvent e)
{
  Atom type;
  int format;
  unsigned long itemCount;
  unsigned long after;
  unsigned char * data = NULL;

  LG_LOCK(x11cb.lock);
  if (!x11cb.fileImport.window || e.requestor != x11cb.fileImport.window)
  {
    LG_UNLOCK(x11cb.lock);
    return false;
  }

  if (e.target != x11cb.fileImport.target ||
      e.property != x11atoms.SEL_DATA ||
      XGetWindowProperty(e.display, e.requestor, e.property, 0, ~0L,
        True, AnyPropertyType, &type, &format, &itemCount, &after,
        &data) != Success)
  {
    const struct X11ClipboardFileImport failed = takeFileImportNL();
    LG_UNLOCK(x11cb.lock);
    if (data)
      XFree(data);
    free(failed.data);
    lgClipboard_release();
    return true;
  }

  if (type == x11atoms.INCR)
  {
    if (!data || format != 32 || itemCount < 1 || after)
    {
      const struct X11ClipboardFileImport failed = takeFileImportNL();
      LG_UNLOCK(x11cb.lock);
      if (data)
        XFree(data);
      free(failed.data);
      lgClipboard_release();
      return true;
    }
    x11cb.fileImport.incremental = true;
    XFree(data);
    LG_UNLOCK(x11cb.lock);
    return true;
  }

  const bool valid = type == x11cb.fileImport.target && format == 8 &&
    !after && (!itemCount || data);
  const struct X11ClipboardFileImport import = takeFileImportNL();
  LG_UNLOCK(x11cb.lock);
  if (valid)
    finishFileImport(import, data, itemCount);
  else
  {
    free(import.data);
    lgClipboard_release();
  }
  if (data)
    XFree(data);
  return true;
}

static void x11CBFileImportIncr(const XPropertyEvent e)
{
  Atom type;
  int format;
  unsigned long itemCount;
  unsigned long after;
  unsigned char * data = NULL;

  LG_LOCK(x11cb.lock);
  if (!x11cb.fileImport.window || !x11cb.fileImport.incremental ||
      e.window != x11cb.fileImport.window || e.atom != x11atoms.SEL_DATA)
  {
    LG_UNLOCK(x11cb.lock);
    return;
  }

readProperty:
  if (XGetWindowProperty(e.display, e.window, e.atom,
      x11cb.fileImport.propertyOffset,
      (KVMFR_CLIPBOARD_REPRESENTATION_BYTES + 3U) / 4U,
      True, AnyPropertyType, &type, &format, &itemCount, &after,
      &data) != Success || (itemCount && !data) || (!itemCount && after) ||
      type != x11cb.fileImport.target || format != 8 ||
      itemCount > SIZE_MAX ||
      (size_t)itemCount > SIZE_MAX - x11cb.fileImport.size ||
      !fileImportGrowNL(x11cb.fileImport.size + (size_t)itemCount))
  {
    const struct X11ClipboardFileImport failed = takeFileImportNL();
    LG_UNLOCK(x11cb.lock);
    if (data)
      XFree(data);
    free(failed.data);
    lgClipboard_release();
    return;
  }

  if (itemCount)
  {
    memcpy(x11cb.fileImport.data + x11cb.fileImport.size,
        data, (size_t)itemCount);
    x11cb.fileImport.size += (size_t)itemCount;
  }
  if (data)
  {
    XFree(data);
    data = NULL;
  }

  if (after)
  {
    const unsigned long units = itemCount / 4U +
      (itemCount % 4U != 0);
    if (units > LONG_MAX ||
        x11cb.fileImport.propertyOffset > LONG_MAX - (long)units)
    {
      const struct X11ClipboardFileImport failed = takeFileImportNL();
      LG_UNLOCK(x11cb.lock);
      free(failed.data);
      lgClipboard_release();
      return;
    }
    x11cb.fileImport.propertyOffset += (long)units;
    goto readProperty;
  }

  x11cb.fileImport.propertyOffset = 0;
  if (itemCount)
  {
    LG_UNLOCK(x11cb.lock);
    return;
  }

  const struct X11ClipboardFileImport import = takeFileImportNL();
  LG_UNLOCK(x11cb.lock);
  finishFileImport(import, import.data, import.size);
}

enum X11ClipboardReadOperation
{
  X11_CLIPBOARD_READ_BEGIN,
  X11_CLIPBOARD_READ_CHUNK,
  X11_CLIPBOARD_READ_END,
};

enum X11ClipboardReadCallResult
{
  X11_CLIPBOARD_READ_CALL_STALE,
  X11_CLIPBOARD_READ_CALL_ACCEPTED,
  X11_CLIPBOARD_READ_CALL_BLOCKED,
  X11_CLIPBOARD_READ_CALL_RETRY,
  X11_CLIPBOARD_READ_CALL_FAILED,
};

/* x11cb.lock must be held on entry and is held again on return. Retain a ready
 * edge delivered between the provider operation and committing BLOCKED. */
static enum X11ClipboardReadCallResult x11CBReadCallNL(
    enum X11ClipboardReadOperation operation, LG_ClipboardData type,
    uint64_t sizeHint, uint64_t offset, const void * data, size_t size)
{
  const LG_ClipboardRequest request = x11cb.read.request;
  x11cb.read.calling      = true;
  x11cb.read.readyPending = false;
  LG_UNLOCK(x11cb.lock);

  LG_ClipboardResult result;
  switch (operation)
  {
    case X11_CLIPBOARD_READ_BEGIN:
      result = lgClipboard_dataBegin(request, type, sizeHint);
      break;

    case X11_CLIPBOARD_READ_CHUNK:
      result = lgClipboard_dataChunk(request, offset, data, size);
      break;

    case X11_CLIPBOARD_READ_END:
      result = lgClipboard_dataEnd(request, offset);
      break;

    default:
      result = LG_CLIPBOARD_RESULT_FAILED;
      break;
  }

  LG_LOCK(x11cb.lock);
  if (x11cb.read.request != request)
    return X11_CLIPBOARD_READ_CALL_STALE;

  x11cb.read.calling = false;
  if (result == LG_CLIPBOARD_RESULT_BLOCKED)
  {
    if (x11cb.read.readyPending)
    {
      x11cb.read.readyPending = false;
      return X11_CLIPBOARD_READ_CALL_RETRY;
    }

    x11cb.read.blocked = true;
    return X11_CLIPBOARD_READ_CALL_BLOCKED;
  }

  x11cb.read.readyPending = false;
  return result == LG_CLIPBOARD_RESULT_ACCEPTED ?
    X11_CLIPBOARD_READ_CALL_ACCEPTED : X11_CLIPBOARD_READ_CALL_FAILED;
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

  if (x11cb.read.blocked || x11cb.read.calling || x11cb.read.pendingSize ||
      x11cb.read.endPending)
  {
    x11cb.read.propertyPending = true;
    LG_UNLOCK(x11cb.lock);
    return;
  }

readProperty:
  if (XGetWindowProperty(
      e.display,
      e.window,
      e.atom,
      x11cb.read.propertyOffset,
      (KVMFR_CLIPBOARD_REPRESENTATION_BYTES + 3U) / 4U,
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
    free(read.pending);
    lgClipboard_abort(read.request);
    return;
  }

  LG_ClipboardData dataType;
  for(dataType = 0; dataType < LG_CLIPBOARD_DATA_NONE; ++dataType)
    if (x11cb.aTypes[dataType] == type)
      break;

  if ((itemCount && !data) || (!itemCount && after) ||
      dataType == LG_CLIPBOARD_DATA_NONE ||
      dataType != x11cb.read.type || format != 8)
  {
    DEBUG_WARN("Invalid incremental clipboard data");
    const struct X11ClipboardRead read = clearReadNL();
    LG_UNLOCK(x11cb.lock);
    if (data)
      XFree(data);
    free(read.pending);
    lgClipboard_abort(read.request);
    return;
  }

  if (itemCount == 0)
  {
    if (data)
      XFree(data);
    const uint64_t offset = x11cb.read.offset;
    enum X11ClipboardReadCallResult result;
    do
      result = x11CBReadCallNL(X11_CLIPBOARD_READ_END,
          x11cb.read.type, x11cb.read.sizeHint, offset, NULL, 0);
    while (result == X11_CLIPBOARD_READ_CALL_RETRY);
    if (result == X11_CLIPBOARD_READ_CALL_STALE)
    {
      LG_UNLOCK(x11cb.lock);
      return;
    }
    if (result == X11_CLIPBOARD_READ_CALL_BLOCKED)
    {
      x11cb.read.endPending = true;
      LG_UNLOCK(x11cb.lock);
      return;
    }
    cancelReadNL(false);
    LG_UNLOCK(x11cb.lock);
    return;
  }

  if (itemCount > KVMFR_CLIPBOARD_REPRESENTATION_BYTES)
  {
    const struct X11ClipboardRead read = clearReadNL();
    LG_UNLOCK(x11cb.lock);
    XFree(data);
    free(read.pending);
    lgClipboard_abort(read.request);
    return;
  }

  uint8_t * copy = malloc(itemCount);
  if (!copy)
  {
    const struct X11ClipboardRead read = clearReadNL();
    LG_UNLOCK(x11cb.lock);
    XFree(data);
    free(read.pending);
    lgClipboard_abort(read.request);
    return;
  }
  memcpy(copy, data, itemCount);
  XFree(data);
  data = NULL;

  const uint64_t offset = x11cb.read.offset;
  enum X11ClipboardReadCallResult result;
  do
    result = x11CBReadCallNL(X11_CLIPBOARD_READ_CHUNK,
        x11cb.read.type, x11cb.read.sizeHint,
        offset, copy, itemCount);
  while (result == X11_CLIPBOARD_READ_CALL_RETRY);
  if (result == X11_CLIPBOARD_READ_CALL_STALE)
  {
    LG_UNLOCK(x11cb.lock);
    free(copy);
    return;
  }
  if (result == X11_CLIPBOARD_READ_CALL_BLOCKED)
  {
    x11cb.read.pending      = copy;
    x11cb.read.pendingSize  = itemCount;
    x11cb.read.pendingUnits = (itemCount + 3U) / 4U;
    x11cb.read.pendingMore  = after != 0;
    LG_UNLOCK(x11cb.lock);
    return;
  }
  free(copy);
  if (result != X11_CLIPBOARD_READ_CALL_ACCEPTED)
  {
    cancelReadNL(false);
    LG_UNLOCK(x11cb.lock);
    return;
  }
  x11cb.read.offset += itemCount;
  if (after)
  {
    if (!advanceReadPropertyNL((itemCount + 3U) / 4U))
    {
      const struct X11ClipboardRead read = clearReadNL();
      LG_UNLOCK(x11cb.lock);
      free(read.pending);
      lgClipboard_abort(read.request);
      return;
    }
    goto readProperty;
  }
  x11cb.read.propertyOffset = 0;
  LG_UNLOCK(x11cb.lock);
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
    cancelReadNL(true) : LG_CLIPBOARD_REQUEST_INVALID;
  clearTargetsNL();
  clearFileImportNL();
  x11cb.haveRequest = false;
  const uint64_t presentation = x11cb.filePresentation;
  x11cb.filePresentation = 0;

  if (e.owner == 0)
  {
    x11cb.aCurSelection = BadValue;
    XUngrabServer(x11.display);
    XFlush(x11.display);
    LG_UNLOCK(x11cb.lock);
    if (oldRequest != LG_CLIPBOARD_REQUEST_INVALID)
      lgClipboard_abort(oldRequest);
    if (presentation)
      lgClipboardFiles_remotePresentationRelease(presentation);
    lgClipboardFiles_clearLocal();
    lgClipboard_release();
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
      lgClipboard_abort(oldRequest);
    if (presentation)
      lgClipboardFiles_remotePresentationRelease(presentation);
    lgClipboardFiles_clearLocal();
    lgClipboard_release();
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
    lgClipboard_abort(oldRequest);
  if (presentation)
    lgClipboardFiles_remotePresentationRelease(presentation);
  lgClipboardFiles_clearLocal();
}

static void x11CBSelectionNotify(const XSelectionEvent e)
{
  Atom type;
  int format;
  unsigned long itemCount;
  unsigned long after;
  unsigned char * data = NULL;

  if (x11CBFileImportSelectionNotify(e))
    return;

  LG_LOCK(x11cb.lock);
  const bool targetReply = x11cb.targetsWindow &&
    e.requestor == x11cb.targetsWindow && e.target == x11atoms.TARGETS;
  if (targetReply)
  {
    if (e.property == None)
    {
      clearTargetsNL();
      LG_UNLOCK(x11cb.lock);
      lgClipboard_release();
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
      lgClipboard_release();
      return;
    }
    clearTargetsNL();
    LG_UNLOCK(x11cb.lock);

    // the format is 32-bit and we must have data
    // this is technically incorrect however as it's
    // an array of padded 64-bit values
    if (!data || format != 32)
    {
      lgClipboard_release();
      goto out;
    }

    size_t typeCount = 0;
    LG_ClipboardData types[LG_CLIPBOARD_DATA_NONE];

    // see if we support any of the targets listed
    const Atom * targets = (const Atom *)data;
    Atom fileTarget = None;
    for (unsigned long i = 0; i < itemCount; ++i)
      if (targets[i] == x11cb.aFileGnome)
      {
        fileTarget = x11cb.aFileGnome;
        break;
      }
    if (fileTarget == None)
      for (unsigned long i = 0; i < itemCount; ++i)
        if (targets[i] == x11cb.aTypes[LG_CLIPBOARD_DATA_FILES])
        {
          fileTarget = x11cb.aTypes[LG_CLIPBOARD_DATA_FILES];
          break;
        }

    if (fileTarget != None)
    {
      lgClipboardFiles_clearLocal();
      if (!startFileImport(fileTarget))
        lgClipboard_release();
      goto out;
    }

    for(int n = 0; n < LG_CLIPBOARD_DATA_NONE; ++n)
      for(unsigned long i = 0; i < itemCount; ++i)
        if (x11cb.aTypes[n] == targets[i])
        {
          types[typeCount++] = n;
          break;
        }

    lgClipboard_notifyTypes(types, typeCount);
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
    const LG_ClipboardRequest request = cancelReadNL(true);
    LG_UNLOCK(x11cb.lock);
    lgClipboard_abort(request);
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
    const LG_ClipboardRequest request = cancelReadNL(true);
    LG_UNLOCK(x11cb.lock);
    lgClipboard_abort(request);
    return;
  }

  if (type == x11atoms.INCR)
  {
    if (!data || format != 32 || itemCount < 1)
    {
      const LG_ClipboardRequest request = cancelReadNL(true);
      LG_UNLOCK(x11cb.lock);
      if (data)
        XFree(data);
      lgClipboard_abort(request);
      return;
    }

    const uint64_t hint = *(const unsigned long *)data;
    const uint64_t sizeHint = hint ? hint : LG_CLIPBOARD_SIZE_UNKNOWN;
    const LG_ClipboardData dataType = x11cb.read.type;
    x11cb.read.incremental = true;
    x11cb.read.sizeHint    = sizeHint;
    XFree(data);
    enum X11ClipboardReadCallResult result;
    do
      result = x11CBReadCallNL(X11_CLIPBOARD_READ_BEGIN,
          dataType, sizeHint, 0, NULL, 0);
    while (result == X11_CLIPBOARD_READ_CALL_RETRY);
    if (result == X11_CLIPBOARD_READ_CALL_STALE)
    {
      LG_UNLOCK(x11cb.lock);
      return;
    }
    if (result == X11_CLIPBOARD_READ_CALL_BLOCKED)
    {
      LG_UNLOCK(x11cb.lock);
      return;
    }
    if (result != X11_CLIPBOARD_READ_CALL_ACCEPTED)
    {
      cancelReadNL(false);
      LG_UNLOCK(x11cb.lock);
      return;
    }
    x11cb.read.begun = true;
    const bool processProperty = x11cb.read.propertyPending;
    const Window processWindow = x11cb.read.window;
    x11cb.read.propertyPending = false;
    LG_UNLOCK(x11cb.lock);
    if (processProperty)
      x11CBSelectionIncr((XPropertyEvent)
      {
        .display = x11.display,
        .window  = processWindow,
        .atom    = x11atoms.SEL_DATA,
        .state   = PropertyNewValue,
      });
    return;
  }

  LG_ClipboardData dataType;
  for(dataType = 0; dataType < LG_CLIPBOARD_DATA_NONE; ++dataType)
    if (x11cb.aTypes[dataType] == type)
      break;

  const LG_ClipboardRequest request = x11cb.read.request;
  const bool valid = dataType != LG_CLIPBOARD_DATA_NONE &&
    dataType == x11cb.read.type && format == 8 && (!itemCount || data);
  cancelReadNL(false);
  LG_UNLOCK(x11cb.lock);

  if (!valid)
  {
    DEBUG_WARN("Invalid clipboard data");
    lgClipboard_abort(request);
    goto out;
  }

  lgClipboard_data(request, dataType, data, itemCount);

out:
  if (data)
    XFree(data);
}

void x11CBNotice(LG_ClipboardData type)
{
  const uint64_t nextPresentation = type == LG_CLIPBOARD_DATA_FILES ?
    lgClipboardFiles_remotePresentationAcquire() : 0;
  LG_LOCK(x11cb.lock);
  const LG_ClipboardRequest oldRequest = x11cb.read.window ?
    cancelReadNL(true) : LG_CLIPBOARD_REQUEST_INVALID;
  clearTargetsNL();
  clearFileImportNL();
  const uint64_t oldPresentation = x11cb.filePresentation;
  x11cb.filePresentation = nextPresentation;
  x11cb.aCurSelection     = BadValue;
  x11cb.haveRequest       = type != LG_CLIPBOARD_DATA_FILES || nextPresentation;
  x11cb.type              = type;
  XSetSelectionOwner(x11.display, x11atoms.CLIPBOARD,
      x11cb.haveRequest ? x11.window : None, CurrentTime);
  XFlush(x11.display);
  LG_UNLOCK(x11cb.lock);
  if (oldRequest != LG_CLIPBOARD_REQUEST_INVALID)
    lgClipboard_abort(oldRequest);
  if (oldPresentation)
    lgClipboardFiles_remotePresentationRelease(oldPresentation);
  lgClipboardFiles_clearLocal();
}

void x11CBRelease(void)
{
  LG_LOCK(x11cb.lock);
  x11cb.haveRequest = false;
  const uint64_t presentation = x11cb.filePresentation;
  x11cb.filePresentation = 0;
  XGrabServer(x11.display);
  if (XGetSelectionOwner(x11.display, x11atoms.CLIPBOARD) == x11.window)
    XSetSelectionOwner(
        x11.display, x11atoms.CLIPBOARD, None, CurrentTime);
  XUngrabServer(x11.display);
  XFlush(x11.display);
  LG_UNLOCK(x11cb.lock);
  if (presentation)
    lgClipboardFiles_remotePresentationRelease(presentation);
}

void x11CBFree(void)
{
  x11CBRelease();

  LG_LOCK(x11cb.lock);
  const LG_ClipboardRequest request = x11cb.read.window ?
    cancelReadNL(true) : LG_CLIPBOARD_REQUEST_INVALID;
  clearTargetsNL();
  clearFileImportNL();
  struct X11ClipboardWrite * writes = takeFileWritesNL();
  LG_UNLOCK(x11cb.lock);

  if (request != LG_CLIPBOARD_REQUEST_INVALID)
    lgClipboard_abort(request);
  cancelFileWrites(writes, true);
  lgClipboardFiles_clearLocal();
}

void x11CBRequest(LG_ClipboardRequest request, LG_ClipboardData type)
{
  if (request == LG_CLIPBOARD_REQUEST_INVALID ||
      type < LG_CLIPBOARD_DATA_TEXT || type >= LG_CLIPBOARD_DATA_NONE)
    return;

  LG_LOCK(x11cb.lock);
  const LG_ClipboardRequest oldRequest = x11cb.read.window ?
    cancelReadNL(true) : LG_CLIPBOARD_REQUEST_INVALID;
  if (x11cb.aCurSelection == BadValue)
  {
    LG_UNLOCK(x11cb.lock);
    if (oldRequest != LG_CLIPBOARD_REQUEST_INVALID)
      lgClipboard_abort(oldRequest);
    lgClipboard_abort(request);
    return;
  }

  const Window window = XCreateSimpleWindow(
      x11.display, x11.window, 0, 0, 1, 1, 0, 0, 0);
  if (!window)
  {
    LG_UNLOCK(x11cb.lock);
    if (oldRequest != LG_CLIPBOARD_REQUEST_INVALID)
      lgClipboard_abort(oldRequest);
    lgClipboard_abort(request);
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
    lgClipboard_abort(oldRequest);
}

void x11CBRequestReady(LG_ClipboardRequest request)
{
  LG_LOCK(x11cb.lock);
  if (x11cb.read.request != request)
  {
    LG_UNLOCK(x11cb.lock);
    return;
  }
  if (x11cb.read.calling)
  {
    x11cb.read.readyPending = true;
    LG_UNLOCK(x11cb.lock);
    return;
  }
  if (!x11cb.read.blocked)
  {
    LG_UNLOCK(x11cb.lock);
    return;
  }

  x11cb.read.blocked = false;
  const bool begin = !x11cb.read.begun;
  const bool end = x11cb.read.endPending;
  const LG_ClipboardData type = x11cb.read.type;
  const uint64_t sizeHint = x11cb.read.sizeHint;
  const uint64_t offset = x11cb.read.offset;
  const size_t pendingSize = x11cb.read.pendingSize;
  const unsigned long pendingUnits = x11cb.read.pendingUnits;
  const bool pendingMore = x11cb.read.pendingMore;
  uint8_t * pending = NULL;

  /* The provider call runs without x11cb.lock.  Request cancellation or a
   * selection replacement may therefore clear and free read.pending while
   * the call is in progress.  Give the call its own bounded copy so teardown
   * can remain immediate without invalidating the callback's data. */
  if (pendingSize)
  {
    pending = malloc(pendingSize);
    if (!pending)
    {
      const LG_ClipboardRequest abort = cancelReadNL(true);
      LG_UNLOCK(x11cb.lock);
      lgClipboard_abort(abort);
      return;
    }
    memcpy(pending, x11cb.read.pending, pendingSize);
  }

  enum X11ClipboardReadOperation operation;
  if (begin)
    operation = X11_CLIPBOARD_READ_BEGIN;
  else if (pendingSize)
    operation = X11_CLIPBOARD_READ_CHUNK;
  else if (end)
    operation = X11_CLIPBOARD_READ_END;
  else
  {
    LG_UNLOCK(x11cb.lock);
    return;
  }

  enum X11ClipboardReadCallResult result;
  do
    result = x11CBReadCallNL(operation, type, sizeHint,
        offset, pending, pendingSize);
  while (result == X11_CLIPBOARD_READ_CALL_RETRY);
  free(pending);
  if (result == X11_CLIPBOARD_READ_CALL_STALE)
  {
    LG_UNLOCK(x11cb.lock);
    return;
  }
  if (result == X11_CLIPBOARD_READ_CALL_BLOCKED)
  {
    LG_UNLOCK(x11cb.lock);
    return;
  }
  if (result != X11_CLIPBOARD_READ_CALL_ACCEPTED)
  {
    cancelReadNL(false);
    LG_UNLOCK(x11cb.lock);
    return;
  }

  bool processProperty = false;
  Window window = None;
  if (begin)
    x11cb.read.begun = true;
  else if (pendingSize)
  {
    x11cb.read.offset += pendingSize;
    free(x11cb.read.pending);
    x11cb.read.pending      = NULL;
    x11cb.read.pendingSize  = 0;
    x11cb.read.pendingUnits = 0;
    x11cb.read.pendingMore  = false;
    if (pendingMore)
    {
      if (!advanceReadPropertyNL(pendingUnits))
      {
        const struct X11ClipboardRead read = clearReadNL();
        LG_UNLOCK(x11cb.lock);
        free(read.pending);
        lgClipboard_abort(read.request);
        return;
      }
      processProperty = true;
      window = x11cb.read.window;
    }
    else
      x11cb.read.propertyOffset = 0;
  }
  else
  {
    cancelReadNL(false);
    LG_UNLOCK(x11cb.lock);
    return;
  }

  if (x11cb.read.propertyPending && x11cb.read.incremental &&
      !x11cb.read.blocked && !x11cb.read.pendingSize &&
      !x11cb.read.endPending)
  {
    x11cb.read.propertyPending = false;
    processProperty = true;
    window = x11cb.read.window;
  }
  LG_UNLOCK(x11cb.lock);

  if (processProperty)
    x11CBSelectionIncr((XPropertyEvent)
    {
      .display = x11.display,
      .window  = window,
      .atom    = x11atoms.SEL_DATA,
      .state   = PropertyNewValue,
    });
}

void x11CBRequestCancel(LG_ClipboardRequest request,
    LG_ClipboardCancelReason reason)
{
  (void)reason;
  LG_LOCK(x11cb.lock);
  if (x11cb.read.request == request)
    cancelReadNL(false);
  LG_UNLOCK(x11cb.lock);
}
