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

#include "atoms.h"
#include "clipboard.h"
#include "test.h"
#include "x11.h"
#include "core/clipboard.h"
#include "core/clipboard_files.h"

#include "common/debug.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_ABORT    12U
#define MAX_CHANGE   8U
#define MAX_CONVERT  16U
#define MAX_DATA     4U
#define MAX_NOTICE   8U
#define MAX_PROP     16U
#define MAX_SEND     8U
#define MAX_TRANSFER (128U * 1024U)
#define MAX_WINDOW   16U

#define A_CLIPBOARD 10UL
#define A_TARGETS   11UL
#define A_SEL_DATA  12UL
#define A_INCR      13UL
#define A_TEXT      100UL
#define A_PNG       101UL
#define A_BMP       102UL
#define A_TIFF      103UL
#define A_JPEG      104UL
#define A_URI       105UL
#define A_GNOME     106UL
#define A_KDE       107UL

struct WindowLog
{
  Window window;
  bool   dead;
};

struct ConvertLog
{
  Atom   selection;
  Atom   target;
  Atom   property;
  Window requestor;
};

struct ChangeLog
{
  Window        window;
  Atom          property;
  Atom          type;
  int           format;
  int           count;
  unsigned char data[MAX_TRANSFER];
  size_t        size;
};

struct Property
{
  int           status;
  Atom          type;
  int           format;
  unsigned long count;
  unsigned long after;
  unsigned char data[MAX_TRANSFER];
  size_t        size;
  bool          null;
};

struct TypeNotice
{
  LG_ClipboardData type[LG_CLIPBOARD_DATA_NONE];
  size_t           count;
};

struct DataNotice
{
  LG_ClipboardRequest request;
  LG_ClipboardData    type;
  uint8_t             data[MAX_TRANSFER];
  size_t              size;
};

struct Log
{
  struct WindowLog   window[MAX_WINDOW];
  struct ConvertLog  convert[MAX_CONVERT];
  struct ChangeLog   change[MAX_CHANGE];
  struct Property    prop[MAX_PROP];
  struct TypeNotice  notice[MAX_NOTICE];
  struct DataNotice  data[MAX_DATA];
  XEvent             send[MAX_SEND];
  LG_ClipboardRequest abort[MAX_ABORT];
  unsigned int       windowN;
  unsigned int       convertN;
  unsigned int       changeN;
  unsigned int       propN;
  unsigned int       propPos;
  unsigned int       noticeN;
  unsigned int       dataN;
  unsigned int       sendN;
  unsigned int       abortN;
  unsigned int       releaseN;
  unsigned int       requestN;
  unsigned int       requestReadyN;
  unsigned int       destroyN;
  unsigned int       freeN;
  unsigned int       flushN;
  unsigned int       grabN;
  unsigned int       ungrabN;
  unsigned int       selectN;
  unsigned int       structureSelectN;
  LG_ClipboardData   requestType;
  const LG_ClipboardStreamOps * requestStream;
  void              * requestOpaque;
  LG_ClipboardRequest requestId;
  LG_ClipboardRequest streamRequest;
  LG_ClipboardData    streamType;
  uint64_t            streamSizeHint;
  unsigned int        streamBeginN;
  unsigned int        streamChunkN;
  unsigned int        streamEndN;
  size_t              streamMaxChunk;
  LG_ClipboardResult  beginResult;
  LG_ClipboardResult  chunkResult;
  LG_ClipboardResult  endResult;
  LG_ClipboardResult  requestBeginResult;
  bool                readyDuringChunk;
  bool                readyDuringEnd;
  Window              owner;
  Window              nextWindow;
  bool                requestOK;
  bool                requestCancelSync;
  bool                requestBeginSync;
  bool                fixesOK;
  bool                fileSetOK;
  bool                presentationOK;
  unsigned int        fileSetN;
  unsigned int        fileClearN;
  unsigned int        presentationAcquireN;
  unsigned int        presentationDeliveredN;
  unsigned int        presentationReleaseN;
  uint64_t            presentationDelivered[8];
  uint64_t            presentationReleased[8];
  char                fileMime[80];
  uint8_t             fileData[MAX_TRANSFER];
  size_t              fileSize;
};

struct X11DSState x11;
struct X11DSAtoms x11atoms;

static struct Log rec;

static pthread_mutex_t readRaceLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  readRaceCond = PTHREAD_COND_INITIALIZER;
static bool readRacePause;
static bool readRaceEntered;
static bool readRaceResume;

static size_t itemSize(int format, unsigned long count)
{
  switch (format)
  {
    case 8:
      return count;
    case 16:
      return count * sizeof(short);
    case 32:
      return count * sizeof(long);
    default:
      return 0;
  }
}

Atom XInternAtom(Display * display, const char * name, Bool onlyIfExists)
{
  CHECK(display == x11.display);
  CHECK(!onlyIfExists);
  if (!strcmp(name, "UTF8_STRING"))
    return A_TEXT;
  if (!strcmp(name, "image/png"))
    return A_PNG;
  if (!strcmp(name, "image/bmp"))
    return A_BMP;
  if (!strcmp(name, "image/tiff"))
    return A_TIFF;
  if (!strcmp(name, "image/jpeg"))
    return A_JPEG;
  if (!strcmp(name, "text/uri-list"))
    return A_URI;
  if (!strcmp(name, "x-special/gnome-copied-files"))
    return A_GNOME;
  if (!strcmp(name, "application/x-kde-cutselection"))
    return A_KDE;
  CHECK(false);
  return None;
}

Bool XFixesQueryExtension(Display * display, int * eventBase, int * errorBase)
{
  CHECK(display == x11.display);
  if (!rec.fixesOK)
    return False;
  *eventBase = 80;
  *errorBase = 81;
  return True;
}

void XFixesSelectSelectionInput(Display * display, Window window,
    Atom selection, unsigned long mask)
{
  CHECK(display == x11.display);
  CHECK(window == x11.window);
  CHECK(selection == x11atoms.CLIPBOARD);
  CHECK(mask == XFixesSetSelectionOwnerNotifyMask);
  ++rec.selectN;
}

