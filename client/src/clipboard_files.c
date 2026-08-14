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

#define FUSE_USE_VERSION 310

#include "core/clipboard_files.h"
#include "core/clipboard.h"

#include "common/KVMFRClipboard.h"
#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"
#include "common/thread.h"
#include "common/time.h"

#include <fuse3/fuse_lowlevel.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#define FILE_WORKERS             4U
#define FILE_ATTR_TIMEOUT        1.0
#define FILE_ENTRY_TIMEOUT       1.0
#define FILE_REQUEST_MAX         KVMFR_CLIPBOARD_FILE_READ_BYTES
#define FILE_STREAM_CHUNK        KVMFR_CLIPBOARD_DATA_BYTES
#define FILE_DELIVERY_GRACE_US   UINT64_C(30000000)
#define FILE_DELIVERY_GRACE_MAX  4U
#define FILE_URI_PREFIX          "file://"
#define FILE_GNOME_MIME          "x-special/gnome-copied-files"
#define FILE_URI_MIME            "text/uri-list"
#define FILE_KDE_CUT_MIME        "application/x-kde-cutselection"

_Static_assert(FILE_REQUEST_MAX <= UINT32_MAX,
    "clipboard FUSE read size exceeds the FUSE protocol field");

enum RemoteReply
{
  REMOTE_REPLY_INITIAL,
  REMOTE_REPLY_LOOKUP,
  REMOTE_REPLY_READDIR,
  REMOTE_REPLY_READ,
};

struct RemoteDataset;

struct RemoteNode
{
  struct RemoteNode    * next;
  struct RemoteDataset * dataset;
  fuse_ino_t             ino;
  fuse_ino_t             parent;
  uint64_t               node;
  uint64_t               size;
  uint64_t               createdNs;
  uint64_t               modifiedNs;
  KVMFRClipboardFileType type;
  uint64_t               lookups;
  bool                   listed;
  char                 * name;
};

struct RemoteDataset
{
  struct RemoteDataset * next;
  uint64_t               dataset;
  uint64_t               acquisition;
  fuse_ino_t             ino;
  uint64_t               lookupPins;
  uint64_t               openPins;
  uint64_t               requestPins;
  uint64_t               operationPins;
  uint64_t               deliveryDeadline;
  unsigned               presentationPins;
  bool                   acquiring;
  bool                   acquired;
  bool                   ready;
  bool                   current;
  bool                   stale;
  bool                   deliveryPending;
  bool                   pruneQueued;
  char                   name[40];
};

struct RemoteRequest
{
  struct RemoteRequest       * next;
  LG_ClipboardFileRequest      request;
  struct RemoteDataset       * dataset;
  struct RemoteNode          * directory;
  enum RemoteReply             reply;
  fuse_req_t                   fuseRequest;
  size_t                       fuseSize;
  off_t                        fuseOffset;
  char                       * lookupName;
  uint8_t                    * data;
  size_t                       dataSize;
  size_t                       dataCapacity;
  uint64_t                     sizeHint;
  bool                         began;
  bool                         transportStarted;
};

enum RemoteActionType
{
  REMOTE_ACTION_NONE,
  REMOTE_ACTION_CANCEL_ACQUISITION,
  REMOTE_ACTION_RELEASE_ACQUISITION,
};

struct RemoteAction
{
  enum RemoteActionType   type;
  struct RemoteDataset  * object;
  uint64_t                dataset;
  uint64_t                acquisition;
};

struct LocalDataset;

struct LocalNode
{
  struct LocalNode    * next;
  struct LocalDataset * dataset;
  struct LocalNode    * root;
  uint64_t              node;
  uint64_t              parent;
  uint64_t              size;
  uint64_t              createdNs;
  uint64_t              modifiedNs;
  KVMFRClipboardFileType type;
  dev_t                 device;
  ino_t                 inode;
  int                   fd;
  char                 * path;
  char                 * name;
};

struct LocalDataset
{
  struct LocalDataset * next;
  struct LocalNode    * nodes;
  uint64_t              nodeSerial;
  uint64_t              dataset;
  unsigned              references;
  bool                  current;
};

struct LocalAcquisition
{
  struct LocalAcquisition * next;
  struct LocalDataset     * dataset;
  uint64_t                  wireDataset;
  uint64_t                  acquisition;
};

struct LocalJob
{
  struct LocalJob          * next;
  struct LocalDataset      * dataset;
  LG_ClipboardFileRequest    request;
  LGEvent                  * ready;
  atomic_bool                cancelled;
};

static struct
{
  LG_Lock                    lock;
  struct fuse_session      * session;
  LGThread                 * fuseThread;
  int                        fuseStopFd;
  char                       mountpoint[PATH_MAX];
  bool                       initialized;
  bool                       stopping;
  fuse_ino_t                 inodeSerial;
  uint64_t                   requestSerial;
  uint64_t                   datasetSerial;
  struct RemoteDataset     * remoteDatasets;
  struct RemoteDataset     * remoteCurrent;
  struct RemoteNode        * remoteNodes;
  struct RemoteRequest     * remoteRequests;
  struct LocalDataset      * localDatasets;
  struct LocalDataset      * localCurrent;
  struct LocalAcquisition * localAcquisitions;
  struct LocalJob          * jobsHead;
  struct LocalJob         ** jobsTail;
  struct LocalJob          * activeJobs;
  LGEvent                  * jobEvent;
  LGThread                 * workers[FILE_WORKERS];
}
files;

static bool filesLockInitialized;

#ifdef ENABLE_TESTS
static atomic_bool forceLocalEof;
#endif

static bool randomBytes(void * buffer, size_t size)
{
  uint8_t * bytes = buffer;
  size_t offset = 0;
  while (offset < size)
  {
    const ssize_t result = getrandom(bytes + offset, size - offset, 0);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
    {
      if (!result)
        errno = EIO;
      return false;
    }
    offset += (size_t)result;
  }
  return true;
}

static uint64_t nsFromTimespec(struct timespec value)
{
  if (value.tv_sec < 0 || value.tv_nsec < 0)
    return 0;
  if ((uint64_t)value.tv_sec > UINT64_MAX / UINT64_C(1000000000))
    return UINT64_MAX;
  return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
    (uint64_t)value.tv_nsec;
}

static struct timespec timespecFromNs(uint64_t value)
{
  return (struct timespec)
  {
    .tv_sec  = (time_t)(value / UINT64_C(1000000000)),
    .tv_nsec = (long)(value % UINT64_C(1000000000)),
  };
}

static uint64_t nextRequestNL(void)
{
  do
    ++files.requestSerial;
  while (!files.requestSerial ||
      kvmfrClipboardTransferFromHelper(files.requestSerial));
  return files.requestSerial;
}

static uint64_t nextAcquisitionNL(void)
{
  return nextRequestNL();
}

static uint64_t nextDatasetNL(void)
{
  do
    ++files.datasetSerial;
  while (!files.datasetSerial ||
      kvmfrClipboardTransferFromHelper(files.datasetSerial));
  return files.datasetSerial;
}

static fuse_ino_t nextInodeNL(void)
{
  if (++files.inodeSerial <= FUSE_ROOT_ID)
    files.inodeSerial = FUSE_ROOT_ID + 1;
  return files.inodeSerial;
}

static int errnoFromFileError(LG_ClipboardFileError error)
{
  switch (error)
  {
    case LG_CLIPBOARD_FILE_ERROR_NONE:          return 0;
    case LG_CLIPBOARD_FILE_ERROR_NOT_FOUND:     return ENOENT;
    case LG_CLIPBOARD_FILE_ERROR_ACCESS:        return EACCES;
    case LG_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY: return ENOTDIR;
    case LG_CLIPBOARD_FILE_ERROR_IS_DIRECTORY:  return EISDIR;
    case LG_CLIPBOARD_FILE_ERROR_NO_MEMORY:     return ENOMEM;
    case LG_CLIPBOARD_FILE_ERROR_NO_SPACE:      return ENOSPC;
    case LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED: return ENOTSUP;
    case LG_CLIPBOARD_FILE_ERROR_CANCELLED:     return EINTR;
    case LG_CLIPBOARD_FILE_ERROR_STALE:         return ESTALE;
    default:                                       return EIO;
  }
}

static const char * fileErrorName(LG_ClipboardFileError error)
{
  switch (error)
  {
    case LG_CLIPBOARD_FILE_ERROR_NONE:          return "none";
    case LG_CLIPBOARD_FILE_ERROR_NOT_FOUND:     return "not found";
    case LG_CLIPBOARD_FILE_ERROR_ACCESS:        return "access denied";
    case LG_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY: return "not a directory";
    case LG_CLIPBOARD_FILE_ERROR_IS_DIRECTORY:  return "is a directory";
    case LG_CLIPBOARD_FILE_ERROR_IO:            return "I/O error";
    case LG_CLIPBOARD_FILE_ERROR_INVALID:       return "invalid";
    case LG_CLIPBOARD_FILE_ERROR_NO_MEMORY:     return "out of memory";
    case LG_CLIPBOARD_FILE_ERROR_NO_SPACE:      return "no space";
    case LG_CLIPBOARD_FILE_ERROR_DISCONNECTED:  return "disconnected";
    case LG_CLIPBOARD_FILE_ERROR_CANCELLED:     return "cancelled";
    case LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED: return "not supported";
    case LG_CLIPBOARD_FILE_ERROR_STALE:         return "stale";
  }
  return "unknown";
}

static LG_ClipboardFileError fileErrorFromErrno(int error)
{
  switch (error)
  {
    case ENOENT:    return LG_CLIPBOARD_FILE_ERROR_NOT_FOUND;
    case EACCES:
    case EPERM:     return LG_CLIPBOARD_FILE_ERROR_ACCESS;
    case ENOTDIR:   return LG_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY;
    case EISDIR:    return LG_CLIPBOARD_FILE_ERROR_IS_DIRECTORY;
    case ENOMEM:
    case EMFILE:
    case ENFILE:    return LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
    case ENOSPC:    return LG_CLIPBOARD_FILE_ERROR_NO_SPACE;
    case ELOOP:
    case ENOTSUP:   return LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED;
    case ESTALE:    return LG_CLIPBOARD_FILE_ERROR_STALE;
    case ECANCELED: return LG_CLIPBOARD_FILE_ERROR_CANCELLED;
    default:        return LG_CLIPBOARD_FILE_ERROR_IO;
  }
}

static bool grow(void ** buffer, size_t * capacity, size_t wanted)
{
  if (wanted <= *capacity)
    return !wanted || *buffer;
  size_t next = *capacity ? *capacity : 4096U;
  while (next < wanted)
  {
    if (next > SIZE_MAX / 2U)
    {
      next = wanted;
      break;
    }
    next *= 2U;
  }
  void * resized = realloc(*buffer, next);
  if (!resized)
    return false;
  *buffer   = resized;
  *capacity = next;
  return true;
}

static struct RemoteDataset * remoteDatasetByIdNL(uint64_t dataset)
{
  for (struct RemoteDataset * item = files.remoteDatasets;
      item; item = item->next)
    if (item->dataset == dataset)
      return item;
  return NULL;
}

static struct RemoteDataset * remoteDatasetByAcquisitionNL(
    uint64_t acquisition)
{
  for (struct RemoteDataset * item = files.remoteDatasets;
      item; item = item->next)
    if (item->acquisition == acquisition)
      return item;
  return NULL;
}

static struct RemoteAction retireRemoteDatasetNL(
    struct RemoteDataset * dataset)
{
  struct RemoteAction action = { 0 };
  if (!dataset || dataset->current || dataset->presentationPins ||
      dataset->lookupPins || dataset->openPins || dataset->requestPins ||
      dataset->operationPins || dataset->deliveryPending)
    return action;

  if (!dataset->stale)
  {
    dataset->ready     = false;
    dataset->stale     = true;
    action.dataset     = dataset->dataset;
    action.acquisition = dataset->acquisition;
    if (dataset->acquired)
      action.type = REMOTE_ACTION_RELEASE_ACQUISITION;
    else if (dataset->acquiring)
      action.type = REMOTE_ACTION_CANCEL_ACQUISITION;
    dataset->acquiring = false;
    dataset->acquired  = false;
  }
  if (dataset->pruneQueued)
    return action;
  dataset->pruneQueued = true;
  action.object        = dataset;
  return action;
}

/* Retire a failed initial acquisition/list only while it is still the
 * dataset backing the current remote file offer.  The caller must hold
 * files.lock and subsequently call retireRemoteDatasetNL so an acquired
 * dataset is released before it is pruned. */
static uint64_t failCurrentRemoteDatasetNL(
    struct RemoteDataset * dataset)
{
  if (!dataset || !dataset->current)
    return 0;

  dataset->current          = false;
  dataset->ready            = false;
  dataset->deliveryPending  = false;
  dataset->deliveryDeadline = 0;
  if (files.remoteCurrent == dataset)
    files.remoteCurrent = NULL;
  return dataset->dataset;
}

static void pruneRemoteDataset(struct RemoteDataset * dataset)
{
  if (!dataset)
    return;
  LG_LOCK(files.lock);
  if (!dataset->pruneQueued || !dataset->stale || dataset->current ||
      dataset->presentationPins || dataset->lookupPins ||
      dataset->openPins || dataset->requestPins ||
      dataset->operationPins || dataset->deliveryPending)
  {
    dataset->pruneQueued = false;
    LG_UNLOCK(files.lock);
    return;
  }

  struct RemoteDataset ** datasetLink = &files.remoteDatasets;
  while (*datasetLink && *datasetLink != dataset)
    datasetLink = &(*datasetLink)->next;
  if (!*datasetLink)
  {
    LG_UNLOCK(files.lock);
    return;
  }
  *datasetLink = dataset->next;
  if (files.remoteCurrent == dataset)
    files.remoteCurrent = NULL;

  struct RemoteNode * retired = NULL;
  struct RemoteNode ** nodeLink = &files.remoteNodes;
  while (*nodeLink)
  {
    struct RemoteNode * node = *nodeLink;
    if (node->dataset != dataset)
    {
      nodeLink = &node->next;
      continue;
    }
    *nodeLink = node->next;
    node->next = retired;
    retired = node;
  }
  LG_UNLOCK(files.lock);

  while (retired)
  {
    struct RemoteNode * next = retired->next;
    free(retired->name);
    free(retired);
    retired = next;
  }
  free(dataset);
}