Window XCreateSimpleWindow(Display * display, Window parent,
    int x, int y, unsigned int width, unsigned int height,
    unsigned int borderWidth, unsigned long border,
    unsigned long background)
{
  CHECK(display == x11.display);
  CHECK(parent == x11.window);
  CHECK(x == 0);
  CHECK(y == 0);
  CHECK(width == 1);
  CHECK(height == 1);
  CHECK(borderWidth == 0);
  CHECK(border == 0);
  CHECK(background == 0);
  CHECK(rec.windowN < ARRAY_LENGTH(rec.window));
  struct WindowLog * window = &rec.window[rec.windowN++];
  window->window = ++rec.nextWindow;
  return window->window;
}

int XDestroyWindow(Display * display, Window window)
{
  CHECK(display == x11.display);
  for (unsigned int i = 0; i < rec.windowN; ++i)
    if (rec.window[i].window == window)
    {
      CHECK(!rec.window[i].dead);
      rec.window[i].dead = true;
      ++rec.destroyN;
      return Success;
    }
  CHECK(false);
  return BadWindow;
}

int XConvertSelection(Display * display, Atom selection, Atom target,
    Atom property, Window requestor, Time time)
{
  CHECK(display == x11.display);
  CHECK(time == CurrentTime);
  CHECK(rec.convertN < ARRAY_LENGTH(rec.convert));
  struct ConvertLog * convert = &rec.convert[rec.convertN++];
  convert->selection = selection;
  convert->target    = target;
  convert->property  = property;
  convert->requestor = requestor;
  return Success;
}

int XSelectInput(Display * display, Window window, long mask)
{
  CHECK(display == x11.display);
  CHECK(mask == PropertyChangeMask ||
      mask == (PropertyChangeMask | StructureNotifyMask));
  if (mask & StructureNotifyMask)
    ++rec.structureSelectN;
  CHECK(window != None);
  return Success;
}

int XSetSelectionOwner(Display * display, Atom selection,
    Window owner, Time time)
{
  CHECK(display == x11.display);
  CHECK(selection == x11atoms.CLIPBOARD);
  CHECK(time == CurrentTime);
  rec.owner = owner;
  return Success;
}

Window XGetSelectionOwner(Display * display, Atom selection)
{
  CHECK(display == x11.display);
  CHECK(selection == x11atoms.CLIPBOARD);
  return rec.owner;
}

int XGrabServer(Display * display)
{
  CHECK(display == x11.display);
  ++rec.grabN;
  return Success;
}

int XUngrabServer(Display * display)
{
  CHECK(display == x11.display);
  ++rec.ungrabN;
  return Success;
}

int XFlush(Display * display)
{
  CHECK(display == x11.display);
  ++rec.flushN;
  return Success;
}

int XChangeProperty(Display * display, Window window, Atom property,
    Atom type, int format, int mode, const unsigned char * data, int count)
{
  CHECK(display == x11.display);
  CHECK(mode == PropModeReplace);
  CHECK(count >= 0);
  CHECK(rec.changeN < ARRAY_LENGTH(rec.change));
  struct ChangeLog * change = &rec.change[rec.changeN++];
  change->window   = window;
  change->property = property;
  change->type     = type;
  change->format   = format;
  change->count    = count;
  change->size     = itemSize(format, count);
  CHECK(change->size <= sizeof(change->data));
  CHECK(!change->size || data);
  if (change->size)
    memcpy(change->data, data, change->size);
  return Success;
}

Status XSendEvent(Display * display, Window window, Bool propagate,
    long mask, XEvent * event)
{
  CHECK(display == x11.display);
  CHECK(!propagate);
  CHECK(mask == 0);
  CHECK(rec.sendN < ARRAY_LENGTH(rec.send));
  CHECK(event->xselection.requestor == window);
  rec.send[rec.sendN++] = *event;
  return True;
}

int XGetWindowProperty(Display * display, Window window, Atom property,
    long offset, long length, Bool delete, Atom requestedType,
    Atom * actualType, int * actualFormat, unsigned long * count,
    unsigned long * after, unsigned char ** data)
{
  CHECK(display == x11.display);
  CHECK(window != None);
  CHECK(property != None);
  CHECK(offset >= 0);
  CHECK(length == ~0L || length ==
    (KVMFR_CLIPBOARD_REPRESENTATION_BYTES + 3U) / 4U);
  CHECK(delete);
  CHECK(requestedType == AnyPropertyType);
  CHECK(rec.propPos < rec.propN);
  const struct Property * prop = &rec.prop[rec.propPos];
  *actualType   = prop->type;
  *actualFormat = prop->format;
  size_t start = 0;
  size_t size = prop->size;
  if (prop->format == 8 && length != ~0L)
  {
    start = (size_t)offset * 4U;
    CHECK(start <= prop->size);
    const size_t maximum = (size_t)length * 4U;
    size = prop->size - start;
    if (size > maximum)
      size = maximum;
    *count = size;
    *after = prop->size - start - size;
  }
  else
  {
    *count = prop->count;
    *after = prop->after;
  }
  if (prop->null)
    *data = NULL;
  else
  {
    CHECK(start + size <= sizeof(prop->data));
    *data = malloc(size ? size : 1);
    CHECK(*data);
    if (size)
      memcpy(*data, prop->data + start, size);
  }
  if (*after == 0)
    ++rec.propPos;
  return prop->status;
}

int XFree(void * data)
{
  CHECK(data);
  ++rec.freeN;
  free(data);
  return Success;
}

void lgClipboard_notifyTypes(
    const LG_ClipboardData types[], size_t count)
{
  CHECK(rec.noticeN < ARRAY_LENGTH(rec.notice));
  CHECK(count <= ARRAY_LENGTH(rec.notice[0].type));
  struct TypeNotice * notice = &rec.notice[rec.noticeN++];
  notice->count = count;
  memcpy(notice->type, types, count * sizeof(*types));
}

void lgClipboard_data(LG_ClipboardRequest request,
    LG_ClipboardData type, const void * data, size_t size)
{
  CHECK(rec.dataN < ARRAY_LENGTH(rec.data));
  CHECK(size <= MAX_TRANSFER);
  CHECK(!size || data);
  struct DataNotice * notice = &rec.data[rec.dataN++];
  notice->request = request;
  notice->type    = type;
  notice->size    = size;
  if (size)
    memcpy(notice->data, data, size);
}