static void performRemoteAction(struct RemoteAction action)
{
  switch (action.type)
  {
    case REMOTE_ACTION_CANCEL_ACQUISITION:
      if (!lgClipboard_fileCancel(action.dataset, action.acquisition,
            LG_CLIPBOARD_FILE_ERROR_CANCELLED))
        DEBUG_WARN("Failed to cancel retired clipboard file acquisition");
      break;

    case REMOTE_ACTION_RELEASE_ACQUISITION:
      if (!lgClipboard_fileRelease(action.dataset, action.acquisition))
        DEBUG_WARN("Failed to release retired clipboard file acquisition");
      break;

    case REMOTE_ACTION_NONE:
      break;
  }
  pruneRemoteDataset(action.object);
}

static bool pinRemoteOperationNL(
    struct RemoteDataset * dataset, bool consumeDelivery)
{
  if (!dataset || dataset->stale)
    return false;
  ++dataset->operationPins;
  if (consumeDelivery)
  {
    dataset->deliveryPending  = false;
    dataset->deliveryDeadline = 0;
  }
  return true;
}

static void releaseRemoteOperation(struct RemoteDataset * dataset)
{
  if (!dataset)
    return;
  LG_LOCK(files.lock);
  if (dataset->operationPins)
    --dataset->operationPins;
  const struct RemoteAction action = retireRemoteDatasetNL(dataset);
  LG_UNLOCK(files.lock);
  performRemoteAction(action);
}

static void reapRemoteDeliveries(bool pressure)
{
  for (;;)
  {
    const uint64_t now = microtime();
    LG_LOCK(files.lock);
    struct RemoteDataset * candidate = NULL;
    struct RemoteDataset * oldest = NULL;
    unsigned pending = 0;
    for (struct RemoteDataset * dataset = files.remoteDatasets;
        dataset; dataset = dataset->next)
    {
      if (!dataset->deliveryPending)
        continue;
      if (dataset->deliveryDeadline <= now)
      {
        candidate = dataset;
        break;
      }
      if (dataset->current)
        continue;
      ++pending;
      if (!oldest || dataset->deliveryDeadline < oldest->deliveryDeadline)
        oldest = dataset;
    }
    if (!candidate && pressure && pending > FILE_DELIVERY_GRACE_MAX)
      candidate = oldest;
    if (!candidate)
    {
      LG_UNLOCK(files.lock);
      return;
    }
    candidate->deliveryPending  = false;
    candidate->deliveryDeadline = 0;
    const struct RemoteAction action = retireRemoteDatasetNL(candidate);
    LG_UNLOCK(files.lock);
    performRemoteAction(action);
  }
}

static int nextRemoteDeliveryTimeout(void)
{
  const uint64_t now = microtime();
  uint64_t deadline = UINT64_MAX;
  LG_LOCK(files.lock);
  for (struct RemoteDataset * dataset = files.remoteDatasets;
      dataset; dataset = dataset->next)
    if (dataset->deliveryPending &&
        dataset->deliveryDeadline < deadline)
      deadline = dataset->deliveryDeadline;
  LG_UNLOCK(files.lock);
  if (deadline == UINT64_MAX)
    return -1;
  if (deadline <= now)
    return 0;
  const uint64_t delta = deadline - now;
  const uint64_t milliseconds = delta / UINT64_C(1000) +
    (delta % UINT64_C(1000) != 0);
  return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static void pruneUnusedRemoteDatasets(void)
{
  for (;;)
  {
    LG_LOCK(files.lock);
    struct RemoteDataset * dataset = files.remoteDatasets;
    while (dataset && (dataset->current || dataset->presentationPins ||
        dataset->lookupPins || dataset->openPins || dataset->requestPins ||
        dataset->operationPins || dataset->deliveryPending ||
        dataset->pruneQueued))
      dataset = dataset->next;
    const struct RemoteAction action = retireRemoteDatasetNL(dataset);
    LG_UNLOCK(files.lock);
    if (!action.object)
      return;
    performRemoteAction(action);
  }
}

static struct RemoteNode * remoteNodeByInodeNL(fuse_ino_t ino)
{
  for (struct RemoteNode * node = files.remoteNodes; node; node = node->next)
    if (node->ino == ino)
      return node;
  return NULL;
}

static struct RemoteNode * remoteChildNL(struct RemoteDataset * dataset,
    fuse_ino_t parent, const char * name)
{
  for (struct RemoteNode * node = files.remoteNodes; node; node = node->next)
    if (node->dataset == dataset && node->parent == parent &&
        !strcmp(node->name, name))
      return node;
  return NULL;
}

static struct RemoteNode * remoteNodeByWireNL(
    struct RemoteDataset * dataset, uint64_t wireNode)
{
  for (struct RemoteNode * node = files.remoteNodes; node; node = node->next)
    if (node->dataset == dataset && node->node == wireNode)
      return node;
  return NULL;
}

static bool validUtf8(const uint8_t * value, size_t length)
{
  for (size_t i = 0; i < length;)
  {
    const uint8_t first = value[i++];
    if (first < 0x80)
      continue;
    unsigned continuation;
    uint32_t codepoint;
    uint32_t minimum;
    if ((first & 0xe0U) == 0xc0U)
    {
      continuation = 1;
      codepoint = first & 0x1fU;
      minimum = 0x80;
    }
    else if ((first & 0xf0U) == 0xe0U)
    {
      continuation = 2;
      codepoint = first & 0x0fU;
      minimum = 0x800;
    }
    else if ((first & 0xf8U) == 0xf0U)
    {
      continuation = 3;
      codepoint = first & 0x07U;
      minimum = 0x10000;
    }
    else
      return false;
    if (continuation > length - i)
      return false;
    while (continuation--)
    {
      const uint8_t next = value[i++];
      if ((next & 0xc0U) != 0x80U)
        return false;
      codepoint = (codepoint << 6) | (next & 0x3fU);
    }
    if (codepoint < minimum || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU))
      return false;
  }
  return true;
}

static struct RemoteRequest * remoteRequestTakeNL(uint64_t request)
{
  struct RemoteRequest ** link = &files.remoteRequests;
  while (*link && (*link)->request.request != request)
    link = &(*link)->next;
  if (!*link)
    return NULL;
  struct RemoteRequest * result = *link;
  *link = result->next;
  result->next = NULL;
  return result;
}

static struct RemoteRequest * remoteRequestTakePointerNL(
    struct RemoteRequest * request)
{
  struct RemoteRequest ** link = &files.remoteRequests;
  while (*link && *link != request)
    link = &(*link)->next;
  if (!*link)
    return NULL;
  *link = request->next;
  request->next = NULL;
  return request;
}

static struct RemoteAction completeRemoteRequestNL(
    struct RemoteRequest * request)
{
  if (request && request->dataset->requestPins)
    --request->dataset->requestPins;
  return request ? retireRemoteDatasetNL(request->dataset) :
    (struct RemoteAction) { 0 };
}

static void freeRemoteRequest(struct RemoteRequest * request)
{
  if (!request)
    return;
  free(request->lookupName);
  free(request->data);
  free(request);
}

static void unregisterRemoteInterrupt(struct RemoteRequest * request)
{
  if (request && request->fuseRequest)
    fuse_req_interrupt_func(request->fuseRequest, NULL, NULL);
}

static void failRemoteRequest(struct RemoteRequest * request, int error)
{
  if (!request)
    return;
  if (request->reply == REMOTE_REPLY_INITIAL)
    DEBUG_WARN("Remote clipboard file root list failed: "
        "dataset=%" PRIu64 ", request=%" PRIu64 ", error=%d (%s)",
        request->request.dataset, request->request.request,
        error, strerror(error));
  unregisterRemoteInterrupt(request);
  if (request->fuseRequest)
    fuse_reply_err(request->fuseRequest, error);
  LG_LOCK(files.lock);
  const uint64_t failedDataset = request->reply == REMOTE_REPLY_INITIAL ?
    failCurrentRemoteDatasetNL(request->dataset) : 0;
  const struct RemoteAction action = completeRemoteRequestNL(request);
  LG_UNLOCK(files.lock);
  freeRemoteRequest(request);
  performRemoteAction(action);
  if (failedDataset)
    lgClipboard_fileRemoteFailed(failedDataset);
}

static void completeRemoteRequest(struct RemoteRequest * request)
{
  LG_LOCK(files.lock);
  const struct RemoteAction action = completeRemoteRequestNL(request);
  LG_UNLOCK(files.lock);
  freeRemoteRequest(request);
  performRemoteAction(action);
}

static void fillStat(const struct RemoteNode * node, struct stat * value)
{
  memset(value, 0, sizeof(*value));
  value->st_ino   = node->ino;
  value->st_mode  = node->type == KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY ?
    S_IFDIR | 0500 : S_IFREG | 0400;
  value->st_nlink = node->type == KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY ? 2 : 1;
  value->st_uid   = getuid();
  value->st_gid   = getgid();
  value->st_size  = node->size > INT64_MAX ? INT64_MAX : (off_t)node->size;
  value->st_mtim  = timespecFromNs(node->modifiedNs);
  value->st_ctim  = value->st_mtim;
  value->st_atim  = value->st_mtim;
}

static void replyEntry(fuse_req_t request, struct RemoteNode * node)
{
  struct fuse_entry_param entry =
  {
    .ino           = node->ino,
    .generation    = 1,
    .attr_timeout  = FILE_ATTR_TIMEOUT,
    .entry_timeout = FILE_ENTRY_TIMEOUT,
  };
  fillStat(node, &entry.attr);
  LG_LOCK(files.lock);
  const bool available = !node->dataset->stale;
  if (available)
  {
    ++node->lookups;
    ++node->dataset->lookupPins;
  }
  LG_UNLOCK(files.lock);
  if (!available)
  {
    fuse_reply_err(request, ESTALE);
    return;
  }
  if (fuse_reply_entry(request, &entry) == 0)
    return;

  LG_LOCK(files.lock);
  if (node->lookups)
    --node->lookups;
  if (node->dataset->lookupPins)
    --node->dataset->lookupPins;
  const struct RemoteAction action = retireRemoteDatasetNL(node->dataset);
  LG_UNLOCK(files.lock);
  performRemoteAction(action);
}

static bool installRemoteListNL(struct RemoteRequest * request)
{
  struct RemoteNode * additions = NULL;
  size_t offset = 0;
  while (offset < request->dataSize)
  {
    if (request->dataSize - offset < sizeof(KVMFRClipboardFileEntry))
      goto invalid;
    KVMFRClipboardFileEntry entry;
    memcpy(&entry, request->data + offset, sizeof(entry));
    if (!entry.node || !entry.nameLength || entry.nameLength > NAME_MAX ||
        (entry.type != KVMFR_CLIPBOARD_FILE_TYPE_REGULAR &&
         entry.type != KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY))
      goto invalid;
    const uint64_t bytes64 = KVMFR_CLIPBOARD_FILE_ENTRY_BYTES(
        entry.nameLength);
    if (bytes64 > SIZE_MAX || bytes64 > request->dataSize - offset)
      goto invalid;
    const uint8_t * encodedName = request->data + offset +
      sizeof(entry);
    if (memchr(encodedName, 0, entry.nameLength) ||
        !validUtf8(encodedName, entry.nameLength) ||
        (entry.nameLength == 1 && encodedName[0] == '.') ||
        (entry.nameLength == 2 && encodedName[0] == '.' &&
         encodedName[1] == '.') ||
        memchr(encodedName, '/', entry.nameLength))
      goto invalid;
    for (size_t i = sizeof(entry) + entry.nameLength;
        i < (size_t)bytes64; ++i)
      if (request->data[offset + i])
        goto invalid;
    char * name = malloc((size_t)entry.nameLength + 1U);
    if (!name)
      goto invalid;
    memcpy(name, encodedName, entry.nameLength);
    name[entry.nameLength] = 0;

    struct RemoteNode * existing = remoteChildNL(
        request->dataset, request->directory->ino, name);
    struct RemoteNode * wireNode = remoteNodeByWireNL(
        request->dataset, entry.node);
    for (struct RemoteNode * node = additions; node; node = node->next)
    {
      if (node->parent == request->directory->ino &&
          !strcmp(node->name, name))
        existing = node;
      if (node->node == entry.node)
        wireNode = node;
    }

    if (existing || wireNode)
    {
      const bool same = existing && existing == wireNode &&
        existing->size == entry.size &&
        existing->createdNs == entry.createdNs &&
        existing->modifiedNs == entry.modifiedNs &&
        existing->type == entry.type;
      free(name);
      if (!same)
        goto invalid;
    }
    else
    {
      struct RemoteNode * node = calloc(1, sizeof(*node));
      if (!node)
      {
        free(name);
        goto invalid;
      }
      *node = (struct RemoteNode)
      {
        .dataset    = request->dataset,
        .ino        = nextInodeNL(),
        .parent     = request->directory->ino,
        .node       = entry.node,
        .size       = entry.size,
        .createdNs  = entry.createdNs,
        .modifiedNs = entry.modifiedNs,
        .type       = entry.type,
        .name       = name,
        .next       = additions,
      };
      additions = node;
    }
    offset += (size_t)bytes64;
  }
  while (additions)
  {
    struct RemoteNode * node = additions;
    additions = node->next;
    node->next = files.remoteNodes;
    files.remoteNodes = node;
  }
  request->directory->listed = true;
  return true;

invalid:
  while (additions)
  {
    struct RemoteNode * next = additions->next;
    free(additions->name);
    free(additions);
    additions = next;
  }
  return false;
}

static void replyDirectory(struct RemoteRequest * request)
{
  char * buffer = malloc(request->fuseSize ? request->fuseSize : 1U);
  if (!buffer)
  {
    fuse_reply_err(request->fuseRequest, ENOMEM);
    return;
  }
  size_t used = 0;
  off_t index = 0;
  struct stat attr;
  if (request->fuseOffset <= index)
  {
    fillStat(request->directory, &attr);
    const size_t entry = fuse_add_direntry(request->fuseRequest,
        buffer + used, request->fuseSize - used, ".", &attr, index + 1);
    if (entry > request->fuseSize - used)
      goto done;
    used += entry;
  }
  ++index;
  if (request->fuseOffset <= index)
  {
    LG_LOCK(files.lock);
    struct RemoteNode * parent = remoteNodeByInodeNL(
        request->directory->parent);
    fillStat(parent ? parent : request->directory, &attr);
    LG_UNLOCK(files.lock);
    const size_t entry = fuse_add_direntry(request->fuseRequest,
        buffer + used, request->fuseSize - used, "..", &attr, index + 1);
    if (entry > request->fuseSize - used)
      goto done;
    used += entry;
  }
  LG_LOCK(files.lock);
  for (struct RemoteNode * node = files.remoteNodes; node; node = node->next)
  {
    if (node->dataset != request->dataset ||
        node->parent != request->directory->ino)
      continue;
    ++index;
    if (request->fuseOffset > index)
      continue;
    fillStat(node, &attr);
    const size_t entry = fuse_add_direntry(request->fuseRequest,
        buffer + used, request->fuseSize - used,
        node->name, &attr, index + 1);
    if (entry > request->fuseSize - used)
      break;
    used += entry;
  }
  LG_UNLOCK(files.lock);

done:
  fuse_reply_buf(request->fuseRequest, buffer, used);
  free(buffer);
}