LG_ClipboardResult lgClipboard_dataBegin(LG_ClipboardRequest request,
    LG_ClipboardData type, uint64_t sizeHint)
{
  ++rec.streamBeginN;
  rec.streamRequest  = request;
  rec.streamType     = type;
  rec.streamSizeHint = sizeHint;
  if (rec.beginResult == LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    CHECK(rec.dataN < ARRAY_LENGTH(rec.data));
    rec.data[rec.dataN] = (struct DataNotice)
    {
      .request = request,
      .type    = type,
    };
  }
  return rec.beginResult;
}

LG_ClipboardResult lgClipboard_dataChunk(LG_ClipboardRequest request,
    uint64_t offset, const void * data, size_t size)
{
  const LG_ClipboardResult result = rec.chunkResult;
  ++rec.streamChunkN;
  CHECK(request == rec.streamRequest);
  CHECK(rec.dataN < ARRAY_LENGTH(rec.data));
  struct DataNotice * notice = &rec.data[rec.dataN];
  CHECK(offset == notice->size);
  CHECK(data);
  CHECK(size);
  CHECK(size <= sizeof(notice->data) - notice->size);
  if (size > rec.streamMaxChunk)
    rec.streamMaxChunk = size;
  pthread_mutex_lock(&readRaceLock);
  if (readRacePause)
  {
    readRacePause   = false;
    readRaceEntered = true;
    pthread_cond_signal(&readRaceCond);
    while (!readRaceResume)
      pthread_cond_wait(&readRaceCond, &readRaceLock);
  }
  pthread_mutex_unlock(&readRaceLock);
  if (rec.readyDuringChunk)
  {
    rec.readyDuringChunk = false;
    rec.chunkResult = LG_CLIPBOARD_RESULT_ACCEPTED;
    x11CBRequestReady(request);
  }
  if (result == LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    memcpy(notice->data + notice->size, data, size);
    notice->size += size;
  }
  return result;
}

LG_ClipboardResult lgClipboard_dataEnd(LG_ClipboardRequest request,
    uint64_t finalSize)
{
  const LG_ClipboardResult result = rec.endResult;
  ++rec.streamEndN;
  CHECK(request == rec.streamRequest);
  CHECK(rec.dataN < ARRAY_LENGTH(rec.data));
  if (rec.readyDuringEnd)
  {
    rec.readyDuringEnd = false;
    rec.endResult = LG_CLIPBOARD_RESULT_ACCEPTED;
    x11CBRequestReady(request);
  }
  if (result == LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    CHECK(finalSize == rec.data[rec.dataN].size);
    ++rec.dataN;
  }
  return result;
}

void lgClipboard_abort(LG_ClipboardRequest request)
{
  CHECK(rec.abortN < ARRAY_LENGTH(rec.abort));
  rec.abort[rec.abortN++] = request;
}

void lgClipboard_release(void)
{
  ++rec.releaseN;
}

bool lgClipboard_requestStream(LG_ClipboardData type,
    const LG_ClipboardStreamOps * stream, void * opaque,
    LG_ClipboardRequest * request)
{
  ++rec.requestN;
  rec.requestType = type;
  rec.requestStream = stream;
  rec.requestOpaque = opaque;
  rec.requestId = 9000 + rec.requestN;
  if (request)
    *request = rec.requestId;
  if (rec.requestBeginSync)
    rec.requestBeginResult = stream->begin(
        opaque, type, LG_CLIPBOARD_SIZE_UNKNOWN);
  if (rec.requestCancelSync)
    stream->cancel(opaque, LG_CLIPBOARD_CANCEL_REPLACED);
  return rec.requestOK;
}

bool lgClipboard_requestReady(LG_ClipboardRequest request)
{
  CHECK(request == rec.requestId);
  ++rec.requestReadyN;
  return true;
}

bool lgClipboardFiles_setLocal(const char * mime,
    const void * data, size_t size)
{
  CHECK(mime);
  CHECK(!size || data);
  CHECK(size <= sizeof(rec.fileData));
  ++rec.fileSetN;
  snprintf(rec.fileMime, sizeof(rec.fileMime), "%s", mime);
  rec.fileSize = size;
  if (size)
    memcpy(rec.fileData, data, size);
  return rec.fileSetOK;
}

void lgClipboardFiles_clearLocal(void)
{
  ++rec.fileClearN;
}

uint64_t lgClipboardFiles_remotePresentationAcquire(void)
{
  ++rec.presentationAcquireN;
  return rec.presentationOK ? 2001U : 0;
}

bool lgClipboardFiles_getRemotePresentation(uint64_t presentation,
    const char * mime, char ** data, size_t * size)
{
  CHECK(presentation >= 2001U);
  const char * value;
  if (!strcmp(mime, "text/uri-list"))
    value = "file:///run/user/1000/looking-glass/guest.txt\r\n";
  else if (!strcmp(mime, "x-special/gnome-copied-files"))
    value = "copy\nfile:///run/user/1000/looking-glass/guest.txt\r\n";
  else if (!strcmp(mime, "application/x-kde-cutselection"))
    value = "0";
  else
    return false;
  *size = strlen(value);
  *data = malloc(*size);
  CHECK(*data);
  memcpy(*data, value, *size);
  return true;
}

void lgClipboardFiles_remotePresentationDelivered(uint64_t presentation)
{
  CHECK(presentation);
  CHECK(rec.presentationDeliveredN <
      ARRAY_LENGTH(rec.presentationDelivered));
  rec.presentationDelivered[rec.presentationDeliveredN++] = presentation;
}

void lgClipboardFiles_remotePresentationRelease(uint64_t presentation)
{
  CHECK(presentation);
  CHECK(rec.presentationReleaseN <
      ARRAY_LENGTH(rec.presentationReleased));
  rec.presentationReleased[rec.presentationReleaseN++] = presentation;
}

static struct Property * prop(void)
{
  CHECK(rec.propN < ARRAY_LENGTH(rec.prop));
  struct Property * prop = &rec.prop[rec.propN++];
  prop->status = Success;
  return prop;
}