static void finishRemoteRequest(struct RemoteRequest * request)
{
  unregisterRemoteInterrupt(request);
  bool installed = true;
  if (request->request.operation == LG_CLIPBOARD_FILE_LIST)
  {
    LG_LOCK(files.lock);
    installed = installRemoteListNL(request);
    if (installed && request->reply == REMOTE_REPLY_INITIAL)
    {
      request->dataset->ready = true;
      const bool current = request->dataset->current;
      const uint64_t datasetId = request->dataset->dataset;
      const uint64_t requestId = request->request.request;
      LG_UNLOCK(files.lock);
      if (current)
      {
        DEBUG_INFO("Remote clipboard file root list ready; publishing: "
            "dataset=%" PRIu64 ", request=%" PRIu64,
            datasetId, requestId);
        lgClipboard_fileRemoteReady(datasetId);
      }
      completeRemoteRequest(request);
      return;
    }
    if (installed && request->reply == REMOTE_REPLY_LOOKUP)
    {
      struct RemoteNode * node = remoteChildNL(request->dataset,
          request->directory->ino, request->lookupName);
      LG_UNLOCK(files.lock);
      if (node)
        replyEntry(request->fuseRequest, node);
      else
        fuse_reply_err(request->fuseRequest, ENOENT);
      completeRemoteRequest(request);
      return;
    }
    LG_UNLOCK(files.lock);
    if (!installed && request->reply == REMOTE_REPLY_INITIAL)
    {
      failRemoteRequest(request, EIO);
      return;
    }
    if (installed && request->reply == REMOTE_REPLY_READDIR)
      replyDirectory(request);
    else if (request->fuseRequest)
      fuse_reply_err(request->fuseRequest, EIO);
  }
  else if (request->reply == REMOTE_REPLY_READ)
  {
    if (request->dataSize > request->request.length)
      fuse_reply_err(request->fuseRequest, EIO);
    else
      fuse_reply_buf(request->fuseRequest,
          (const char *)request->data, request->dataSize);
  }
  completeRemoteRequest(request);
}

static void interruptRemoteRequest(fuse_req_t fuseRequest, void * opaque)
{
  struct RemoteRequest * request = opaque;
  LG_LOCK(files.lock);
  request = remoteRequestTakePointerNL(request);
  const struct RemoteAction action = completeRemoteRequestNL(request);
  LG_UNLOCK(files.lock);
  if (!request)
    return;

  const LG_ClipboardFileRequest descriptor = request->request;
  if (request->transportStarted &&
      !lgClipboard_fileCancel(descriptor.dataset, descriptor.request,
        LG_CLIPBOARD_FILE_ERROR_CANCELLED))
    DEBUG_WARN("Failed to cancel interrupted clipboard file request");
  fuse_reply_err(fuseRequest, EINTR);
  freeRemoteRequest(request);
  performRemoteAction(action);
}

/* Returns true when another path already took ownership of the request. */
static bool finishUnsentRemoteRequest(uint64_t requestId)
{
  LG_LOCK(files.lock);
  struct RemoteRequest * failed = remoteRequestTakeNL(requestId);
  LG_UNLOCK(files.lock);
  if (!failed)
    return true;
  failRemoteRequest(failed, EIO);
  return false;
}

static bool requestRemote(struct RemoteDataset * dataset,
    struct RemoteNode * node, LG_ClipboardFileOperation operation,
    uint64_t offset, uint32_t length, enum RemoteReply reply,
    fuse_req_t fuseRequest, size_t fuseSize, off_t fuseOffset,
    const char * lookupName)
{
  LG_LOCK(files.lock);
  const bool stale = dataset->stale;
  LG_UNLOCK(files.lock);
  if (stale)
    return false;
  struct RemoteRequest * request = calloc(1, sizeof(*request));
  if (!request)
    return false;
  if (lookupName && !(request->lookupName = strdup(lookupName)))
  {
    free(request);
    return false;
  }
  LG_LOCK(files.lock);
  if (dataset->stale ||
      (reply == REMOTE_REPLY_INITIAL && !dataset->current))
  {
    LG_UNLOCK(files.lock);
    freeRemoteRequest(request);
    return false;
  }
  request->request     = (LG_ClipboardFileRequest)
  {
    .dataset   = dataset->dataset,
    .request   = nextRequestNL(),
    .node      = node->node,
    .offset    = offset,
    .length    = length,
    .operation = operation,
  };
  request->dataset     = dataset;
  request->directory   = node;
  request->reply       = reply;
  request->fuseRequest = fuseRequest;
  request->fuseSize    = fuseSize;
  request->fuseOffset  = fuseOffset;
  request->sizeHint    = KVMFR_CLIPBOARD_SIZE_UNKNOWN;
  request->next        = files.remoteRequests;
  files.remoteRequests = request;
  ++dataset->requestPins;
  LG_UNLOCK(files.lock);
  const LG_ClipboardFileRequest descriptor = request->request;
  if (fuseRequest)
    fuse_req_interrupt_func(fuseRequest, interruptRemoteRequest, request);

  LG_LOCK(files.lock);
  struct RemoteRequest * live = files.remoteRequests;
  while (live && live != request)
    live = live->next;
  const bool sent = live && lgClipboard_fileRequest(&descriptor);
  if (sent)
    request->transportStarted = true;
  LG_UNLOCK(files.lock);
  if (!live)
    return true;
  if (sent)
    return true;
  return finishUnsentRemoteRequest(descriptor.request);
}

static struct RemoteNode * datasetRootNL(struct RemoteDataset * dataset)
{
  return remoteNodeByInodeNL(dataset->ino);
}

static void fuseLookup(fuse_req_t request, fuse_ino_t parent,
    const char * name)
{
  LG_LOCK(files.lock);
  if (parent == FUSE_ROOT_ID)
  {
    for (struct RemoteDataset * dataset = files.remoteDatasets;
        dataset; dataset = dataset->next)
      if (dataset->ready && !dataset->stale &&
          !strcmp(dataset->name, name))
      {
        struct RemoteNode * node = datasetRootNL(dataset);
        const bool pinned = pinRemoteOperationNL(dataset, true);
        LG_UNLOCK(files.lock);
        if (pinned)
          replyEntry(request, node);
        else
          fuse_reply_err(request, ESTALE);
        releaseRemoteOperation(pinned ? dataset : NULL);
        return;
      }
    LG_UNLOCK(files.lock);
    fuse_reply_err(request, ENOENT);
    return;
  }
  struct RemoteNode * directory = remoteNodeByInodeNL(parent);
  const int error = !directory ? ENOENT :
    directory->dataset->stale ? ESTALE :
    directory->type != KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY ? ENOTDIR : 0;
  if (error)
  {
    LG_UNLOCK(files.lock);
    fuse_reply_err(request, error);
    return;
  }
  struct RemoteNode * child = remoteChildNL(directory->dataset, parent, name);
  const bool listed = directory->listed;
  struct RemoteDataset * dataset = directory->dataset;
  const bool pinned = pinRemoteOperationNL(dataset, false);
  LG_UNLOCK(files.lock);
  if (!pinned)
  {
    fuse_reply_err(request, ESTALE);
    return;
  }
  if (child)
  {
    replyEntry(request, child);
    releaseRemoteOperation(dataset);
    return;
  }
  if (listed)
  {
    fuse_reply_err(request, ENOENT);
    releaseRemoteOperation(dataset);
    return;
  }
  if (!requestRemote(dataset, directory,
        LG_CLIPBOARD_FILE_LIST, 0, 0, REMOTE_REPLY_LOOKUP,
        request, 0, 0, name))
    fuse_reply_err(request, EIO);
  releaseRemoteOperation(dataset);
}

static void fillRootStat(struct stat * value)
{
  memset(value, 0, sizeof(*value));
  value->st_ino   = FUSE_ROOT_ID;
  value->st_mode  = S_IFDIR | 0500;
  value->st_nlink = 2;
  value->st_uid   = getuid();
  value->st_gid   = getgid();
}

static void fuseGetattr(fuse_req_t request, fuse_ino_t ino,
    struct fuse_file_info * info)
{
  (void)info;
  struct stat value;
  if (ino == FUSE_ROOT_ID)
    fillRootStat(&value);
  else
  {
    LG_LOCK(files.lock);
    struct RemoteNode * node = remoteNodeByInodeNL(ino);
    if (node && !node->dataset->stale)
      fillStat(node, &value);
    const bool stale = node && node->dataset->stale;
    const bool found = node && !stale;
    LG_UNLOCK(files.lock);
    if (!found)
    {
      fuse_reply_err(request, stale ? ESTALE : ENOENT);
      return;
    }
  }
  fuse_reply_attr(request, &value, FILE_ATTR_TIMEOUT);
}

static void fuseOpendir(fuse_req_t request, fuse_ino_t ino,
    struct fuse_file_info * info)
{
  struct RemoteDataset * dataset = NULL;
  if (ino != FUSE_ROOT_ID)
  {
    LG_LOCK(files.lock);
    struct RemoteNode * node = remoteNodeByInodeNL(ino);
    const int error = !node ? ENOENT :
      node->dataset->stale ? ESTALE :
      node->type != KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY ? ENOTDIR : 0;
    if (!error)
    {
      dataset = node->dataset;
      ++dataset->openPins;
      info->fh = dataset->acquisition;
    }
    LG_UNLOCK(files.lock);
    if (error)
    {
      fuse_reply_err(request, error);
      return;
    }
  }
  if (fuse_reply_open(request, info) == 0 || !dataset)
    return;
  LG_LOCK(files.lock);
  if (dataset->openPins)
    --dataset->openPins;
  const struct RemoteAction action = retireRemoteDatasetNL(dataset);
  LG_UNLOCK(files.lock);
  performRemoteAction(action);
}

static void replyRootDirectory(fuse_req_t request, size_t size, off_t offset)
{
  char * buffer = malloc(size ? size : 1U);
  if (!buffer)
  {
    fuse_reply_err(request, ENOMEM);
    return;
  }
  size_t used = 0;
  off_t index = 0;
  struct stat attr;
  fillRootStat(&attr);
  const char * fixed[] = { ".", ".." };
  for (unsigned i = 0; i < 2; ++i, ++index)
  {
    if (offset > index)
      continue;
    const size_t entry = fuse_add_direntry(request, buffer + used,
        size - used, fixed[i], &attr, index + 1);
    if (entry > size - used)
      goto done;
    used += entry;
  }
  LG_LOCK(files.lock);
  for (struct RemoteDataset * dataset = files.remoteDatasets;
      dataset; dataset = dataset->next)
  {
    if (!dataset->ready || dataset->stale)
      continue;
    ++index;
    if (offset > index)
      continue;
    struct RemoteNode * node = datasetRootNL(dataset);
    fillStat(node, &attr);
    const size_t entry = fuse_add_direntry(request, buffer + used,
        size - used, dataset->name, &attr, index + 1);
    if (entry > size - used)
      break;
    used += entry;
  }
  LG_UNLOCK(files.lock);

done:
  fuse_reply_buf(request, buffer, used);
  free(buffer);
}

static void fuseReaddir(fuse_req_t request, fuse_ino_t ino,
    size_t size, off_t offset, struct fuse_file_info * info)
{
  if (ino == FUSE_ROOT_ID)
  {
    replyRootDirectory(request, size, offset);
    return;
  }
  LG_LOCK(files.lock);
  struct RemoteNode * directory = remoteNodeByInodeNL(ino);
  const int error = !directory ? ENOENT :
    directory->dataset->stale ||
      info->fh != directory->dataset->acquisition ? ESTALE :
    directory->type != KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY ? ENOTDIR : 0;
  if (error)
  {
    LG_UNLOCK(files.lock);
    fuse_reply_err(request, error);
    return;
  }
  const bool listed = directory->listed;
  struct RemoteDataset * dataset = directory->dataset;
  const bool pinned = pinRemoteOperationNL(dataset, false);
  LG_UNLOCK(files.lock);
  if (!pinned)
  {
    fuse_reply_err(request, ESTALE);
    return;
  }
  if (listed)
  {
    struct RemoteRequest local =
    {
      .dataset     = dataset,
      .directory   = directory,
      .fuseRequest = request,
      .fuseSize    = size,
      .fuseOffset  = offset,
    };
    replyDirectory(&local);
    releaseRemoteOperation(dataset);
    return;
  }
  if (!requestRemote(dataset, directory,
        LG_CLIPBOARD_FILE_LIST, 0, 0, REMOTE_REPLY_READDIR,
        request, size, offset, NULL))
    fuse_reply_err(request, EIO);
  releaseRemoteOperation(dataset);
}

static void fuseOpen(fuse_req_t request, fuse_ino_t ino,
    struct fuse_file_info * info)
{
  LG_LOCK(files.lock);
  struct RemoteNode * node = remoteNodeByInodeNL(ino);
  const int error = !node ? ENOENT :
    node->dataset->stale ? ESTALE :
    node->type == KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY ? EISDIR : 0;
  if (error)
  {
    LG_UNLOCK(files.lock);
    fuse_reply_err(request, error);
    return;
  }
  if ((info->flags & O_ACCMODE) != O_RDONLY)
  {
    LG_UNLOCK(files.lock);
    fuse_reply_err(request, EROFS);
    return;
  }
  struct RemoteDataset * dataset = node->dataset;
  ++dataset->openPins;
  info->fh = dataset->acquisition;
  LG_UNLOCK(files.lock);
  info->keep_cache = 1;
  if (fuse_reply_open(request, info) == 0)
    return;
  LG_LOCK(files.lock);
  if (dataset->openPins)
    --dataset->openPins;
  const struct RemoteAction action = retireRemoteDatasetNL(dataset);
  LG_UNLOCK(files.lock);
  performRemoteAction(action);
}