static void prop8(Atom type, const void * data, size_t size)
{
  struct Property * value = prop();
  CHECK(size <= sizeof(value->data));
  value->type   = type;
  value->format = 8;
  value->count  = size;
  value->size   = size;
  if (size)
    memcpy(value->data, data, size);
}

static void prop32(Atom type, const unsigned long * data, size_t count)
{
  struct Property * value = prop();
  const size_t size = count * sizeof(*data);
  CHECK(size <= sizeof(value->data));
  value->type   = type;
  value->format = 32;
  value->count  = count;
  value->size   = size;
  if (size)
    memcpy(value->data, data, size);
}

static void propNull(Atom type, int format, unsigned long count)
{
  struct Property * value = prop();
  value->type   = type;
  value->format = format;
  value->count  = count;
  value->null   = true;
}

static void selection(Window requestor, Atom target, Atom property)
{
  XEvent event = {};
  event.xselection.type      = SelectionNotify;
  event.xselection.display   = x11.display;
  event.xselection.requestor = requestor;
  event.xselection.selection = x11atoms.CLIPBOARD;
  event.xselection.target    = target;
  event.xselection.property  = property;
  CHECK(x11CBEventThread(&event));
}

static void propertyState(Window window, Atom atom, int state)
{
  XEvent event = {};
  event.xproperty.type    = PropertyNotify;
  event.xproperty.display = x11.display;
  event.xproperty.window  = window;
  event.xproperty.atom    = atom;
  event.xproperty.state   = state;
  CHECK(x11CBEventThread(&event));
}

static void property(Window window)
{
  propertyState(window, x11atoms.SEL_DATA, PropertyNewValue);
}

static void owner(Window owner)
{
  rec.owner = owner;
  XFixesSelectionNotifyEvent event =
  {
    .type      = x11.eventBase + XFixesSelectionNotify,
    .display   = x11.display,
    .window    = x11.window,
    .owner     = owner,
    .selection = x11atoms.CLIPBOARD,
  };
  CHECK(x11CBEventThread((const XEvent *)&event));
}

static void selectionClear(Window owner)
{
  rec.owner = owner;
  XEvent event = {};
  event.xselectionclear.type      = SelectionClear;
  event.xselectionclear.display   = x11.display;
  event.xselectionclear.window    = x11.window;
  event.xselectionclear.selection = x11atoms.CLIPBOARD;
  CHECK(x11CBEventThread(&event));
}

static void discover(Window ownerWindow,
    const unsigned long * targets, size_t count)
{
  const unsigned int no = rec.convertN;
  owner(ownerWindow);
  CHECK(rec.convertN == no + 1);
  const struct ConvertLog * convert = &rec.convert[no];
  CHECK(convert->selection == x11atoms.CLIPBOARD);
  CHECK(convert->target == x11atoms.TARGETS);
  CHECK(convert->property == x11atoms.TARGETS);
  prop32(XA_ATOM, targets, count);
  selection(convert->requestor, x11atoms.TARGETS, x11atoms.TARGETS);
}

static Window request(LG_ClipboardRequest id, LG_ClipboardData type)
{
  const unsigned int no = rec.convertN;
  x11CBRequest(id, type);
  CHECK(rec.convertN == no + 1);
  const struct ConvertLog * convert = &rec.convert[no];
  CHECK(convert->selection == x11atoms.CLIPBOARD);
  CHECK(convert->property == x11atoms.SEL_DATA);
  return convert->requestor;
}

static void selectionRequest(Atom target, Atom property)
{
  XEvent event = {};
  event.xselectionrequest.type      = SelectionRequest;
  event.xselectionrequest.display   = x11.display;
  event.xselectionrequest.owner     = x11.window;
  event.xselectionrequest.requestor = 900;
  event.xselectionrequest.selection = x11atoms.CLIPBOARD;
  event.xselectionrequest.target    = target;
  event.xselectionrequest.property  = property;
  CHECK(x11CBEventThread(&event));
}

static void start(void)
{
  memset(&x11, 0, sizeof(x11));
  memset(&x11atoms, 0, sizeof(x11atoms));
  memset(&rec, 0, sizeof(rec));
  rec.nextWindow     = 1000;
  rec.requestOK      = true;
  rec.fixesOK        = true;
  rec.beginResult    = LG_CLIPBOARD_RESULT_ACCEPTED;
  rec.chunkResult    = LG_CLIPBOARD_RESULT_ACCEPTED;
  rec.endResult      = LG_CLIPBOARD_RESULT_ACCEPTED;
  rec.fileSetOK      = true;
  rec.presentationOK = true;

  pthread_mutex_lock(&readRaceLock);
  readRacePause   = false;
  readRaceEntered = false;
  readRaceResume  = false;
  pthread_mutex_unlock(&readRaceLock);

  x11.display     = (Display *)(uintptr_t)1;
  x11.window      = 50;

  x11atoms.CLIPBOARD = A_CLIPBOARD;
  x11atoms.TARGETS   = A_TARGETS;
  x11atoms.SEL_DATA  = A_SEL_DATA;
  x11atoms.INCR      = A_INCR;

  CHECK(x11CBInit());
  CHECK(x11.eventBase == 80);
  CHECK(x11.errorBase == 81);
  CHECK(rec.selectN == 1);
}

static void finish(void)
{
  x11CBRelease();
  owner(None);
  CHECK(rec.owner == None);
  CHECK(rec.grabN == rec.ungrabN);
  for (unsigned int i = 0; i < rec.windowN; ++i)
    CHECK(rec.window[i].dead);
}

static void testTargets(void)
{
  start();
  const unsigned long targets[] = { A_PNG, 999, A_TEXT, A_PNG };
  discover(60, targets, ARRAY_LENGTH(targets));
  CHECK(rec.noticeN == 1);
  CHECK(rec.notice[0].count == 2);
  CHECK(rec.notice[0].type[0] == LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.notice[0].type[1] == LG_CLIPBOARD_DATA_PNG);
  CHECK(rec.destroyN == 1);
  CHECK(rec.freeN == 1);
  finish();
}