static void fuseRead(fuse_req_t request, fuse_ino_t ino, size_t size,
    off_t offset, struct fuse_file_info * info)
{
  if (offset < 0)
  {
    fuse_reply_err(request, EINVAL);
    return;
  }
  LG_LOCK(files.lock);
  struct RemoteNode * node = remoteNodeByInodeNL(ino);
  const int error = !node ? ENOENT :
    node->dataset->stale ||
      info->fh != node->dataset->acquisition ? ESTALE :
    node->type != KVMFR_CLIPBOARD_FILE_TYPE_REGULAR ? EISDIR : 0;
  if (error)
  {
    LG_UNLOCK(files.lock);
    fuse_reply_err(request, error);
    return;
  }
  const uint64_t fileSize = node->size;
  struct RemoteDataset * dataset = node->dataset;
  const bool pinned = pinRemoteOperationNL(dataset, false);
  LG_UNLOCK(files.lock);
  if (!pinned)
  {
    fuse_reply_err(request, ESTALE);
    return;
  }
  if ((uint64_t)offset >= fileSize || !size)
  {
    fuse_reply_buf(request, NULL, 0);
    releaseRemoteOperation(dataset);
    return;
  }
  uint64_t wanted = fileSize - (uint64_t)offset;
  if (wanted > size)
    wanted = size;
  if (wanted > FILE_REQUEST_MAX)
    wanted = FILE_REQUEST_MAX;
  if (!requestRemote(dataset, node, LG_CLIPBOARD_FILE_READ,
        (uint64_t)offset, (uint32_t)wanted, REMOTE_REPLY_READ,
        request, 0, 0, NULL))
    fuse_reply_err(request, EIO);
  releaseRemoteOperation(dataset);
}

static void fuseStatfs(fuse_req_t request, fuse_ino_t ino)
{
  (void)ino;
  struct statvfs value;
  memset(&value, 0, sizeof(value));
  value.f_bsize   = 4096;
  value.f_frsize  = 4096;
  value.f_namemax = NAME_MAX;
  value.f_flag    = ST_RDONLY | ST_NOSUID;
  fuse_reply_statfs(request, &value);
}

static void fuseAccess(fuse_req_t request, fuse_ino_t ino, int mask)
{
  if (mask & W_OK)
  {
    fuse_reply_err(request, EROFS);
    return;
  }
  if (ino == FUSE_ROOT_ID)
  {
    fuse_reply_err(request, 0);
    return;
  }
  LG_LOCK(files.lock);
  struct RemoteNode * node = remoteNodeByInodeNL(ino);
  const bool found = node && !node->dataset->stale;
  const bool stale = node && node->dataset->stale;
  LG_UNLOCK(files.lock);
  fuse_reply_err(request, found ? 0 : stale ? ESTALE : ENOENT);
}

static void forgetRemoteNode(fuse_ino_t ino, uint64_t count)
{
  LG_LOCK(files.lock);
  struct RemoteNode * node = remoteNodeByInodeNL(ino);
  struct RemoteAction action = { 0 };
  if (node)
  {
    if (count > node->lookups)
      count = node->lookups;
    node->lookups -= count;
    if (count > node->dataset->lookupPins)
      node->dataset->lookupPins = 0;
    else
      node->dataset->lookupPins -= count;
    action = retireRemoteDatasetNL(node->dataset);
  }
  LG_UNLOCK(files.lock);
  performRemoteAction(action);
}

static void fuseForget(fuse_req_t request, fuse_ino_t ino,
    uint64_t count)
{
  if (ino != FUSE_ROOT_ID)
    forgetRemoteNode(ino, count);
  fuse_reply_none(request);
}

static void fuseForgetMulti(fuse_req_t request, size_t count,
    struct fuse_forget_data * forgets)
{
  for (size_t i = 0; i < count; ++i)
    if (forgets[i].ino != FUSE_ROOT_ID)
      forgetRemoteNode(forgets[i].ino, forgets[i].nlookup);
  fuse_reply_none(request);
}

static void releaseRemoteHandle(fuse_req_t request,
    struct fuse_file_info * info)
{
  if (!info->fh)
  {
    fuse_reply_err(request, 0);
    return;
  }
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset = remoteDatasetByAcquisitionNL(info->fh);
  const bool found = dataset != NULL;
  if (dataset && dataset->openPins)
    --dataset->openPins;
  const struct RemoteAction action = retireRemoteDatasetNL(dataset);
  LG_UNLOCK(files.lock);
  fuse_reply_err(request, found ? 0 : ESTALE);
  performRemoteAction(action);
}

static void fuseRelease(fuse_req_t request, fuse_ino_t ino,
    struct fuse_file_info * info)
{
  (void)ino;
  releaseRemoteHandle(request, info);
}

static void fuseReleasedir(fuse_req_t request, fuse_ino_t ino,
    struct fuse_file_info * info)
{
  (void)ino;
  releaseRemoteHandle(request, info);
}

static void fuseInit(void * userData, struct fuse_conn_info * connection)
{
  (void)userData;
  connection->max_read = FILE_REQUEST_MAX;
}

static const struct fuse_lowlevel_ops fuseOps =
{
  .init         = fuseInit,
  .lookup       = fuseLookup,
  .forget       = fuseForget,
  .forget_multi = fuseForgetMulti,
  .getattr      = fuseGetattr,
  .opendir      = fuseOpendir,
  .readdir      = fuseReaddir,
  .releasedir   = fuseReleasedir,
  .open         = fuseOpen,
  .read         = fuseRead,
  .release      = fuseRelease,
  .access       = fuseAccess,
  .statfs       = fuseStatfs,
};

enum FuseWaitResult
{
  FUSE_WAIT_ERROR,
  FUSE_WAIT_SESSION,
  FUSE_WAIT_WAKE,
  FUSE_WAIT_TIMEOUT,
};

static enum FuseWaitResult waitFuseEvent(
    int sessionFd, int stopFd, int timeout)
{
  struct pollfd descriptors[] =
  {
    { .fd = sessionFd, .events = POLLIN },
    { .fd = stopFd,    .events = POLLIN },
  };
  int result;
  do
    result = poll(descriptors,
        sizeof(descriptors) / sizeof(*descriptors), timeout);
  while (result < 0 && errno == EINTR);
  if (result < 0)
    return FUSE_WAIT_ERROR;
  if (!result)
    return FUSE_WAIT_TIMEOUT;
  if (descriptors[1].revents & POLLIN)
  {
    uint64_t value;
    ssize_t received;
    do
      received = read(stopFd, &value, sizeof(value));
    while (received < 0 && errno == EINTR);
    if (received != (ssize_t)sizeof(value) &&
        !(received < 0 && errno == EAGAIN))
      return FUSE_WAIT_ERROR;
    return FUSE_WAIT_WAKE;
  }
  if (descriptors[1].revents)
  {
    errno = EIO;
    return FUSE_WAIT_ERROR;
  }
  if (descriptors[0].revents & POLLIN)
    return FUSE_WAIT_SESSION;
  errno = EIO;
  return FUSE_WAIT_ERROR;
}

static bool wakeFuseThread(int stopFd)
{
  const uint64_t value = 1;
  ssize_t result;
  do
    result = write(stopFd, &value, sizeof(value));
  while (result < 0 && errno == EINTR);
  return result == (ssize_t)sizeof(value) ||
    (result < 0 && errno == EAGAIN);
}

static int fuseThread(void * opaque)
{
  struct fuse_session * session = opaque;
  struct fuse_buf buffer =
  {
    .mem = NULL,
  };
  int result = 0;
  while (!fuse_session_exited(session))
  {
    switch (waitFuseEvent(fuse_session_fd(session), files.fuseStopFd,
          nextRemoteDeliveryTimeout()))
    {
      case FUSE_WAIT_WAKE:
      {
        LG_LOCK(files.lock);
        const bool stopping = files.stopping;
        LG_UNLOCK(files.lock);
        if (stopping)
          goto done;
        reapRemoteDeliveries(false);
        continue;
      }

      case FUSE_WAIT_TIMEOUT:
        reapRemoteDeliveries(false);
        continue;

      case FUSE_WAIT_ERROR:
        result = -errno;
        goto done;

      case FUSE_WAIT_SESSION:
        break;
    }

    result = fuse_session_receive_buf(session, &buffer);
    if (result == -EINTR)
      continue;
    if (result <= 0)
      goto done;
    fuse_session_process_buf(session, &buffer);
  }

done:
  free(buffer.mem);
  if (result > 0)
    result = 0;
  fuse_session_reset(session);
  return result;
}

static char * uriDecode(const char * value, size_t length)
{
  static const char hex[] = "0123456789abcdef";
  char * result = malloc(length + 1U);
  if (!result)
    return NULL;
  size_t out = 0;
  for (size_t i = 0; i < length; ++i)
  {
    unsigned char ch = (unsigned char)value[i];
    if (ch == '%')
    {
      if (i + 2U >= length)
        goto invalid;
      const char * high = strchr(hex, (char)(value[i + 1U] | 0x20));
      const char * low  = strchr(hex, (char)(value[i + 2U] | 0x20));
      if (!high || !low)
        goto invalid;
      ch = (unsigned char)(((high - hex) << 4) | (low - hex));
      i += 2U;
    }
    if (!ch)
      goto invalid;
    result[out++] = (char)ch;
  }
  result[out] = 0;
  return result;

invalid:
  free(result);
  return NULL;
}

static bool localNodeStat(struct LocalNode * node, const struct stat * value)
{
  if (S_ISREG(value->st_mode))
    node->type = KVMFR_CLIPBOARD_FILE_TYPE_REGULAR;
  else if (S_ISDIR(value->st_mode))
    node->type = KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY;
  else
    return false;
  node->size       = value->st_size < 0 ? 0 : (uint64_t)value->st_size;
  node->modifiedNs = nsFromTimespec(value->st_mtim);
  node->createdNs  = 0;
  node->device     = value->st_dev;
  node->inode      = value->st_ino;
  return true;
}

static bool localStatSupported(const struct stat * value)
{
  return S_ISREG(value->st_mode) || S_ISDIR(value->st_mode);
}

static bool localStatEqual(const struct stat * first,
    const struct stat * second)
{
  return first->st_dev == second->st_dev &&
    first->st_ino == second->st_ino &&
    ((S_ISREG(first->st_mode) && S_ISREG(second->st_mode)) ||
     (S_ISDIR(first->st_mode) && S_ISDIR(second->st_mode))) &&
    first->st_size == second->st_size &&
    first->st_mtim.tv_sec == second->st_mtim.tv_sec &&
    first->st_mtim.tv_nsec == second->st_mtim.tv_nsec;
}

static bool localNodeMatches(const struct LocalNode * node,
    const struct stat * value)
{
  return node->device == value->st_dev && node->inode == value->st_ino &&
    ((node->type == KVMFR_CLIPBOARD_FILE_TYPE_REGULAR &&
      S_ISREG(value->st_mode)) ||
     (node->type == KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY &&
      S_ISDIR(value->st_mode))) &&
    node->size == (value->st_size < 0 ? 0 : (uint64_t)value->st_size) &&
    node->modifiedNs == nsFromTimespec(value->st_mtim);
}

static int openAbsoluteLocal(const char * path, char ** name,
    struct stat * value)
{
  if (!path || path[0] != '/' || !path[1])
    return -1;

  int current = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (current < 0)
    return -1;

  const char * component = path + 1;
  for (;;)
  {
    const char * slash = strchr(component, '/');
    const size_t length = slash ? (size_t)(slash - component) :
      strlen(component);
    if (!length || length > NAME_MAX ||
        (length == 1U && component[0] == '.') ||
        (length == 2U && component[0] == '.' && component[1] == '.'))
      goto failed;

    char * part = strndup(component, length);
    if (!part)
      goto failed;

    struct stat before;
    const bool final = !slash;
    int next = -1;
    if (fstatat(current, part, &before, AT_SYMLINK_NOFOLLOW) == 0 &&
        !S_ISLNK(before.st_mode) &&
        (final ? localStatSupported(&before) : S_ISDIR(before.st_mode)))
    {
      int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
      if (!final || S_ISDIR(before.st_mode))
        flags |= O_DIRECTORY;
      next = openat(current, part, flags);
    }
    if (next < 0)
    {
      free(part);
      goto failed;
    }

    struct stat after;
    if (fstat(next, &after) < 0 || !localStatEqual(&before, &after))
    {
      close(next);
      free(part);
      goto failed;
    }
    close(current);
    current = next;

    if (final)
    {
      *name  = part;
      *value = after;
      return current;
    }
    free(part);
    component = slash + 1;
  }

failed:
  close(current);
  return -1;
}

static int openLocalNode(const struct LocalNode * node,
    struct stat * value)
{
  if (!node || !node->root || node->root->fd < 0)
    return -1;

  int current;
  if (node->root->type == KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY)
    current = openat(node->root->fd, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  else
    current = fcntl(node->root->fd, F_DUPFD_CLOEXEC, 0);
  if (current < 0)
    return -1;

  const char * component = node->path;
  while (*component)
  {
    const char * slash = strchr(component, '/');
    const size_t length = slash ? (size_t)(slash - component) :
      strlen(component);
    if (!length || length > NAME_MAX ||
        (length == 1U && component[0] == '.') ||
        (length == 2U && component[0] == '.' && component[1] == '.'))
      goto failed;
    char * part = strndup(component, length);
    if (!part)
      goto failed;
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    if (slash || node->type == KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY)
      flags |= O_DIRECTORY;
    const int next = openat(current, part, flags);
    free(part);
    if (next < 0)
      goto failed;
    close(current);
    current = next;
    component = slash ? slash + 1 : component + length;
  }

  if (fstat(current, value) < 0)
    goto failed;
  if (!localNodeMatches(node, value))
  {
    errno = ESTALE;
    goto failed;
  }
  return current;

failed:
  close(current);
  return -1;
}

static void freeLocalDataset(struct LocalDataset * dataset)
{
  if (!dataset)
    return;
  struct LocalNode * node = dataset->nodes;
  while (node)
  {
    struct LocalNode * next = node->next;
    if (node->fd >= 0)
      close(node->fd);
    free(node->path);
    free(node->name);
    free(node);
    node = next;
  }
  free(dataset);
}

static struct LocalDataset * takeUnusedLocalDatasetNL(
    struct LocalDataset * dataset)
{
  if (!dataset || dataset->current || dataset->references)
    return NULL;
  struct LocalDataset ** link = &files.localDatasets;
  while (*link && *link != dataset)
    link = &(*link)->next;
  if (!*link)
    return NULL;
  *link = dataset->next;
  dataset->next = NULL;
  return dataset;
}

static char * uriPath(const char * line, size_t length)
{
  if (length < strlen(FILE_URI_PREFIX) ||
      strncasecmp(line, FILE_URI_PREFIX, strlen(FILE_URI_PREFIX)))
    return NULL;
  line   += strlen(FILE_URI_PREFIX);
  length -= strlen(FILE_URI_PREFIX);

  if (length >= 10U && !strncasecmp(line, "localhost/", 10U))
  {
    line += 9U;
    length -= 9U;
  }
  else if (!length || line[0] != '/')
    return NULL;

  static const char punctuation[] = "-._~!$&'()*+,;=:@/%";
  for (size_t i = 0; i < length; ++i)
  {
    const unsigned char ch = (unsigned char)line[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || strchr(punctuation, ch)))
      return NULL;
  }
  char * path = uriDecode(line, length);
  if (!path)
    return NULL;
  size_t pathLength = strlen(path);
  while (pathLength > 1U && path[pathLength - 1U] == '/')
    path[--pathLength] = 0;
  return path;
}