static void testFileImport(void)
{
  start();
  const unsigned long targets[] = { A_TEXT, A_URI };
  discover(601, targets, ARRAY_LENGTH(targets));
  CHECK(rec.convertN == 2);
  const struct ConvertLog * convert = &rec.convert[1];
  CHECK(convert->target == A_URI);
  CHECK(convert->property == x11atoms.SEL_DATA);
  CHECK(rec.noticeN == 0);

  const char payload[] =
    "file:///home/user/first.txt\r\nfile:///home/user/second.txt\r\n";
  prop8(A_URI, payload, sizeof(payload) - 1U);
  selection(convert->requestor, A_URI, x11atoms.SEL_DATA);
  CHECK(rec.fileSetN == 1);
  CHECK(strcmp(rec.fileMime, "text/uri-list") == 0);
  CHECK(rec.fileSize == sizeof(payload) - 1U);
  CHECK(memcmp(rec.fileData, payload, sizeof(payload) - 1U) == 0);
  CHECK(rec.noticeN == 0);
  finish();
}

static void testFileImportIncr(void)
{
  start();
  const unsigned long targets[] = { A_URI, A_TEXT, A_GNOME };
  discover(602, targets, ARRAY_LENGTH(targets));
  CHECK(rec.convertN == 2);
  const struct ConvertLog * convert = &rec.convert[1];
  CHECK(convert->target == A_GNOME);

  const unsigned long capacity = 128;
  prop32(x11atoms.INCR, &capacity, 1);
  selection(convert->requestor, A_GNOME, x11atoms.SEL_DATA);
  CHECK(rec.fileSetN == 0);

  const char first[] = "copy\nfile:///home/user/";
  const char second[] = "large%20file.txt\r\n";
  prop8(A_GNOME, first, sizeof(first) - 1U);
  property(convert->requestor);
  CHECK(rec.fileSetN == 0);
  prop8(A_GNOME, second, sizeof(second) - 1U);
  property(convert->requestor);
  propNull(A_GNOME, 8, 0);
  property(convert->requestor);

  CHECK(rec.fileSetN == 1);
  CHECK(strcmp(rec.fileMime,
      "x-special/gnome-copied-files") == 0);
  CHECK(rec.fileSize == sizeof(first) + sizeof(second) - 2U);
  CHECK(memcmp(rec.fileData,
      "copy\nfile:///home/user/large%20file.txt\r\n",
      rec.fileSize) == 0);
  CHECK(rec.noticeN == 0);
  finish();
}

static void testFileSource(void)
{
  start();
  x11CBNotice(LG_CLIPBOARD_DATA_FILES);
  CHECK(rec.owner == x11.window);
  CHECK(rec.presentationAcquireN == 1);
  CHECK(rec.presentationReleaseN == 0);

  selectionRequest(x11atoms.TARGETS, 710);
  CHECK(rec.changeN == 1);
  CHECK(rec.change[0].format == 32);
  CHECK(rec.change[0].count == 4);
  const Atom * targets = (const Atom *)rec.change[0].data;
  CHECK(targets[0] == x11atoms.TARGETS);
  CHECK(targets[1] == A_URI);
  CHECK(targets[2] == A_GNOME);
  CHECK(targets[3] == A_KDE);

  selectionRequest(A_URI, 711);
  CHECK(rec.presentationAcquireN == 2);
  CHECK(rec.structureSelectN == 1);
  CHECK(rec.changeN == 2);
  CHECK(rec.change[1].type == x11atoms.INCR);
  CHECK(rec.change[1].format == 32);
  CHECK(rec.change[1].count == 1);
  CHECK(rec.sendN == 2);

  propertyState(900, 711, PropertyDelete);
  CHECK(rec.changeN == 3);
  CHECK(rec.change[2].type == A_URI);
  CHECK(rec.change[2].format == 8);
  const char expected[] =
    "file:///run/user/1000/looking-glass/guest.txt\r\n";
  CHECK(rec.change[2].size == sizeof(expected) - 1U);
  CHECK(memcmp(rec.change[2].data,
      expected, sizeof(expected) - 1U) == 0);

  propertyState(900, 711, PropertyDelete);
  CHECK(rec.changeN == 4);
  CHECK(rec.change[3].type == A_URI);
  CHECK(rec.change[3].count == 0);
  CHECK(rec.presentationDeliveredN == 1);
  CHECK(rec.presentationDelivered[0] == 2001U);
  CHECK(rec.presentationReleaseN == 1);
  CHECK(rec.presentationReleased[0] == 2001U);

  selectionRequest(A_KDE, 712);
  CHECK(rec.presentationAcquireN == 3);
  CHECK(rec.structureSelectN == 2);
  CHECK(rec.changeN == 5);
  CHECK(rec.change[4].type == x11atoms.INCR);
  propertyState(900, 712, PropertyDelete);
  CHECK(rec.changeN == 6);
  CHECK(rec.change[5].type == A_KDE);
  CHECK(rec.change[5].size == 1);
  CHECK(rec.change[5].data[0] == '0');
  propertyState(900, 712, PropertyDelete);
  CHECK(rec.changeN == 7);
  CHECK(rec.change[6].type == A_KDE);
  CHECK(rec.change[6].count == 0);
  CHECK(rec.presentationDeliveredN == 1);
  CHECK(rec.presentationReleaseN == 2);
  CHECK(rec.presentationReleased[1] == 2001U);

  x11CBNotice(LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.presentationReleaseN == 3);
  CHECK(rec.presentationReleased[2] == 2001U);
  finish();
  CHECK(rec.presentationReleaseN == 3);
}

static void testFileSourceActiveReplacement(void)
{
  start();
  x11CBNotice(LG_CLIPBOARD_DATA_FILES);
  selectionRequest(A_URI, 713);
  CHECK(rec.changeN == 1);
  CHECK(rec.change[0].type == x11atoms.INCR);
  CHECK(rec.presentationAcquireN == 2);
  CHECK(rec.presentationReleaseN == 0);

  x11CBNotice(LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.presentationReleaseN == 1);
  CHECK(rec.changeN == 1);

  selectionClear(901);
  CHECK(rec.presentationReleaseN == 1);
  CHECK(rec.changeN == 1);

  propertyState(900, 713, PropertyDelete);
  CHECK(rec.changeN == 2);
  CHECK(rec.change[1].type == A_URI);
  CHECK(rec.change[1].size > 0);
  CHECK(rec.presentationDeliveredN == 0);

  propertyState(900, 713, PropertyDelete);
  CHECK(rec.changeN == 3);
  CHECK(rec.change[2].type == A_URI);
  CHECK(rec.change[2].count == 0);
  CHECK(rec.presentationDeliveredN == 1);
  CHECK(rec.presentationDelivered[0] == 2001U);
  CHECK(rec.presentationReleaseN == 2);
  CHECK(rec.presentationReleased[1] == 2001U);
  finish();
  CHECK(rec.presentationReleaseN == 2);

  start();
  x11CBNotice(LG_CLIPBOARD_DATA_FILES);
  selectionRequest(A_URI, 714);
  CHECK(rec.changeN == 1);
  CHECK(rec.presentationAcquireN == 2);

  XEvent destroyed = {};
  destroyed.xdestroywindow.type    = DestroyNotify;
  destroyed.xdestroywindow.display = x11.display;
  destroyed.xdestroywindow.window  = 900;
  CHECK(x11CBEventThread(&destroyed));
  CHECK(rec.changeN == 1);
  CHECK(rec.presentationReleaseN == 1);
  CHECK(rec.presentationDeliveredN == 0);

  finish();
  CHECK(rec.presentationReleaseN == 2);
}

static void testNormal(void)
{
  start();
  const unsigned long targets[] = { A_TEXT };
  discover(61, targets, ARRAY_LENGTH(targets));
  const Window window = request(101, LG_CLIPBOARD_DATA_TEXT);
  const uint8_t data[] = "normal data";
  prop8(A_TEXT, data, sizeof(data) - 1);
  selection(window, A_TEXT, x11atoms.SEL_DATA);

  CHECK(rec.dataN == 1);
  CHECK(rec.data[0].request == 101);
  CHECK(rec.data[0].type == LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.data[0].size == sizeof(data) - 1);
  CHECK(memcmp(rec.data[0].data, data, sizeof(data) - 1) == 0);
  CHECK(rec.abortN == 0);
  CHECK(rec.destroyN == 2);
  CHECK(rec.freeN == 2);
  finish();
}

static void testIncr(void)
{
  start();
  const unsigned long targets[] = { A_TEXT };
  discover(62, targets, ARRAY_LENGTH(targets));
  const Window window = request(201, LG_CLIPBOARD_DATA_TEXT);
  const unsigned long capacity = 2;
  prop32(x11atoms.INCR, &capacity, 1);
  selection(window, A_TEXT, x11atoms.SEL_DATA);
  CHECK(rec.dataN == 0);
  CHECK(rec.abortN == 0);

  const uint8_t first[] = "abc";
  const uint8_t second[] = "defg";
  prop8(A_TEXT, first, sizeof(first) - 1);
  property(window);
  prop8(A_TEXT, second, sizeof(second) - 1);
  property(window);
  propNull(A_TEXT, 8, 0);
  property(window);

  CHECK(rec.dataN == 1);
  CHECK(rec.data[0].request == 201);
  CHECK(rec.data[0].type == LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.data[0].size == 7);
  CHECK(memcmp(rec.data[0].data, "abcdefg", 7) == 0);
  CHECK(rec.abortN == 0);
  CHECK(rec.destroyN == 2);
  CHECK(rec.freeN == 4);
  finish();
}

static void testIncrBlocked(void)
{
  start();
  const unsigned long targets[] = { A_TEXT };
  discover(620, targets, ARRAY_LENGTH(targets));
  const Window window = request(211, LG_CLIPBOARD_DATA_TEXT);
  const unsigned long capacity = 9;
  rec.beginResult = LG_CLIPBOARD_RESULT_BLOCKED;
  prop32(x11atoms.INCR, &capacity, 1);
  selection(window, A_TEXT, x11atoms.SEL_DATA);
  CHECK(rec.streamBeginN == 1);
  CHECK(rec.dataN == 0);

  const uint8_t first[] = "abc";
  prop8(A_TEXT, first, sizeof(first) - 1);
  property(window);
  CHECK(rec.streamChunkN == 0);

  rec.beginResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  rec.chunkResult = LG_CLIPBOARD_RESULT_BLOCKED;
  x11CBRequestReady(211);
  CHECK(rec.streamBeginN == 2);
  CHECK(rec.streamSizeHint == capacity);
  CHECK(rec.streamChunkN == 1);
  CHECK(rec.data[0].size == 0);

  const uint8_t second[] = "defghi";
  prop8(A_TEXT, second, sizeof(second) - 1);
  property(window);
  CHECK(rec.streamChunkN == 1);

  rec.chunkResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  x11CBRequestReady(211);
  CHECK(rec.streamChunkN == 3);
  CHECK(rec.data[0].size == 9);
  CHECK(memcmp(rec.data[0].data, "abcdefghi", 9) == 0);

  rec.endResult = LG_CLIPBOARD_RESULT_BLOCKED;
  propNull(A_TEXT, 8, 0);
  property(window);
  CHECK(rec.streamEndN == 1);
  CHECK(rec.dataN == 0);
  rec.endResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  x11CBRequestReady(211);
  CHECK(rec.streamEndN == 2);
  CHECK(rec.dataN == 1);
  CHECK(rec.abortN == 0);
  finish();
}

static void testIncrReadyRace(void)
{
  start();
  const unsigned long targets[] = { A_TEXT };
  discover(622, targets, ARRAY_LENGTH(targets));
  const Window window = request(213, LG_CLIPBOARD_DATA_TEXT);
  const unsigned long capacity = 4;
  prop32(x11atoms.INCR, &capacity, 1);
  selection(window, A_TEXT, x11atoms.SEL_DATA);

  const uint8_t data[] = "race";
  rec.chunkResult = LG_CLIPBOARD_RESULT_BLOCKED;
  rec.readyDuringChunk = true;
  prop8(A_TEXT, data, sizeof(data) - 1);
  property(window);
  CHECK(rec.streamChunkN == 2);
  CHECK(rec.data[0].size == sizeof(data) - 1);

  rec.endResult = LG_CLIPBOARD_RESULT_BLOCKED;
  rec.readyDuringEnd = true;
  propNull(A_TEXT, 8, 0);
  property(window);
  CHECK(rec.streamEndN == 2);
  CHECK(rec.dataN == 1);
  CHECK(rec.abortN == 0);
  finish();
}