static struct LocalNode * localNodeByIdNL(struct LocalDataset * dataset,
    uint64_t node)
{
  for (struct LocalNode * item = dataset->nodes; item; item = item->next)
    if (item->node == node)
      return item;
  return NULL;
}

static struct LocalNode * localChildNL(struct LocalDataset * dataset,
    uint64_t parent, const char * name)
{
  for (struct LocalNode * item = dataset->nodes; item; item = item->next)
    if (item->parent == parent && !strcmp(item->name, name))
      return item;
  return NULL;
}

static struct LocalNode * addLocalNode(struct LocalDataset * dataset,
    uint64_t parent, struct LocalNode * root, char * path,
    const char * name, const struct stat * value)
{
  struct LocalNode * node = calloc(1, sizeof(*node));
  if (!node)
  {
    free(path);
    return NULL;
  }
  node->fd   = -1;
  node->path = path;
  node->name = strdup(name);
  if (!node->name || !localNodeStat(node, value))
  {
    free(node->path);
    free(node->name);
    free(node);
    return NULL;
  }
  node->dataset  = dataset;
  node->root     = root ? root : node;
  node->parent   = parent;
  node->node     = ++dataset->nodeSerial;
  node->next     = dataset->nodes;
  dataset->nodes = node;
  return node;
}

bool lgClipboardFiles_setLocal(const char * mime,
    const void * data, size_t size)
{
  if (!mime || (!data && size) ||
      (strcmp(mime, FILE_URI_MIME) && strcmp(mime, FILE_GNOME_MIME)))
    return false;
  const char * text = data;
  size_t offset = 0;
  if (!strcmp(mime, FILE_GNOME_MIME))
  {
    const char * newline = size ? memchr(text, '\n', size) : NULL;
    if (!newline)
      return false;
    const size_t operationLength = (size_t)(newline - text);
    if ((operationLength != 4U || memcmp(text, "copy", 4U)) &&
        (operationLength != 3U || memcmp(text, "cut", 3U)))
      return false;
    offset = operationLength + 1U;
  }
  struct LocalDataset * dataset = calloc(1, sizeof(*dataset));
  if (!dataset)
    return false;
  while (offset < size)
  {
    const char * line = text + offset;
    const char * newline = memchr(line, '\n', size - offset);
    size_t length = newline ? (size_t)(newline - line) : size - offset;
    if (length && line[length - 1U] == '\r')
      --length;
    offset += (newline ? (size_t)(newline - line) + 1U : size - offset);
    if (!length || line[0] == '#')
      continue;
    char * path = uriPath(line, length);
    if (!path)
      goto invalid;
    char * name = NULL;
    struct stat value;
    const int fd = openAbsoluteLocal(path, &name, &value);
    free(path);
    if (fd < 0)
    {
      free(name);
      goto invalid;
    }
    char * relative = strdup("");
    if (!relative || !validUtf8((const uint8_t *)name, strlen(name)) ||
        localChildNL(dataset, KVMFR_CLIPBOARD_FILE_ROOT_NODE, name))
    {
      close(fd);
      free(relative);
      free(name);
      goto invalid;
    }
    struct LocalNode * node = addLocalNode(dataset,
        KVMFR_CLIPBOARD_FILE_ROOT_NODE, NULL, relative, name, &value);
    free(name);
    if (!node)
    {
      close(fd);
      goto invalid;
    }
    node->fd = fd;
  }
  if (!dataset->nodes)
    goto invalid;
  dataset->current = true;
  LG_LOCK(files.lock);
  dataset->dataset = nextDatasetNL();
  struct LocalDataset * retired = NULL;
  if (files.localCurrent)
  {
    files.localCurrent->current = false;
    retired = takeUnusedLocalDatasetNL(files.localCurrent);
  }
  dataset->next       = files.localDatasets;
  files.localDatasets = dataset;
  files.localCurrent  = dataset;
  const uint64_t datasetId = dataset->dataset;
  LG_UNLOCK(files.lock);
  freeLocalDataset(retired);
  lgClipboard_notifyFiles(datasetId);
  return true;

invalid:
  freeLocalDataset(dataset);
  return false;
}

void lgClipboardFiles_clearLocal(void)
{
  LG_LOCK(files.lock);
  struct LocalDataset * retired = NULL;
  if (files.localCurrent)
  {
    files.localCurrent->current = false;
    retired = takeUnusedLocalDatasetNL(files.localCurrent);
  }
  files.localCurrent = NULL;
  LG_UNLOCK(files.lock);
  freeLocalDataset(retired);
}

static char * uriEncodePath(const char * path)
{
  static const char hex[] = "0123456789ABCDEF";
  const size_t length = strlen(path);
  if (length > (SIZE_MAX - 1U) / 3U)
    return NULL;
  char * result = malloc(length * 3U + 1U);
  if (!result)
    return NULL;
  size_t out = 0;
  for (size_t i = 0; i < length; ++i)
  {
    const unsigned char ch = (unsigned char)path[i];
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '/' || ch == '-' ||
        ch == '_' || ch == '.' || ch == '~')
      result[out++] = (char)ch;
    else
    {
      result[out++] = '%';
      result[out++] = hex[ch >> 4];
      result[out++] = hex[ch & 0xfU];
    }
  }
  result[out] = 0;
  return result;
}

static bool getRemoteDatasetNL(struct RemoteDataset * dataset,
    const char * mime, char ** data, size_t * size)
{
  if (!strcmp(mime, FILE_KDE_CUT_MIME))
  {
    *data = strdup("0");
    *size = *data ? 1U : 0U;
    return *data != NULL;
  }
  if (strcmp(mime, FILE_URI_MIME) && strcmp(mime, FILE_GNOME_MIME))
    return false;
  char * result = NULL;
  size_t used = 0;
  size_t capacity = 0;
  if (!strcmp(mime, FILE_GNOME_MIME))
  {
    if (!grow((void **)&result, &capacity, 6U))
      return false;
    memcpy(result, "copy\n", 5U);
    used = 5U;
  }
  for (struct RemoteNode * node = files.remoteNodes; node; node = node->next)
  {
    if (node->dataset != dataset || node->parent != dataset->ino)
      continue;
    const size_t rawLength = strlen(files.mountpoint) + 1U +
      strlen(dataset->name) + 1U + strlen(node->name) + 1U;
    char * raw = malloc(rawLength);
    if (!raw)
      goto error;
    snprintf(raw, rawLength, "%s/%s/%s",
        files.mountpoint, dataset->name, node->name);
    char * encoded = uriEncodePath(raw);
    free(raw);
    if (!encoded)
      goto error;
    const size_t line = strlen(FILE_URI_PREFIX) + strlen(encoded) + 2U;
    if (!grow((void **)&result, &capacity, used + line + 1U))
    {
      free(encoded);
      goto error;
    }
    used += (size_t)snprintf(result + used, capacity - used,
        FILE_URI_PREFIX "%s\r\n", encoded);
    free(encoded);
  }
  if (!result)
    return false;
  *data = result;
  *size = used;
  return true;

error:
  free(result);
  return false;
}

bool lgClipboardFiles_getRemote(const char * mime,
    char ** data, size_t * size)
{
  if (!mime || !data || !size)
    return false;
  *data = NULL;
  *size = 0;
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset = files.remoteCurrent;
  const bool result = dataset && dataset->ready && !dataset->stale &&
    getRemoteDatasetNL(dataset, mime, data, size);
  LG_UNLOCK(files.lock);
  return result;
}

uint64_t lgClipboardFiles_remotePresentationAcquire(void)
{
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset = files.remoteCurrent;
  const uint64_t presentation = dataset && dataset->ready &&
      dataset->acquired && !dataset->stale ? dataset->acquisition : 0;
  if (presentation)
    ++dataset->presentationPins;
  LG_UNLOCK(files.lock);
  return presentation;
}

bool lgClipboardFiles_getRemotePresentation(uint64_t presentation,
    const char * mime, char ** data, size_t * size)
{
  if (!presentation || !mime || !data || !size)
    return false;
  *data = NULL;
  *size = 0;
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset = remoteDatasetByAcquisitionNL(presentation);
  const bool result = dataset && dataset->presentationPins &&
    dataset->ready && !dataset->stale &&
    getRemoteDatasetNL(dataset, mime, data, size);
  LG_UNLOCK(files.lock);
  return result;
}

void lgClipboardFiles_remotePresentationDelivered(uint64_t presentation)
{
  if (!presentation)
    return;
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset =
    remoteDatasetByAcquisitionNL(presentation);
  const bool delivered = dataset && dataset->presentationPins &&
    dataset->ready && dataset->acquired && !dataset->stale;
  int wakeFd = -1;
  if (delivered)
  {
    const uint64_t now = microtime();
    dataset->deliveryDeadline = now >
        UINT64_MAX - FILE_DELIVERY_GRACE_US ? UINT64_MAX :
      now + FILE_DELIVERY_GRACE_US;
    dataset->deliveryPending  = true;
    wakeFd                    = files.fuseStopFd;
  }
  LG_UNLOCK(files.lock);
  if (wakeFd >= 0 && !wakeFuseThread(wakeFd))
    DEBUG_WARN("Failed to schedule clipboard file delivery expiry: %s",
        strerror(errno));
  if (delivered)
    reapRemoteDeliveries(true);
}

void lgClipboardFiles_remotePresentationRelease(uint64_t presentation)
{
  if (!presentation)
    return;
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset = remoteDatasetByAcquisitionNL(presentation);
  if (dataset && dataset->presentationPins)
    --dataset->presentationPins;
  const struct RemoteAction action = retireRemoteDatasetNL(dataset);
  LG_UNLOCK(files.lock);
  performRemoteAction(action);
}

bool lgClipboardFiles_remoteReady(uint64_t dataset)
{
  LG_LOCK(files.lock);
  struct RemoteDataset * found = remoteDatasetByIdNL(dataset);
  const bool result = found && found->ready && found->current;
  LG_UNLOCK(files.lock);
  return result;
}

static struct RemoteRequest * takeInitialRequestNL(
    struct RemoteDataset * dataset)
{
  struct RemoteRequest * request = files.remoteRequests;
  while (request && (request->dataset != dataset ||
      request->reply != REMOTE_REPLY_INITIAL))
    request = request->next;
  return request ? remoteRequestTakePointerNL(request) : NULL;
}

static void cancelRetiredInitialRequest(struct RemoteRequest * request)
{
  if (!request)
    return;
  const LG_ClipboardFileRequest descriptor = request->request;
  if (request->transportStarted &&
      !lgClipboard_fileCancel(descriptor.dataset, descriptor.request,
        LG_CLIPBOARD_FILE_ERROR_CANCELLED))
    DEBUG_WARN("Failed to cancel retired clipboard file request");
  unregisterRemoteInterrupt(request);
  completeRemoteRequest(request);
}

bool lgClipboardFiles_remoteOffer(uint64_t datasetId)
{
  DEBUG_INFO("Remote clipboard file offer received: dataset=%" PRIu64,
      datasetId);
  if (!datasetId)
    return false;
  struct RemoteDataset * dataset = calloc(1, sizeof(*dataset));
  struct RemoteNode * root = calloc(1, sizeof(*root));
  if (!dataset || !root)
  {
    free(dataset);
    free(root);
    return false;
  }
  LG_LOCK(files.lock);
  if (files.remoteCurrent && files.remoteCurrent->dataset == datasetId &&
      !files.remoteCurrent->stale)
  {
    LG_UNLOCK(files.lock);
    free(dataset);
    free(root);
    return true;
  }
  struct RemoteDataset * reusable = remoteDatasetByIdNL(datasetId);
  if (reusable && reusable->stale)
    reusable = NULL;
  if (!reusable)
  {
    dataset->dataset     = datasetId;
    dataset->acquisition = nextAcquisitionNL();
    dataset->ino         = nextInodeNL();
    dataset->current     = true;
    dataset->acquiring   = true;
    snprintf(dataset->name, sizeof(dataset->name),
        "%016" PRIx64 "-%016" PRIx64,
        datasetId, (uint64_t)dataset->ino);
    *root = (struct RemoteNode)
    {
      .dataset = dataset,
      .ino     = dataset->ino,
      .parent  = FUSE_ROOT_ID,
      .node    = KVMFR_CLIPBOARD_FILE_ROOT_NODE,
      .type    = KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY,
      .name    = strdup(dataset->name),
    };
    if (!root->name)
    {
      LG_UNLOCK(files.lock);
      free(dataset);
      free(root);
      return false;
    }
  }
  struct RemoteDataset * previous = files.remoteCurrent;
  struct RemoteRequest * cancelled = NULL;
  struct RemoteAction previousAction = { 0 };
  if (previous)
  {
    previous->current = false;
    cancelled = takeInitialRequestNL(previous);
    previousAction = retireRemoteDatasetNL(previous);
  }
  if (reusable)
  {
    reusable->current   = true;
    files.remoteCurrent = reusable;
    LG_UNLOCK(files.lock);
    cancelRetiredInitialRequest(cancelled);
    performRemoteAction(previousAction);
    reapRemoteDeliveries(true);
    free(dataset);
    free(root);
    return true;
  }
  dataset->next        = files.remoteDatasets;
  files.remoteDatasets = dataset;
  files.remoteCurrent  = dataset;
  root->next           = files.remoteNodes;
  files.remoteNodes    = root;
  const uint64_t acquisition = dataset->acquisition;
  LG_UNLOCK(files.lock);
  cancelRetiredInitialRequest(cancelled);
  performRemoteAction(previousAction);
  reapRemoteDeliveries(true);
  if (!lgClipboard_fileAcquire(datasetId, acquisition))
  {
    lgClipboardFiles_remoteCancel(datasetId, acquisition,
        LG_CLIPBOARD_FILE_ERROR_DISCONNECTED);
    return false;
  }
  return true;
}