static void * requestReadyThread(void * opaque)
{
  x11CBRequestReady(*(const LG_ClipboardRequest *)opaque);
  return NULL;
}

static void testIncrCancelRace(void)
{
  start();
  const unsigned long targets[] = { A_TEXT };
  discover(623, targets, ARRAY_LENGTH(targets));
  const LG_ClipboardRequest id = 214;
  const Window window = request(id, LG_CLIPBOARD_DATA_TEXT);
  const unsigned long capacity = 6;
  prop32(x11atoms.INCR, &capacity, 1);
  selection(window, A_TEXT, x11atoms.SEL_DATA);

  const uint8_t data[] = "cancel";
  rec.chunkResult = LG_CLIPBOARD_RESULT_BLOCKED;
  prop8(A_TEXT, data, sizeof(data) - 1);
  property(window);
  CHECK(rec.streamChunkN == 1);

  rec.chunkResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  pthread_mutex_lock(&readRaceLock);
  readRacePause = true;
  pthread_mutex_unlock(&readRaceLock);

  pthread_t readyThread;
  CHECK(pthread_create(&readyThread, NULL, requestReadyThread,
      (void *)&id) == 0);

  pthread_mutex_lock(&readRaceLock);
  while (!readRaceEntered)
    pthread_cond_wait(&readRaceCond, &readRaceLock);
  pthread_mutex_unlock(&readRaceLock);

  /* The callback is outside x11cb.lock and still consuming its chunk. */
  x11CBRequestCancel(id, LG_CLIPBOARD_CANCEL_REPLACED);

  pthread_mutex_lock(&readRaceLock);
  readRaceResume = true;
  pthread_cond_signal(&readRaceCond);
  pthread_mutex_unlock(&readRaceLock);

  CHECK(pthread_join(readyThread, NULL) == 0);
  CHECK(rec.streamChunkN == 2);
  CHECK(rec.data[0].size == sizeof(data) - 1);
  CHECK(memcmp(rec.data[0].data, data, sizeof(data) - 1) == 0);
  CHECK(rec.abortN == 0);
  finish();
}

static void testIncrLargeProperty(void)
{
  start();
  const unsigned long targets[] = { A_TEXT };
  discover(621, targets, ARRAY_LENGTH(targets));
  const Window window = request(212, LG_CLIPBOARD_DATA_TEXT);
  const unsigned long capacity = 70U * 1024U;
  prop32(x11atoms.INCR, &capacity, 1);
  selection(window, A_TEXT, x11atoms.SEL_DATA);

  const size_t size = 70U * 1024U;
  uint8_t * data = malloc(size);
  CHECK(data);
  for (size_t i = 0; i < size; ++i)
    data[i] = (uint8_t)i;
  prop8(A_TEXT, data, size);
  property(window);
  propNull(A_TEXT, 8, 0);
  property(window);

  CHECK(rec.streamChunkN == 2);
  CHECK(rec.streamMaxChunk == KVMFR_CLIPBOARD_REPRESENTATION_BYTES);
  CHECK(rec.dataN == 1);
  CHECK(rec.data[0].size == size);
  CHECK(memcmp(rec.data[0].data, data, size) == 0);
  free(data);
  finish();
}

static void testMalformed(void)
{
  start();
  const unsigned long targets[] = { A_TEXT };
  discover(63, targets, ARRAY_LENGTH(targets));

  Window window = request(301, LG_CLIPBOARD_DATA_TEXT);
  const unsigned long bad = 1;
  prop32(A_TEXT, &bad, 1);
  selection(window, A_TEXT, x11atoms.SEL_DATA);
  CHECK(rec.abortN == 1);
  CHECK(rec.abort[0] == 301);

  window = request(302, LG_CLIPBOARD_DATA_TEXT);
  const unsigned long capacity = 8;
  prop32(x11atoms.INCR, &capacity, 1);
  selection(window, A_TEXT, x11atoms.SEL_DATA);
  prop32(A_TEXT, &bad, 1);
  property(window);
  CHECK(rec.abortN == 2);
  CHECK(rec.abort[1] == 302);

  window = request(303, LG_CLIPBOARD_DATA_TEXT);
  propNull(A_TEXT, 8, 3);
  selection(window, A_TEXT, x11atoms.SEL_DATA);
  CHECK(rec.abortN == 3);
  CHECK(rec.abort[2] == 303);
  CHECK(rec.dataN == 0);
  finish();
}

static void testReplace(void)
{
  start();
  const unsigned long targets[] = { A_TEXT };
  discover(64, targets, ARRAY_LENGTH(targets));
  const Window first = request(401, LG_CLIPBOARD_DATA_TEXT);

  const unsigned int convertN = rec.convertN;
  owner(65);
  CHECK(rec.convertN == convertN + 1);
  CHECK(rec.abortN == 1);
  CHECK(rec.abort[0] == 401);
  CHECK(rec.window[1].window == first);
  CHECK(rec.window[1].dead);

  const Window second = request(402, LG_CLIPBOARD_DATA_TEXT);
  owner(None);
  CHECK(rec.abortN == 2);
  CHECK(rec.abort[1] == 402);
  CHECK(rec.releaseN == 1);
  bool dead = false;
  for (unsigned int i = 0; i < rec.windowN; ++i)
    if (rec.window[i].window == second)
      dead = rec.window[i].dead;
  CHECK(dead);

  x11CBRequest(403, LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.abortN == 3);
  CHECK(rec.abort[2] == 403);
  finish();
}