void lgClipboardFiles_remoteClear(void)
{
  LG_LOCK(files.lock);
  struct RemoteDataset * previous = files.remoteCurrent;
  struct RemoteRequest * cancelled = NULL;
  struct RemoteAction action = { 0 };
  if (previous)
  {
    previous->current = false;
    cancelled = takeInitialRequestNL(previous);
    action = retireRemoteDatasetNL(previous);
  }
  files.remoteCurrent = NULL;
  LG_UNLOCK(files.lock);
  cancelRetiredInitialRequest(cancelled);
  performRemoteAction(action);
}

void lgClipboardFiles_providerUnavailable(void)
{
  if (!files.initialized)
    return;

  LG_LOCK(files.lock);
  files.remoteCurrent = NULL;
  for (struct RemoteDataset * dataset = files.remoteDatasets;
      dataset; dataset = dataset->next)
  {
    dataset->current          = false;
    dataset->acquiring        = false;
    dataset->acquired         = false;
    dataset->stale            = true;
    dataset->deliveryPending  = false;
    dataset->deliveryDeadline = 0;
  }
  struct RemoteRequest * remoteRequests = files.remoteRequests;
  files.remoteRequests = NULL;

  struct LocalAcquisition * localAcquisitions = files.localAcquisitions;
  files.localAcquisitions = NULL;
  for (struct LocalAcquisition * acquisition = localAcquisitions;
      acquisition; acquisition = acquisition->next)
    if (acquisition->dataset->references)
      --acquisition->dataset->references;

  struct LocalDataset * retired = NULL;
  struct LocalDataset ** datasetLink = &files.localDatasets;
  while (*datasetLink)
  {
    struct LocalDataset * dataset = *datasetLink;
    if (dataset->current || dataset->references)
    {
      datasetLink = &dataset->next;
      continue;
    }
    *datasetLink = dataset->next;
    dataset->next = retired;
    retired = dataset;
  }

  for (struct LocalJob * job = files.jobsHead; job; job = job->next)
    atomic_store_explicit(&job->cancelled, true, memory_order_release);
  for (struct LocalJob * job = files.activeJobs; job; job = job->next)
  {
    atomic_store_explicit(&job->cancelled, true, memory_order_release);
    lgSignalEvent(job->ready);
  }
  LG_UNLOCK(files.lock);
  if (files.jobEvent)
    lgSignalEvent(files.jobEvent);

  while (remoteRequests)
  {
    struct RemoteRequest * next = remoteRequests->next;
    unregisterRemoteInterrupt(remoteRequests);
    if (remoteRequests->fuseRequest)
      fuse_reply_err(remoteRequests->fuseRequest, EIO);
    completeRemoteRequest(remoteRequests);
    remoteRequests = next;
  }
  pruneUnusedRemoteDatasets();
  while (localAcquisitions)
  {
    struct LocalAcquisition * next = localAcquisitions->next;
    free(localAcquisitions);
    localAcquisitions = next;
  }
  while (retired)
  {
    struct LocalDataset * next = retired->next;
    freeLocalDataset(retired);
    retired = next;
  }
}

void lgClipboardFiles_remoteAcquired(uint64_t datasetId,
    uint64_t acquisition, LG_ClipboardFileError error)
{
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset = remoteDatasetByAcquisitionNL(acquisition);
  const bool valid = dataset && dataset->dataset == datasetId &&
    dataset->acquiring && !dataset->stale;
  if (valid)
  {
    dataset->acquiring = false;
    dataset->acquired  = !error;
  }
  struct RemoteNode * root = valid ? datasetRootNL(dataset) : NULL;
  const bool pinned = valid && !error && root &&
    pinRemoteOperationNL(dataset, false);
  const uint64_t failedDataset = valid && (error || !pinned) ?
    failCurrentRemoteDatasetNL(dataset) : 0;
  const struct RemoteAction action = valid ?
    retireRemoteDatasetNL(dataset) : (struct RemoteAction) { 0 };
  LG_UNLOCK(files.lock);
  if (valid && error)
    DEBUG_WARN("Remote clipboard file acquisition failed: "
        "dataset=%" PRIu64 ", acquisition=%" PRIu64 ", error=%u (%s)",
        datasetId, acquisition, (unsigned)error, fileErrorName(error));
  else if (valid)
    DEBUG_INFO("Remote clipboard file acquisition succeeded: "
        "dataset=%" PRIu64 ", acquisition=%" PRIu64,
        datasetId, acquisition);
  performRemoteAction(action);
  if (failedDataset)
    lgClipboard_fileRemoteFailed(failedDataset);
  if (!pinned)
    return;
  if (!requestRemote(dataset, root, LG_CLIPBOARD_FILE_LIST,
        0, 0, REMOTE_REPLY_INITIAL, NULL, 0, 0, NULL))
  {
    LG_LOCK(files.lock);
    const uint64_t unsentDataset =
      failCurrentRemoteDatasetNL(dataset);
    const struct RemoteAction failed = retireRemoteDatasetNL(dataset);
    LG_UNLOCK(files.lock);
    performRemoteAction(failed);
    if (unsentDataset)
      lgClipboard_fileRemoteFailed(unsentDataset);
  }
  releaseRemoteOperation(dataset);
}

static bool sameRemoteFileRequest(
    const struct RemoteRequest * request,
    const LG_ClipboardFileRequest * descriptor)
{
  return request->request.dataset == descriptor->dataset &&
    request->request.request == descriptor->request &&
    request->request.node == descriptor->node &&
    request->request.offset == descriptor->offset &&
    request->request.length == descriptor->length &&
    request->request.operation == descriptor->operation;
}

LG_ClipboardResult lgClipboardFiles_remoteDataBegin(
    const LG_ClipboardFileRequest * descriptor, uint64_t sizeHint)
{
  if (!descriptor)
    return LG_CLIPBOARD_RESULT_FAILED;
  LG_LOCK(files.lock);
  struct RemoteRequest * request = files.remoteRequests;
  while (request && request->request.request != descriptor->request)
    request = request->next;
  const bool valid = request && !request->began &&
    sameRemoteFileRequest(request, descriptor) &&
    (descriptor->operation != LG_CLIPBOARD_FILE_READ ||
     sizeHint == descriptor->length);
  if (valid)
  {
    request->began    = true;
    request->sizeHint = sizeHint;
  }
  struct RemoteRequest * failed = !valid && request ?
    remoteRequestTakeNL(descriptor->request) : NULL;
  LG_UNLOCK(files.lock);
  failRemoteRequest(failed, EIO);
  return valid ? LG_CLIPBOARD_RESULT_ACCEPTED :
    LG_CLIPBOARD_RESULT_FAILED;
}

LG_ClipboardResult lgClipboardFiles_remoteDataChunk(
    const LG_ClipboardFileRequest * descriptor, uint64_t responseOffset,
    const void * data, size_t size)
{
  if (!descriptor)
    return LG_CLIPBOARD_RESULT_FAILED;
  const bool envelopeValid = data && size && responseOffset <= SIZE_MAX &&
    size <= SIZE_MAX - (size_t)responseOffset;
  LG_LOCK(files.lock);
  struct RemoteRequest * request = files.remoteRequests;
  while (request && request->request.request != descriptor->request)
    request = request->next;
  const size_t end = envelopeValid ? (size_t)responseOffset + size : 0;
  const bool valid = envelopeValid && request && request->began &&
    sameRemoteFileRequest(request, descriptor) &&
    responseOffset == request->dataSize &&
    (request->sizeHint == KVMFR_CLIPBOARD_SIZE_UNKNOWN ||
     end <= request->sizeHint) &&
    (descriptor->operation != LG_CLIPBOARD_FILE_READ ||
     end <= descriptor->length) &&
    grow((void **)&request->data, &request->dataCapacity, end);
  if (valid)
  {
    memcpy(request->data + request->dataSize, data, size);
    request->dataSize = end;
  }
  struct RemoteRequest * failed = !valid && request ?
    remoteRequestTakeNL(descriptor->request) : NULL;
  LG_UNLOCK(files.lock);
  failRemoteRequest(failed, EIO);
  return valid ? LG_CLIPBOARD_RESULT_ACCEPTED :
    LG_CLIPBOARD_RESULT_FAILED;
}