static void testSource(void)
{
  start();
  x11CBNotice(LG_CLIPBOARD_DATA_PNG);
  CHECK(rec.owner == x11.window);

  selectionRequest(x11atoms.TARGETS, 700);
  CHECK(rec.changeN == 1);
  CHECK(rec.change[0].window == 900);
  CHECK(rec.change[0].property == 700);
  CHECK(rec.change[0].type == XA_ATOM);
  CHECK(rec.change[0].format == 32);
  CHECK(rec.change[0].count == 2);
  const Atom * values = (const Atom *)rec.change[0].data;
  CHECK(values[0] == x11atoms.TARGETS);
  CHECK(values[1] == A_PNG);
  CHECK(rec.sendN == 1);
  CHECK(rec.send[0].xselection.property == 700);

  selectionRequest(A_PNG, 701);
  CHECK(rec.requestN == 1);
  CHECK(rec.requestType == LG_CLIPBOARD_DATA_PNG);
  CHECK(rec.requestStream);
  CHECK(rec.sendN == 1);
  const uint8_t data[] = { 1, 2, 3, 4 };
  CHECK(rec.requestStream->begin(rec.requestOpaque,
      LG_CLIPBOARD_DATA_PNG, LG_CLIPBOARD_SIZE_UNKNOWN) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.changeN == 2);
  CHECK(rec.change[1].property == 701);
  CHECK(rec.change[1].type == x11atoms.INCR);
  CHECK(rec.change[1].format == 32);
  CHECK(rec.change[1].count == 1);
  CHECK(rec.sendN == 2);
  CHECK(rec.send[1].xselection.property == 701);

  CHECK(rec.requestStream->chunk(rec.requestOpaque, 0,
      data, ARRAY_LENGTH(data)) == LG_CLIPBOARD_RESULT_BLOCKED);
  propertyState(900, 701, PropertyDelete);
  CHECK(rec.requestReadyN == 1);
  CHECK(rec.requestStream->chunk(rec.requestOpaque, 0,
      data, ARRAY_LENGTH(data)) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.changeN == 3);
  CHECK(rec.change[2].property == 701);
  CHECK(rec.change[2].type == A_PNG);
  CHECK(rec.change[2].format == 8);
  CHECK(rec.change[2].count == 4);
  CHECK(memcmp(rec.change[2].data, data, sizeof(data)) == 0);
  CHECK(rec.requestStream->end(rec.requestOpaque, ARRAY_LENGTH(data)) ==
      LG_CLIPBOARD_RESULT_BLOCKED);
  propertyState(900, 701, PropertyDelete);
  CHECK(rec.requestReadyN == 2);
  CHECK(rec.requestStream->end(rec.requestOpaque, ARRAY_LENGTH(data)) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.changeN == 4);
  CHECK(rec.change[3].property == 701);
  CHECK(rec.change[3].type == A_PNG);
  CHECK(rec.change[3].format == 8);
  CHECK(rec.change[3].count == 0);

  rec.requestCancelSync = true;
  selectionRequest(A_PNG, 704);
  CHECK(rec.requestN == 2);
  CHECK(rec.sendN == 3);
  CHECK(rec.send[2].xselection.property == None);
  rec.requestCancelSync = false;

  selectionRequest(A_TEXT, 702);
  CHECK(rec.requestN == 2);
  CHECK(rec.sendN == 4);
  CHECK(rec.send[3].xselection.property == None);

  rec.requestOK = false;
  selectionRequest(A_PNG, 703);
  CHECK(rec.requestN == 3);
  CHECK(rec.sendN == 5);
  CHECK(rec.send[4].xselection.property == None);

  x11CBRelease();
  CHECK(rec.owner == None);
  finish();
}

static void testSourceBeginDuringRequest(void)
{
  start();
  x11CBNotice(LG_CLIPBOARD_DATA_PNG);
  rec.requestBeginSync = true;
  selectionRequest(A_PNG, 705);
  CHECK(rec.requestN == 1);
  CHECK(rec.requestBeginResult == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.requestOpaque);
  CHECK(rec.changeN == 1);
  CHECK(rec.change[0].type == x11atoms.INCR);
  CHECK(rec.sendN == 1);

  propertyState(900, 705, PropertyDelete);
  CHECK(rec.requestStream->end(rec.requestOpaque, 0) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.changeN == 2);
  CHECK(rec.change[1].count == 0);
  finish();
}

static void testTeardown(void)
{
  start();
  const unsigned int convertN = rec.convertN;
  owner(66);
  CHECK(rec.convertN == convertN + 1);
  const Window targets = rec.convert[convertN].requestor;
  const Window read = request(501, LG_CLIPBOARD_DATA_TEXT);
  x11CBNotice(LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.abortN == 1);
  CHECK(rec.abort[0] == 501);
  CHECK(rec.owner == x11.window);
  CHECK(rec.destroyN == 2);

  bool targetsDead = false;
  bool readDead    = false;
  for (unsigned int i = 0; i < rec.windowN; ++i)
  {
    if (rec.window[i].window == targets)
      targetsDead = rec.window[i].dead;
    if (rec.window[i].window == read)
      readDead = rec.window[i].dead;
  }
  CHECK(targetsDead);
  CHECK(readDead);

  x11CBRelease();
  CHECK(rec.owner == None);
  finish();
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "targets"                   , testTargets                     },
  { "file-import"               , testFileImport                  },
  { "file-incr"                 , testFileImportIncr              },
  { "file-source"               , testFileSource                  },
  { "file-source-active-replace", testFileSourceActiveReplacement },
  { "normal"                    , testNormal                      },
  { "incr"                      , testIncr                        },
  { "incr-block"                , testIncrBlocked                 },
  { "incr-ready"                , testIncrReadyRace               },
  { "incr-cancel"               , testIncrCancelRace              },
  { "incr-large"                , testIncrLargeProperty           },
  { "malformed"                 , testMalformed                   },
  { "replace"                   , testReplace                     },
  { "source"                    , testSource                      },
  { "source-early"              , testSourceBeginDuringRequest    },
  { "teardown"                  , testTeardown                    },
};

int main(int argc, char ** argv)
{
  debug_init();

  if (argc == 2)
  {
    for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i)
      if (strcmp(argv[1], tests[i].name) == 0)
      {
        tests[i].run();
        return 0;
      }

    fprintf(stderr, "unknown test: %s\n", argv[1]);
    return EXIT_FAILURE;
  }

  if (argc != 1)
    return EXIT_FAILURE;

  for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i)
    tests[i].run();
  return 0;
}