LG_ClipboardResult lgClipboardFiles_remoteDataEnd(
    const LG_ClipboardFileRequest * descriptor, uint64_t finalSize)
{
  if (!descriptor)
    return LG_CLIPBOARD_RESULT_FAILED;
  LG_LOCK(files.lock);
  struct RemoteRequest * request = remoteRequestTakeNL(descriptor->request);
  const bool valid = finalSize <= SIZE_MAX && request && request->began &&
    sameRemoteFileRequest(request, descriptor) &&
    request->dataSize == (size_t)finalSize &&
    (request->sizeHint == KVMFR_CLIPBOARD_SIZE_UNKNOWN ||
     request->sizeHint == finalSize) &&
    (descriptor->operation != LG_CLIPBOARD_FILE_READ ||
     finalSize == descriptor->length);
  LG_UNLOCK(files.lock);
  if (!valid)
  {
    failRemoteRequest(request, EIO);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  finishRemoteRequest(request);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

void lgClipboardFiles_remoteCancel(uint64_t dataset,
    uint64_t requestId, LG_ClipboardFileError reason)
{
  LG_LOCK(files.lock);
  struct RemoteRequest * request = remoteRequestTakeNL(requestId);
  bool acquisition = false;
  uint64_t failedDataset = 0;
  struct RemoteAction action = { 0 };
  for (struct RemoteDataset * item = files.remoteDatasets;
      item; item = item->next)
    if (item->dataset == dataset && item->acquisition == requestId)
    {
      item->acquiring        = false;
      item->acquired         = false;
      failedDataset          = failCurrentRemoteDatasetNL(item);
      item->ready            = false;
      item->current          = false;
      item->stale            = true;
      item->deliveryPending  = false;
      item->deliveryDeadline = 0;
      if (files.remoteCurrent == item)
        files.remoteCurrent = NULL;
      acquisition            = true;
      action                 = retireRemoteDatasetNL(item);
      break;
    }
  LG_UNLOCK(files.lock);
  if (acquisition)
    DEBUG_WARN("Remote clipboard file acquisition failed: "
        "dataset=%" PRIu64 ", acquisition=%" PRIu64 ", error=%u (%s)",
        dataset, requestId, (unsigned)reason, fileErrorName(reason));
  if (acquisition && !request)
  {
    performRemoteAction(action);
    if (failedDataset)
      lgClipboard_fileRemoteFailed(failedDataset);
    return;
  }
  if (!request || request->request.dataset != dataset)
  {
    failRemoteRequest(request, EIO);
    performRemoteAction(action);
    if (failedDataset)
      lgClipboard_fileRemoteFailed(failedDataset);
    return;
  }
  failRemoteRequest(request, errnoFromFileError(reason));
  performRemoteAction(action);
  if (failedDataset)
    lgClipboard_fileRemoteFailed(failedDataset);
}

void lgClipboardFiles_localAcquire(uint64_t wireDataset,
    uint64_t acquisition)
{
  LG_ClipboardFileError error = LG_CLIPBOARD_FILE_ERROR_STALE;
  struct LocalAcquisition * item = calloc(1, sizeof(*item));
  LG_LOCK(files.lock);
  struct LocalDataset * dataset = files.localDatasets;
  while (dataset && dataset->dataset != wireDataset)
    dataset = dataset->next;
  if (dataset && item)
  {
    item->dataset     = dataset;
    item->wireDataset = wireDataset;
    item->acquisition = acquisition;
    item->next        = files.localAcquisitions;
    files.localAcquisitions = item;
    ++dataset->references;
    error = LG_CLIPBOARD_FILE_ERROR_NONE;
  }
  else if (!item)
    error = LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
  LG_UNLOCK(files.lock);
  if (error)
    free(item);
  lgClipboard_fileAcquired(wireDataset, acquisition, error);
}

void lgClipboardFiles_localRelease(uint64_t wireDataset,
    uint64_t acquisition)
{
  LG_LOCK(files.lock);
  struct LocalAcquisition ** link = &files.localAcquisitions;
  while (*link && ((*link)->wireDataset != wireDataset ||
      (*link)->acquisition != acquisition))
    link = &(*link)->next;
  struct LocalAcquisition * item = *link;
  struct LocalDataset * retired = NULL;
  if (item)
  {
    *link = item->next;
    if (item->dataset->references)
      --item->dataset->references;
    retired = takeUnusedLocalDatasetNL(item->dataset);
  }
  LG_UNLOCK(files.lock);
  free(item);
  freeLocalDataset(retired);
}

static struct LocalDataset * acquiredLocalDatasetNL(uint64_t wireDataset)
{
  for (struct LocalDataset * dataset = files.localDatasets;
      dataset; dataset = dataset->next)
    if (dataset->dataset == wireDataset)
      return dataset;
  return NULL;
}

static bool sendJobCall(struct LocalJob * job, int operation,
    uint64_t offset, const void * data, size_t size, bool end)
{
  for (;;)
  {
    if (atomic_load_explicit(&job->cancelled, memory_order_acquire))
      return false;
    LG_ClipboardResult result;
    switch (operation)
    {
      case 0:
        result = lgClipboard_fileDataBegin(&job->request, offset);
        break;
      case 1:
        result = lgClipboard_fileDataChunk(
            &job->request, offset, data, size, end);
        break;
      default:
        result = lgClipboard_fileDataEnd(&job->request, offset);
        break;
    }
    if (result == LG_CLIPBOARD_RESULT_ACCEPTED)
      return true;
    if (result == LG_CLIPBOARD_RESULT_FAILED)
      return false;
    if (!lgWaitEvent(job->ready, TIMEOUT_INFINITE))
      return false;
  }
}

static bool sendBufferJob(struct LocalJob * job,
    const uint8_t * data, size_t size)
{
  if (!sendJobCall(job, 0, size, NULL, 0, false))
    return false;
  size_t offset = 0;
  while (offset < size)
  {
    size_t chunk = size - offset;
    if (chunk > FILE_STREAM_CHUNK)
      chunk = FILE_STREAM_CHUNK;
    const bool end = chunk == size - offset;
    if (!sendJobCall(job, 1, offset, data + offset, chunk, end))
      return false;
    offset += chunk;
  }
  return sendJobCall(job, 2, size, NULL, 0, true);
}

static LG_ClipboardFileError appendLocalEntry(uint8_t ** data, size_t * size,
    size_t * capacity, const struct LocalNode * node)
{
  const size_t nameLength = strlen(node->name);
  if (!nameLength || nameLength > NAME_MAX ||
      !validUtf8((const uint8_t *)node->name, nameLength))
    return LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED;
  const uint64_t bytes64 = KVMFR_CLIPBOARD_FILE_ENTRY_BYTES(nameLength);
  if (bytes64 > SIZE_MAX || *size > SIZE_MAX - (size_t)bytes64)
    return LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
  const size_t wanted = *size + (size_t)bytes64;
  if (!wanted || !grow((void **)data, capacity, wanted) || !*data)
    return LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
  KVMFRClipboardFileEntry entry =
  {
    .node       = node->node,
    .size       = node->size,
    .createdNs  = node->createdNs,
    .modifiedNs = node->modifiedNs,
    .type       = node->type,
    .nameLength = (uint32_t)nameLength,
  };
  memcpy(*data + *size, &entry, sizeof(entry));
  memcpy(*data + *size + sizeof(entry), node->name, nameLength);
  memset(*data + *size + sizeof(entry) + nameLength, 0,
      (size_t)bytes64 - sizeof(entry) - nameLength);
  *size += (size_t)bytes64;
  return LG_CLIPBOARD_FILE_ERROR_NONE;
}

static bool localJobCancelled(const struct LocalJob * job)
{
  return atomic_load_explicit(&job->cancelled, memory_order_acquire);
}

static LG_ClipboardFileError localList(struct LocalJob * job)
{
  uint8_t * data = NULL;
  size_t size = 0;
  size_t capacity = 0;
  struct LocalNode ** listed = NULL;
  size_t listedCount = 0;
  size_t listedCapacity = 0;
  LG_ClipboardFileError error = LG_CLIPBOARD_FILE_ERROR_IO;
  LG_LOCK(files.lock);
  struct LocalNode * directory = job->request.node ?
    localNodeByIdNL(job->dataset, job->request.node) : NULL;
  if (job->request.node && !directory)
  {
    LG_UNLOCK(files.lock);
    return LG_CLIPBOARD_FILE_ERROR_NOT_FOUND;
  }
  if (directory &&
      directory->type != KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY)
  {
    LG_UNLOCK(files.lock);
    return LG_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY;
  }
  if (!job->request.node)
  {
    struct LocalNode ** roots = NULL;
    size_t rootCount = 0;
    size_t rootCapacity = 0;
    for (struct LocalNode * node = job->dataset->nodes;
        node; node = node->next)
      if (!node->parent)
      {
        if (rootCount == SIZE_MAX / sizeof(*roots))
        {
          LG_UNLOCK(files.lock);
          free(roots);
          return LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
        }
        if (!grow((void **)&roots, &rootCapacity,
              (rootCount + 1U) * sizeof(*roots)))
        {
          LG_UNLOCK(files.lock);
          free(roots);
          return LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
        }
        roots[rootCount++] = node;
      }
    LG_UNLOCK(files.lock);

    for (size_t i = 0; i < rootCount; ++i)
    {
      if (localJobCancelled(job))
      {
        free(roots);
        free(data);
        return LG_CLIPBOARD_FILE_ERROR_CANCELLED;
      }
      struct stat value;
      const int fd = openLocalNode(roots[i], &value);
      if (fd < 0)
      {
        error = fileErrorFromErrno(errno);
        free(roots);
        free(data);
        return error;
      }
      close(fd);
      error = appendLocalEntry(&data, &size, &capacity, roots[i]);
      if (error)
      {
        free(roots);
        free(data);
        return error;
      }
    }
    for (size_t i = 0; i < rootCount; ++i)
    {
      if (localJobCancelled(job))
      {
        free(roots);
        free(data);
        return LG_CLIPBOARD_FILE_ERROR_CANCELLED;
      }
      struct stat value;
      const int fd = openLocalNode(roots[i], &value);
      if (fd < 0)
      {
        error = fileErrorFromErrno(errno);
        free(roots);
        free(data);
        return error;
      }
      close(fd);
    }
    free(roots);
    const bool result = sendBufferJob(job, data, size);
    free(data);
    return result ? LG_CLIPBOARD_FILE_ERROR_NONE :
      localJobCancelled(job) ? LG_CLIPBOARD_FILE_ERROR_CANCELLED :
      LG_CLIPBOARD_FILE_ERROR_DISCONNECTED;
  }
  LG_UNLOCK(files.lock);

  if (localJobCancelled(job))
    return LG_CLIPBOARD_FILE_ERROR_CANCELLED;
  struct stat directoryBefore;
  const int directoryFd = openLocalNode(directory, &directoryBefore);
  if (directoryFd < 0)
    return fileErrorFromErrno(errno);
  DIR * dir = fdopendir(directoryFd);
  if (!dir)
  {
    error = fileErrorFromErrno(errno);
    close(directoryFd);
    return error;
  }

  for (;;)
  {
    if (localJobCancelled(job))
    {
      error = LG_CLIPBOARD_FILE_ERROR_CANCELLED;
      goto failed;
    }
    errno = 0;
    struct dirent * entry = readdir(dir);
    if (!entry)
    {
      if (errno)
      {
        error = fileErrorFromErrno(errno);
        goto failed;
      }
      break;
    }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;

    struct stat before;
    if (fstatat(directoryFd, entry->d_name, &before,
          AT_SYMLINK_NOFOLLOW) < 0)
    {
      error = fileErrorFromErrno(errno);
      goto failed;
    }
    if (S_ISLNK(before.st_mode))
    {
      error = LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED;
      goto failed;
    }
    if (!localStatSupported(&before))
    {
      error = LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED;
      goto failed;
    }

    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    if (S_ISDIR(before.st_mode))
      flags |= O_DIRECTORY;
    const int childFd = openat(directoryFd, entry->d_name, flags);
    if (childFd < 0)
    {
      error = fileErrorFromErrno(errno);
      goto failed;
    }
    struct stat value;
    if (fstat(childFd, &value) < 0)
    {
      error = fileErrorFromErrno(errno);
      close(childFd);
      goto failed;
    }
    const bool stable = localStatEqual(&before, &value);
    close(childFd);
    if (!stable)
    {
      error = LG_CLIPBOARD_FILE_ERROR_STALE;
      goto failed;
    }

    const size_t parentLength = strlen(directory->path);
    const size_t nameLength = strlen(entry->d_name);
    if (parentLength > SIZE_MAX - nameLength - 2U)
    {
      error = LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
      goto failed;
    }
    const size_t pathLength = parentLength + (parentLength ? 1U : 0U) +
      nameLength + 1U;
    char * childPath = malloc(pathLength);
    if (!childPath)
    {
      error = LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
      goto failed;
    }
    if (parentLength)
      snprintf(childPath, pathLength, "%s/%s",
          directory->path, entry->d_name);
    else
      memcpy(childPath, entry->d_name, nameLength + 1U);

    LG_LOCK(files.lock);
    struct LocalNode * node = localChildNL(
        job->dataset, directory->node, entry->d_name);
    if (!node)
      node = addLocalNode(job->dataset, directory->node,
          directory->root, childPath, entry->d_name, &value);
    else
      free(childPath);
    if (!node)
      error = LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
    else if (!localNodeMatches(node, &value))
      error = LG_CLIPBOARD_FILE_ERROR_STALE;
    else
      error = appendLocalEntry(&data, &size, &capacity, node);
    LG_UNLOCK(files.lock);
    if (error)
      goto failed;
    if (listedCount == SIZE_MAX / sizeof(*listed) ||
        !grow((void **)&listed, &listedCapacity,
          (listedCount + 1U) * sizeof(*listed)))
    {
      error = LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
      goto failed;
    }
    listed[listedCount++] = node;
  }

  for (size_t i = 0; i < listedCount; ++i)
  {
    if (localJobCancelled(job))
    {
      error = LG_CLIPBOARD_FILE_ERROR_CANCELLED;
      goto failed;
    }
    struct stat value;
    const int fd = openLocalNode(listed[i], &value);
    if (fd < 0)
    {
      error = fileErrorFromErrno(errno);
      goto failed;
    }
    close(fd);
  }
  struct stat directoryAfter;
  if (localJobCancelled(job))
  {
    error = LG_CLIPBOARD_FILE_ERROR_CANCELLED;
    goto failed;
  }
  if (fstat(directoryFd, &directoryAfter) < 0)
  {
    error = fileErrorFromErrno(errno);
    goto failed;
  }
  if (!localNodeMatches(directory, &directoryAfter))
  {
    error = LG_CLIPBOARD_FILE_ERROR_STALE;
    goto failed;
  }
  if (closedir(dir) < 0)
  {
    const int closeError = errno;
    free(listed);
    free(data);
    return fileErrorFromErrno(closeError);
  }
  free(listed);
  const bool result = sendBufferJob(job, data, size);
  free(data);
  return result ? LG_CLIPBOARD_FILE_ERROR_NONE :
    localJobCancelled(job) ? LG_CLIPBOARD_FILE_ERROR_CANCELLED :
    LG_CLIPBOARD_FILE_ERROR_DISCONNECTED;

failed:
  closedir(dir);
  free(listed);
  free(data);
  return error;
}

static LG_ClipboardFileError localRead(struct LocalJob * job)
{
  LG_LOCK(files.lock);
  struct LocalNode * node = localNodeByIdNL(
      job->dataset, job->request.node);
  LG_UNLOCK(files.lock);
  if (!node)
    return LG_CLIPBOARD_FILE_ERROR_NOT_FOUND;
  if (node->type != KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
    return LG_CLIPBOARD_FILE_ERROR_IS_DIRECTORY;
  if (job->request.offset > node->size ||
      job->request.length > node->size - job->request.offset)
    return LG_CLIPBOARD_FILE_ERROR_INVALID;
  if (localJobCancelled(job))
    return LG_CLIPBOARD_FILE_ERROR_CANCELLED;

  struct stat before;
  const int fd = openLocalNode(node, &before);
  if (fd < 0)
    return fileErrorFromErrno(errno);
  uint8_t * buffer = malloc(job->request.length ? job->request.length : 1U);
  if (!buffer)
  {
    close(fd);
    return LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
  }
  LG_ClipboardFileError error = LG_CLIPBOARD_FILE_ERROR_NONE;
  size_t received = 0;
  while (received < job->request.length)
  {
    if (localJobCancelled(job))
    {
      error = LG_CLIPBOARD_FILE_ERROR_CANCELLED;
      goto failed;
    }
#ifdef ENABLE_TESTS
    const bool forceEof = atomic_exchange_explicit(
        &forceLocalEof, false, memory_order_acq_rel);
    const ssize_t bytes = forceEof ? 0 :
      pread(fd, buffer + received, job->request.length - received,
          (off_t)(job->request.offset + received));
#else
    const ssize_t bytes = pread(fd, buffer + received,
        job->request.length - received,
        (off_t)(job->request.offset + received));
#endif
    if (bytes < 0 && errno == EINTR)
      continue;
    if (bytes < 0)
    {
      error = fileErrorFromErrno(errno);
      goto failed;
    }
    if (!bytes)
    {
      error = LG_CLIPBOARD_FILE_ERROR_IO;
      goto failed;
    }
    received += (size_t)bytes;
  }

  struct stat after;
  if (localJobCancelled(job))
  {
    error = LG_CLIPBOARD_FILE_ERROR_CANCELLED;
    goto failed;
  }
  if (fstat(fd, &after) < 0)
  {
    error = fileErrorFromErrno(errno);
    goto failed;
  }
  if (!localNodeMatches(node, &after) ||
      !localStatEqual(&before, &after))
  {
    error = LG_CLIPBOARD_FILE_ERROR_STALE;
    goto failed;
  }
  if (received != job->request.length)
  {
    error = LG_CLIPBOARD_FILE_ERROR_IO;
    goto failed;
  }
  close(fd);
  const bool result = sendBufferJob(job, buffer, received);
  free(buffer);
  return result ? LG_CLIPBOARD_FILE_ERROR_NONE :
    localJobCancelled(job) ? LG_CLIPBOARD_FILE_ERROR_CANCELLED :
    LG_CLIPBOARD_FILE_ERROR_DISCONNECTED;

failed:
  close(fd);
  free(buffer);
  return error;
}

static int localWorker(void * opaque)
{
  (void)opaque;
  for (;;)
  {
    LG_LOCK(files.lock);
    struct LocalJob * job = files.jobsHead;
    if (job)
    {
      files.jobsHead = job->next;
      if (!files.jobsHead)
        files.jobsTail = &files.jobsHead;
      job->next = files.activeJobs;
      files.activeJobs = job;
    }
    const bool stopping = files.stopping;
    if (!job && !stopping)
      lgResetEvent(files.jobEvent);
    LG_UNLOCK(files.lock);
    if (!job)
    {
      if (stopping)
        return 0;
      lgWaitEvent(files.jobEvent, TIMEOUT_INFINITE);
      continue;
    }
    LG_ClipboardFileError error = LG_CLIPBOARD_FILE_ERROR_CANCELLED;
    if (!atomic_load_explicit(&job->cancelled, memory_order_acquire))
      error = job->request.operation == LG_CLIPBOARD_FILE_LIST ?
        localList(job) : localRead(job);
    if (error && error != LG_CLIPBOARD_FILE_ERROR_CANCELLED &&
        !atomic_load_explicit(&job->cancelled, memory_order_acquire))
      lgClipboard_fileCancel(job->request.dataset,
          job->request.request, error);
    LG_LOCK(files.lock);
    struct LocalJob ** link = &files.activeJobs;
    while (*link && *link != job)
      link = &(*link)->next;
    if (*link)
      *link = job->next;
    if (job->dataset->references)
      --job->dataset->references;
    struct LocalDataset * retired =
      takeUnusedLocalDatasetNL(job->dataset);
    LG_UNLOCK(files.lock);
    freeLocalDataset(retired);
    lgFreeEvent(job->ready);
    free(job);
  }
}

void lgClipboardFiles_localRequest(
    const LG_ClipboardFileRequest * request)
{
  if (!request || !request->request ||
      (request->operation != LG_CLIPBOARD_FILE_LIST &&
       request->operation != LG_CLIPBOARD_FILE_READ) ||
      (request->operation == LG_CLIPBOARD_FILE_LIST &&
       (request->offset || request->length)) ||
      (request->operation == LG_CLIPBOARD_FILE_READ &&
       (!request->length || request->length > FILE_REQUEST_MAX ||
        request->offset > INT64_MAX ||
        request->length > (uint64_t)INT64_MAX - request->offset)))
  {
    if (request)
      lgClipboard_fileCancel(request->dataset, request->request,
          LG_CLIPBOARD_FILE_ERROR_INVALID);
    return;
  }
  struct LocalJob * job = calloc(1, sizeof(*job));
  if (!job)
  {
    lgClipboard_fileCancel(request->dataset, request->request,
        LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    return;
  }
  job->ready = lgCreateEvent(true, 0);
  if (!job->ready)
  {
    free(job);
    lgClipboard_fileCancel(request->dataset, request->request,
        LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    return;
  }
  atomic_init(&job->cancelled, false);
  job->request = *request;
  LG_LOCK(files.lock);
  job->dataset = acquiredLocalDatasetNL(request->dataset);
  if (job->dataset && !files.stopping)
  {
    ++job->dataset->references;
    *files.jobsTail = job;
    files.jobsTail  = &job->next;
  }
  const bool queued = job->dataset && !files.stopping;
  LG_UNLOCK(files.lock);
  if (queued)
  {
    lgSignalEvent(files.jobEvent);
    return;
  }
  lgFreeEvent(job->ready);
  free(job);
  lgClipboard_fileCancel(request->dataset, request->request,
      LG_CLIPBOARD_FILE_ERROR_STALE);
}

void lgClipboardFiles_localCancel(uint64_t dataset,
    uint64_t request, LG_ClipboardFileError reason)
{
  (void)reason;
  LG_LOCK(files.lock);
  struct LocalAcquisition ** acquisition = &files.localAcquisitions;
  struct LocalDataset * retired = NULL;
  while (*acquisition && ((*acquisition)->wireDataset != dataset ||
      (*acquisition)->acquisition != request))
    acquisition = &(*acquisition)->next;
  if (*acquisition)
  {
    struct LocalAcquisition * cancelled = *acquisition;
    *acquisition = cancelled->next;
    if (cancelled->dataset->references)
      --cancelled->dataset->references;
    retired = takeUnusedLocalDatasetNL(cancelled->dataset);
    free(cancelled);
  }
  for (struct LocalJob * job = files.activeJobs; job; job = job->next)
    if (job->request.dataset == dataset && job->request.request == request)
    {
      atomic_store_explicit(&job->cancelled, true, memory_order_release);
      lgSignalEvent(job->ready);
    }
  for (struct LocalJob * job = files.jobsHead; job; job = job->next)
    if (job->request.dataset == dataset && job->request.request == request)
      atomic_store_explicit(&job->cancelled, true, memory_order_release);
  LG_UNLOCK(files.lock);
  freeLocalDataset(retired);
}

void lgClipboardFiles_localReady(uint64_t request)
{
  LG_LOCK(files.lock);
  for (struct LocalJob * job = files.activeJobs; job; job = job->next)
    if (job->request.request == request)
      lgSignalEvent(job->ready);
  LG_UNLOCK(files.lock);
}

static void freeState(void)
{
  struct RemoteRequest * request = files.remoteRequests;
  while (request)
  {
    struct RemoteRequest * next = request->next;
    unregisterRemoteInterrupt(request);
    if (request->fuseRequest)
      fuse_reply_err(request->fuseRequest, EIO);
    freeRemoteRequest(request);
    request = next;
  }
  struct RemoteNode * remoteNode = files.remoteNodes;
  while (remoteNode)
  {
    struct RemoteNode * next = remoteNode->next;
    free(remoteNode->name);
    free(remoteNode);
    remoteNode = next;
  }
  struct RemoteDataset * remote = files.remoteDatasets;
  while (remote)
  {
    struct RemoteDataset * next = remote->next;
    if (remote->acquired)
      lgClipboard_fileRelease(remote->dataset, remote->acquisition);
    else if (remote->acquiring)
      lgClipboard_fileCancel(remote->dataset, remote->acquisition,
          LG_CLIPBOARD_FILE_ERROR_CANCELLED);
    free(remote);
    remote = next;
  }
  struct LocalAcquisition * acquisition = files.localAcquisitions;
  while (acquisition)
  {
    struct LocalAcquisition * next = acquisition->next;
    free(acquisition);
    acquisition = next;
  }
  struct LocalDataset * local = files.localDatasets;
  while (local)
  {
    struct LocalDataset * next = local->next;
    freeLocalDataset(local);
    local = next;
  }
}

bool lgClipboardFiles_init(void)
{
  if (filesLockInitialized)
    return false;
  memset(&files, 0, sizeof(files));
  files.fuseStopFd = -1;
  LG_LOCK_INIT(files.lock);
  filesLockInitialized = true;
  uint64_t nonce = 0;
  if (!randomBytes(&nonce, sizeof(nonce)))
  {
    DEBUG_ERROR("Failed to seed clipboard file transfer identifiers: %s",
        strerror(errno));
    goto failed;
  }
  nonce &= ~KVMFR_CLIPBOARD_TRANSFER_HELPER;
  if (!nonce)
    nonce = 1;
  files.requestSerial = nonce;
  files.datasetSerial = (nonce ^ UINT64_C(0x4c474346494c4553)) &
    ~KVMFR_CLIPBOARD_TRANSFER_HELPER;
  if (!files.datasetSerial)
    files.datasetSerial = 1;
  files.inodeSerial = FUSE_ROOT_ID;
  files.jobsTail    = &files.jobsHead;
  files.jobEvent    = lgCreateEvent(false, 0);
  if (!files.jobEvent)
    goto failed;
  files.fuseStopFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (files.fuseStopFd < 0)
  {
    DEBUG_ERROR("Failed to create the clipboard FUSE stop event: %s",
        strerror(errno));
    goto failed;
  }
  const char * runtime = getenv("XDG_RUNTIME_DIR");
  if (!runtime || !*runtime)
  {
    DEBUG_ERROR("XDG_RUNTIME_DIR is required for clipboard file transfer");
    goto failed;
  }
  const int length = snprintf(files.mountpoint, sizeof(files.mountpoint),
      "%s/looking-glass-clipboard-XXXXXX", runtime);
  if (length < 0 || (size_t)length >= sizeof(files.mountpoint) ||
      !mkdtemp(files.mountpoint))
  {
    DEBUG_ERROR("Failed to create the clipboard FUSE mountpoint: %s",
        strerror(errno));
    goto failed;
  }
  chmod(files.mountpoint, 0700);
  char mountOptions[160];
  const int optionsLength = snprintf(mountOptions, sizeof(mountOptions),
      "ro,nodev,nosuid,noexec,default_permissions,auto_unmount,max_read=%u",
      (unsigned)FILE_REQUEST_MAX);
  if (optionsLength < 0 || (size_t)optionsLength >= sizeof(mountOptions))
  {
    DEBUG_ERROR("Failed to format clipboard FUSE mount options");
    goto failed;
  }
  struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
  if (fuse_opt_add_arg(&args, "looking-glass-client") < 0 ||
      fuse_opt_add_arg(&args, "-o") < 0 ||
      fuse_opt_add_arg(&args, mountOptions) < 0)
  {
    fuse_opt_free_args(&args);
    goto failed;
  }
  files.session = fuse_session_new(&args, &fuseOps,
      sizeof(fuseOps), NULL);
  fuse_opt_free_args(&args);
  if (!files.session || fuse_session_mount(
      files.session, files.mountpoint) < 0)
  {
    DEBUG_ERROR("Failed to mount the clipboard FUSE filesystem");
    goto failed;
  }
  if (!lgCreateThread("clipboard-fuse", fuseThread,
      files.session, &files.fuseThread))
    goto failed;
  unsigned created = 0;
  for (; created < FILE_WORKERS; ++created)
    if (!lgCreateThread("clipboard-file", localWorker,
        NULL, &files.workers[created]))
      break;
  if (created != FILE_WORKERS)
    goto failed;
  files.initialized = true;
  DEBUG_INFO("Clipboard files mounted at %s", files.mountpoint);
  return true;

failed:
  lgClipboardFiles_free();
  return false;
}

#ifdef ENABLE_TESTS
bool lgClipboardFiles_testFuseStopWake(void)
{
  const int stopFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (stopFd < 0)
    return false;
  const bool result = wakeFuseThread(stopFd) &&
    waitFuseEvent(-1, stopFd, -1) == FUSE_WAIT_WAKE;
  close(stopFd);
  return result;
}

bool lgClipboardFiles_testUnsentRemoteOwnership(void)
{
  return finishUnsentRemoteRequest(0);
}

bool lgClipboardFiles_testInit(uint64_t nonce)
{
  if (filesLockInitialized)
    return false;
  nonce &= ~KVMFR_CLIPBOARD_TRANSFER_HELPER;
  if (!nonce)
    return false;
  memset(&files, 0, sizeof(files));
  files.fuseStopFd = -1;
  LG_LOCK_INIT(files.lock);
  filesLockInitialized = true;
  files.requestSerial = nonce;
  files.datasetSerial = (nonce ^ UINT64_C(0x4c474346494c4553)) &
    ~KVMFR_CLIPBOARD_TRANSFER_HELPER;
  if (!files.datasetSerial)
    files.datasetSerial = 1;
  files.inodeSerial   = FUSE_ROOT_ID;
  files.jobsTail      = &files.jobsHead;
  files.initialized   = true;
  atomic_store_explicit(&forceLocalEof, false, memory_order_release);
  return true;
}

size_t lgClipboardFiles_testRemoteDatasetCount(void)
{
  if (!filesLockInitialized)
    return 0;
  size_t count = 0;
  LG_LOCK(files.lock);
  for (struct RemoteDataset * dataset = files.remoteDatasets;
      dataset; dataset = dataset->next)
    ++count;
  LG_UNLOCK(files.lock);
  return count;
}

void lgClipboardFiles_testExpireRemoteDeliveries(void)
{
  if (!filesLockInitialized)
    return;
  LG_LOCK(files.lock);
  for (struct RemoteDataset * dataset = files.remoteDatasets;
      dataset; dataset = dataset->next)
    if (dataset->deliveryPending)
      dataset->deliveryDeadline = 0;
  LG_UNLOCK(files.lock);
  reapRemoteDeliveries(false);
}

bool lgClipboardFiles_testBeginRemoteLookup(uint64_t presentation)
{
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset =
    remoteDatasetByAcquisitionNL(presentation);
  const bool result = dataset && dataset->ready &&
    pinRemoteOperationNL(dataset, true);
  LG_UNLOCK(files.lock);
  return result;
}

void lgClipboardFiles_testEndRemoteLookup(uint64_t presentation)
{
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset =
    remoteDatasetByAcquisitionNL(presentation);
  if (dataset && dataset->operationPins)
    --dataset->operationPins;
  const struct RemoteAction action = retireRemoteDatasetNL(dataset);
  LG_UNLOCK(files.lock);
  performRemoteAction(action);
}

bool lgClipboardFiles_testRemoteRead(uint64_t presentation,
    uint64_t nodeId, uint64_t offset, uint32_t length)
{
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset =
    remoteDatasetByAcquisitionNL(presentation);
  struct RemoteNode * node = dataset ?
    remoteNodeByWireNL(dataset, nodeId) : NULL;
  const bool pinned = node && length &&
    pinRemoteOperationNL(dataset, false);
  LG_UNLOCK(files.lock);
  if (!pinned)
    return false;
  const bool result = requestRemote(dataset, node,
      LG_CLIPBOARD_FILE_READ, offset, length, REMOTE_REPLY_READ,
      NULL, 0, 0, NULL);
  releaseRemoteOperation(dataset);
  return result;
}

void lgClipboardFiles_testForceLocalEof(void)
{
  atomic_store_explicit(&forceLocalEof, true, memory_order_release);
}

bool lgClipboardFiles_testLocalRequest(
    const LG_ClipboardFileRequest * request)
{
  if (!request ||
      (request->operation != LG_CLIPBOARD_FILE_LIST &&
       request->operation != LG_CLIPBOARD_FILE_READ))
    return false;

  struct LocalJob job = { .request = *request };
  atomic_init(&job.cancelled, false);
  LG_LOCK(files.lock);
  job.dataset = acquiredLocalDatasetNL(request->dataset);
  if (job.dataset)
    ++job.dataset->references;
  LG_UNLOCK(files.lock);
  if (!job.dataset)
    return false;

  const LG_ClipboardFileError error =
    request->operation == LG_CLIPBOARD_FILE_LIST ?
    localList(&job) : localRead(&job);
  if (error && error != LG_CLIPBOARD_FILE_ERROR_CANCELLED)
    lgClipboard_fileCancel(request->dataset, request->request, error);
  LG_LOCK(files.lock);
  if (job.dataset->references)
    --job.dataset->references;
  LG_UNLOCK(files.lock);
  return error == LG_CLIPBOARD_FILE_ERROR_NONE;
}
#endif

void lgClipboardFiles_free(void)
{
  if (!filesLockInitialized)
    return;
  LG_LOCK(files.lock);
  files.stopping = true;
  for (struct LocalJob * job = files.jobsHead; job; job = job->next)
    atomic_store_explicit(&job->cancelled, true, memory_order_release);
  for (struct LocalJob * job = files.activeJobs; job; job = job->next)
  {
    atomic_store_explicit(&job->cancelled, true, memory_order_release);
    if (job->ready)
      lgSignalEvent(job->ready);
  }
  LG_UNLOCK(files.lock);
  if (files.jobEvent)
    lgSignalEvent(files.jobEvent);
  for (unsigned i = 0; i < FILE_WORKERS; ++i)
    if (files.workers[i])
    {
      lgSignalEvent(files.jobEvent);
      lgJoinThread(files.workers[i], NULL);
      files.workers[i] = NULL;
    }
  if (files.session)
    fuse_session_exit(files.session);
  if (files.fuseThread)
  {
    if (!wakeFuseThread(files.fuseStopFd))
      DEBUG_WARN("Failed to wake the clipboard FUSE thread: %s",
          strerror(errno));
    lgJoinThread(files.fuseThread, NULL);
    files.fuseThread = NULL;
  }
  if (files.fuseStopFd >= 0)
  {
    close(files.fuseStopFd);
    files.fuseStopFd = -1;
  }
  /* Pending requests belong to the stopped but still-live session. */
  freeState();
  if (files.session)
  {
    fuse_session_unmount(files.session);
    fuse_session_destroy(files.session);
    files.session = NULL;
  }
  if (files.jobEvent)
  {
    lgFreeEvent(files.jobEvent);
    files.jobEvent = NULL;
  }
  if (files.mountpoint[0] && rmdir(files.mountpoint) < 0 && errno != ENOENT)
    DEBUG_WARN("Failed to remove clipboard mountpoint: %s", strerror(errno));
  files.mountpoint[0] = 0;
  files.initialized = false;
  LG_LOCK_FREE(files.lock);
  filesLockInitialized = false;
}
