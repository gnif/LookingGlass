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

#include "core/clipboard_files.h"
#include "core/clipboard.h"
#include "platform/windows/clipboard_files.h"
#include "platform/windows/clipboard_files_win32.h"

#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"
#include "common/random.h"
#include "common/thread.h"

#include <LGProtocol/KVMFRClipboard.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL_WORKERS     2U
#define MAX_LIST_BYTES    (64U * 1024U * 1024U)
#define REMOTE_TIMEOUT_MS 30000U

enum RemoteActionType
{
  REMOTE_ACTION_NONE,
  REMOTE_ACTION_CANCEL,
  REMOTE_ACTION_RELEASE,
};

struct LocalDataset
{
  struct LocalDataset        * next;
  WindowsClipboardLocalFiles * files;
  uint64_t                     dataset;
  unsigned int                 references;
  bool                         current;
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
  struct LocalJob         * next;
  struct LocalDataset     * dataset;
  LG_ClipboardFileRequest   request;
  LGEvent                 * ready;
  atomic_bool               cancelled;
};

struct RemoteDataset
{
  struct RemoteDataset           * next;
  WindowsClipboardRemoteManifest * manifestData;
  uint64_t                         dataset;
  uint64_t                         acquisition;
  uint64_t                         currentRequest;
  unsigned int                     presentationPins;
  bool                             current;
  bool                             acquiring;
  bool                             acquired;
  bool                             manifest;
  bool                             ready;
  bool                             stale;
};

struct RemoteRequest
{
  struct RemoteRequest   * next;
  struct RemoteDataset   * dataset;
  LG_ClipboardFileRequest  request;
  LGEvent                * completeEvent;
  uint8_t                * data;
  size_t                   dataSize;
  size_t                   dataCapacity;
  uint8_t                * output;
  size_t                   outputCapacity;
  uint64_t                 sizeHint;
  LG_ClipboardFileError    error;
  bool                     began;
  bool                     complete;
  bool                     manifest;
};

struct RemoteAction
{
  enum RemoteActionType   type;
  struct RemoteDataset  * object;
  uint64_t                dataset;
  uint64_t                acquisition;
};

struct RemoteProviderContext
{
  LG_Lock       lock;
  LGEvent     * idle;
  unsigned int  references;
  unsigned int  calls;
  bool          accepting;
};

static struct
{
  LG_Lock                        lock;
  bool                           lockInitialized;
  bool                           initialized;
  bool                           stopping;
  uint64_t                       transferSerial;
  uint64_t                       datasetSerial;
  struct LocalDataset          * localDatasets;
  struct LocalDataset          * localCurrent;
  struct LocalAcquisition      * localAcquisitions;
  struct LocalJob              * jobsHead;
  struct LocalJob             ** jobsTail;
  struct LocalJob              * activeJobs;
  struct RemoteDataset         * remoteDatasets;
  struct RemoteDataset         * remoteCurrent;
  struct RemoteRequest         * remoteRequests;
  LGEvent                      * jobEvent;
  struct RemoteProviderContext * provider;
  LGThread                     * workers[LOCAL_WORKERS];
}
files;

static HRESULT fileErrorHRESULT(LG_ClipboardFileError error)
{
  switch (error)
  {
    case LG_CLIPBOARD_FILE_ERROR_NONE:
      return S_OK;
    case LG_CLIPBOARD_FILE_ERROR_ACCESS:
      return STG_E_ACCESSDENIED;
    case LG_CLIPBOARD_FILE_ERROR_NO_MEMORY:
      return E_OUTOFMEMORY;
    case LG_CLIPBOARD_FILE_ERROR_INVALID:
      return STG_E_INVALIDPARAMETER;
    default:
      return STG_E_READFAULT;
  }
}

static uint64_t nextTransferNL(void)
{
  do
    ++files.transferSerial;
  while (!files.transferSerial ||
      kvmfrClipboardTransferFromHelper(files.transferSerial));
  return files.transferSerial;
}

static uint64_t nextDatasetNL(void)
{
  do
    ++files.datasetSerial;
  while (!files.datasetSerial ||
      kvmfrClipboardTransferFromHelper(files.datasetSerial));
  return files.datasetSerial;
}

static bool growBuffer(uint8_t ** buffer, size_t * capacity, size_t wanted)
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

static struct LocalDataset * localDatasetByIdNL(uint64_t dataset)
{
  for (struct LocalDataset * item = files.localDatasets;
      item; item = item->next)
    if (item->dataset == dataset)
      return item;
  return NULL;
}

static struct LocalDataset * acquiredLocalDatasetNL(uint64_t dataset)
{
  for (struct LocalAcquisition * item = files.localAcquisitions;
      item; item = item->next)
    if (item->wireDataset == dataset)
      return item->dataset;
  return NULL;
}

static bool localAcquisitionIdExistsNL(uint64_t acquisition)
{
  for (struct LocalAcquisition * item = files.localAcquisitions;
      item; item = item->next)
    if (item->acquisition == acquisition)
      return true;
  return false;
}

static bool localJobIdExistsNL(uint64_t request)
{
  for (struct LocalJob * job = files.jobsHead; job; job = job->next)
    if (job->request.request == request)
      return true;
  for (struct LocalJob * job = files.activeJobs; job; job = job->next)
    if (job->request.request == request)
      return true;
  return false;
}

static size_t localAcquisitionCountNL(void)
{
  size_t count = 0;
  for (struct LocalAcquisition * item = files.localAcquisitions;
      item; item = item->next)
    ++count;
  return count;
}

static size_t localJobCountNL(void)
{
  size_t count = 0;
  for (struct LocalJob * job = files.jobsHead; job; job = job->next)
    ++count;
  for (struct LocalJob * job = files.activeJobs; job; job = job->next)
    ++count;
  return count;
}

static size_t remoteDatasetCountNL(void)
{
  size_t count = 0;
  for (struct RemoteDataset * item = files.remoteDatasets;
      item; item = item->next)
    ++count;
  return count;
}

static size_t remoteRequestCountNL(void)
{
  size_t count = 0;
  for (struct RemoteRequest * item = files.remoteRequests;
      item; item = item->next)
    ++count;
  return count;
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
  *link         = dataset->next;
  dataset->next = NULL;
  return dataset;
}

static struct LocalDataset * takeAllUnusedLocalDatasetsNL(void)
{
  struct LocalDataset  * retired = NULL;
  struct LocalDataset ** link    = &files.localDatasets;
  while (*link)
  {
    struct LocalDataset * dataset = *link;
    if (dataset->current || dataset->references)
    {
      link = &dataset->next;
      continue;
    }
    *link         = dataset->next;
    dataset->next = retired;
    retired       = dataset;
  }
  return retired;
}

static void freeLocalDatasets(struct LocalDataset * datasets)
{
  while (datasets)
  {
    struct LocalDataset * next = datasets->next;
    windowsClipboardLocalFilesRelease(datasets->files);
    free(datasets);
    datasets = next;
  }
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

static struct RemoteRequest * remoteRequestByIdNL(uint64_t request)
{
  for (struct RemoteRequest * item = files.remoteRequests;
      item; item = item->next)
    if (item->request.request == request)
      return item;
  return NULL;
}

static struct RemoteRequest * takeRemoteRequestNL(uint64_t request)
{
  struct RemoteRequest ** link = &files.remoteRequests;
  while (*link && (*link)->request.request != request)
    link = &(*link)->next;
  struct RemoteRequest * result = *link;
  if (result)
  {
    *link        = result->next;
    result->next = NULL;
  }
  return result;
}

static void freeRemoteRequest(struct RemoteRequest * request)
{
  if (!request)
    return;
  if (request->completeEvent)
    lgFreeEvent(request->completeEvent);
  free(request->data);
  free(request);
}

static void cancelRemoteManifest(struct RemoteRequest * request,
    LG_ClipboardFileError reason)
{
  if (!request)
    return;

  if (!clipboard_fileCancel(request->request.dataset,
        request->request.request, reason))
    DEBUG_WARN("Failed to cancel a clipboard file manifest request");
  freeRemoteRequest(request);
}

/* files.lock must be held. Read requests remain owned by their waiting
 * providers; only manifest requests are returned to the caller. */
static struct RemoteRequest * disconnectRemoteRequestsNL(void)
{
  struct RemoteRequest * manifests = NULL;
  struct RemoteRequest * request   = files.remoteRequests;
  files.remoteRequests             = NULL;

  while (request)
  {
    struct RemoteRequest * next = request->next;
    request->next     = NULL;
    request->error    = LG_CLIPBOARD_FILE_ERROR_DISCONNECTED;
    request->complete = true;
    if (request->manifest)
    {
      request->next = manifests;
      manifests     = request;
    }
    else if (request->completeEvent)
      lgSignalEvent(request->completeEvent);
    request = next;
  }

  return manifests;
}

static struct RemoteAction retireRemoteDatasetNL(
    struct RemoteDataset * dataset)
{
  struct RemoteAction action = { 0 };
  if (!dataset || dataset->current || dataset->manifest ||
      dataset->presentationPins)
    return action;

  struct RemoteDataset ** link = &files.remoteDatasets;
  while (*link && *link != dataset)
    link = &(*link)->next;
  if (!*link)
    return action;
  *link = dataset->next;

  dataset->next       = NULL;
  dataset->ready      = false;
  dataset->stale      = true;
  action.object       = dataset;
  action.dataset      = dataset->dataset;
  action.acquisition  = dataset->acquisition;
  if (dataset->acquired)
    action.type = REMOTE_ACTION_RELEASE;
  else if (dataset->acquiring)
    action.type = REMOTE_ACTION_CANCEL;
  dataset->acquired  = false;
  dataset->acquiring = false;
  return action;
}

static void performRemoteAction(struct RemoteAction action)
{
  switch (action.type)
  {
    case REMOTE_ACTION_CANCEL:
      if (!clipboard_fileCancel(action.dataset, action.acquisition,
            LG_CLIPBOARD_FILE_ERROR_CANCELLED))
        DEBUG_WARN("Failed to cancel a clipboard file acquisition");
      break;

    case REMOTE_ACTION_RELEASE:
      if (!clipboard_fileRelease(action.dataset, action.acquisition))
        DEBUG_WARN("Failed to release a clipboard file acquisition");
      break;

    case REMOTE_ACTION_NONE:
      break;
  }

  if (action.object)
  {
    windowsClipboardRemoteManifestFree(action.object->manifestData);
    free(action.object);
  }
}

static bool sameRemoteRequest(const struct RemoteRequest * request,
    const LG_ClipboardFileRequest * descriptor)
{
  return request->request.dataset == descriptor->dataset &&
    request->request.request      == descriptor->request &&
    request->request.node         == descriptor->node &&
    request->request.offset       == descriptor->offset &&
    request->request.length       == descriptor->length &&
    request->request.operation    == descriptor->operation;
}

static void failRemoteDataset(struct RemoteDataset * dataset,
    LG_ClipboardFileError error)
{
  if (!dataset)
    return;

  struct RemoteRequest * request = NULL;
  struct RemoteAction    action  = { 0 };
  uint64_t               failed  = 0;
  LG_LOCK(files.lock);
  if (!dataset->stale && remoteDatasetByIdNL(dataset->dataset) == dataset)
  {
    failed = dataset->dataset;
    if (dataset->currentRequest)
      request = takeRemoteRequestNL(dataset->currentRequest);
    dataset->currentRequest = 0;
    dataset->manifest       = false;
    dataset->current        = false;
    dataset->ready          = false;
    if (files.remoteCurrent == dataset)
      files.remoteCurrent = NULL;
    action = retireRemoteDatasetNL(dataset);
  }
  LG_UNLOCK(files.lock);

  if (request)
    cancelRemoteManifest(request, error);
  performRemoteAction(action);
  if (failed)
    clipboard_fileRemoteFailed(failed);
}

static bool startRemoteList(struct RemoteDataset * dataset, uint64_t parent)
{
  struct RemoteRequest * request = calloc(1, sizeof(*request));
  if (!request)
  {
    failRemoteDataset(dataset, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    return false;
  }

  LG_ClipboardFileError error = LG_CLIPBOARD_FILE_ERROR_NO_SPACE;
  LG_LOCK(files.lock);
  if (!files.stopping && dataset && !dataset->stale &&
      dataset->current && dataset->manifest &&
      !dataset->currentRequest &&
      remoteRequestCountNL() + localJobCountNL() <
        KVMFR_CLIPBOARD_FILE_MAX_REQUESTS)
  {
    request->dataset           = dataset;
    request->request.dataset   = dataset->dataset;
    request->request.request   = nextTransferNL();
    request->request.node      = parent;
    request->request.operation = LG_CLIPBOARD_FILE_LIST;
    request->sizeHint          = LG_CLIPBOARD_SIZE_UNKNOWN;
    request->manifest          = true;
    request->next              = files.remoteRequests;
    files.remoteRequests       = request;
    dataset->currentRequest    = request->request.request;
    error                      = LG_CLIPBOARD_FILE_ERROR_NONE;
  }
  LG_UNLOCK(files.lock);

  if (error)
  {
    freeRemoteRequest(request);
    failRemoteDataset(dataset, error);
    return false;
  }
  if (clipboard_fileRequest(&request->request))
    return true;

  LG_LOCK(files.lock);
  struct RemoteRequest * removed = takeRemoteRequestNL(
      request->request.request);
  if (dataset->currentRequest == request->request.request)
    dataset->currentRequest = 0;
  LG_UNLOCK(files.lock);
  freeRemoteRequest(removed);
  failRemoteDataset(dataset, LG_CLIPBOARD_FILE_ERROR_DISCONNECTED);
  return false;
}

static void continueRemoteManifest(struct RemoteRequest * request)
{
  if (!request || !request->dataset)
  {
    freeRemoteRequest(request);
    return;
  }

  struct RemoteDataset * dataset = request->dataset;
  LG_ClipboardFileError  error   = request->error;
  uint64_t               next    = 0;
  bool                   ready   = false;

  LG_LOCK(files.lock);
  const bool current = !dataset->stale && dataset->current &&
    dataset->manifest &&
    dataset->currentRequest == request->request.request;
  if (current && !error && !windowsClipboardRemoteManifestAppendList(
        dataset->manifestData, request->request.node, request->data,
        request->dataSize, &error))
    error = error ? error : LG_CLIPBOARD_FILE_ERROR_INVALID;
  if (current && !error)
  {
    dataset->currentRequest = 0;
    const bool hasNext = windowsClipboardRemoteManifestNextDirectory(
        dataset->manifestData, &next);
    if (!hasNext)
    {
      if (!windowsClipboardRemoteManifestHasEntries(dataset->manifestData))
        error = LG_CLIPBOARD_FILE_ERROR_INVALID;
      else
      {
        dataset->manifest = false;
        dataset->ready    = true;
        ready             = true;
      }
    }
  }
  LG_UNLOCK(files.lock);
  freeRemoteRequest(request);

  if (!current)
    return;
  if (error)
  {
    failRemoteDataset(dataset, error);
    return;
  }
  if (next)
  {
    startRemoteList(dataset, next);
    return;
  }
  if (ready)
    clipboard_fileRemoteReady(dataset->dataset);
}

static struct RemoteProviderContext * providerCreate(void)
{
  struct RemoteProviderContext * context = calloc(1, sizeof(*context));
  if (!context)
    return NULL;

  LG_LOCK_INIT(context->lock);
  context->idle = lgCreateEvent(false, 0);
  if (!context->idle)
  {
    LG_LOCK_FREE(context->lock);
    free(context);
    return NULL;
  }
  context->references = 1;
  context->accepting  = true;
  lgSignalEvent(context->idle);
  return context;
}

static void providerRetain(struct RemoteProviderContext * context)
{
  if (!context)
    return;

  LG_LOCK(context->lock);
  ++context->references;
  LG_UNLOCK(context->lock);
}

static void providerRelease(struct RemoteProviderContext * context)
{
  if (!context)
    return;

  LG_LOCK(context->lock);
  const bool destroy = context->references && !--context->references;
  LG_UNLOCK(context->lock);
  if (!destroy)
    return;

  lgFreeEvent(context->idle);
  LG_LOCK_FREE(context->lock);
  free(context);
}

static bool providerEnter(struct RemoteProviderContext * context)
{
  if (!context)
    return false;

  LG_LOCK(context->lock);
  if (!context->accepting)
  {
    LG_UNLOCK(context->lock);
    return false;
  }
  if (!context->calls)
    lgResetEvent(context->idle);
  ++context->calls;
  LG_UNLOCK(context->lock);
  return true;
}

static void providerLeave(struct RemoteProviderContext * context)
{
  LG_LOCK(context->lock);
  if (context->calls && !--context->calls)
    lgSignalEvent(context->idle);
  LG_UNLOCK(context->lock);
}

static void providerDisable(struct RemoteProviderContext * context)
{
  if (!context)
    return;

  LG_LOCK(context->lock);
  context->accepting = false;
  LG_UNLOCK(context->lock);
}

static void providerWait(struct RemoteProviderContext * context)
{
  if (!context)
    return;

  LG_LOCK(context->lock);
  const bool idle = !context->calls;
  LG_UNLOCK(context->lock);
  if (!idle)
    lgWaitEvent(context->idle, TIMEOUT_INFINITE);
}

static HRESULT remoteProviderRead(void * opaque, uint64_t datasetId,
    uint64_t acquisition, uint64_t node, uint64_t offset, void * output,
    ULONG length, ULONG * read)
{
  struct RemoteProviderContext * context = opaque;
  if (!read || (!output && length))
    return STG_E_INVALIDPOINTER;
  *read = 0;
  if (!providerEnter(context))
    return STG_E_READFAULT;

  HRESULT   result      = S_OK;
  uint8_t * destination = output;
  while (length)
  {
    const uint32_t         wanted  =
      length > KVMFR_CLIPBOARD_FILE_READ_BYTES ?
      KVMFR_CLIPBOARD_FILE_READ_BYTES : (uint32_t)length;
    struct RemoteRequest * request = calloc(1, sizeof(*request));
    if (!request)
    {
      result = E_OUTOFMEMORY;
      break;
    }
    request->completeEvent = lgCreateEvent(true, 0);
    if (!request->completeEvent)
    {
      freeRemoteRequest(request);
      result = E_OUTOFMEMORY;
      break;
    }

    LG_LOCK(files.lock);
    struct RemoteDataset * dataset = remoteDatasetByIdNL(datasetId);
    const bool             valid   = !files.stopping && dataset &&
      dataset->acquisition == acquisition &&
      dataset->acquired && !dataset->stale && dataset->presentationPins &&
      remoteRequestCountNL() + localJobCountNL() <
        KVMFR_CLIPBOARD_FILE_MAX_REQUESTS;
    if (valid)
    {
      request->dataset           = dataset;
      request->request.dataset   = datasetId;
      request->request.request   = nextTransferNL();
      request->request.node      = node;
      request->request.offset    = offset;
      request->request.length    = wanted;
      request->request.operation = LG_CLIPBOARD_FILE_READ;
      request->output            = destination;
      request->outputCapacity    = wanted;
      request->sizeHint          = LG_CLIPBOARD_SIZE_UNKNOWN;
      request->next              = files.remoteRequests;
      files.remoteRequests       = request;
    }
    LG_UNLOCK(files.lock);
    if (!valid)
    {
      freeRemoteRequest(request);
      result = STG_E_READFAULT;
      break;
    }

    if (!clipboard_fileRequest(&request->request))
    {
      LG_LOCK(files.lock);
      takeRemoteRequestNL(request->request.request);
      LG_UNLOCK(files.lock);
      freeRemoteRequest(request);
      result = STG_E_READFAULT;
      break;
    }

    const bool signalled = lgWaitEvent(request->completeEvent,
        REMOTE_TIMEOUT_MS);
    bool       removed;
    LG_LOCK(files.lock);
    removed = takeRemoteRequestNL(request->request.request) == request;
    if (!request->complete)
    {
      request->error    = LG_CLIPBOARD_FILE_ERROR_IO;
      request->complete = true;
    }
    const LG_ClipboardFileError error  = request->error;
    const size_t                actual = request->dataSize;
    LG_UNLOCK(files.lock);
    if (!signalled && removed)
      clipboard_fileCancel(datasetId, request->request.request,
          LG_CLIPBOARD_FILE_ERROR_IO);

    if (error)
    {
      result = fileErrorHRESULT(error);
      freeRemoteRequest(request);
      break;
    }
    if (actual != wanted)
    {
      result = STG_E_READFAULT;
      freeRemoteRequest(request);
      break;
    }

    destination += actual;
    offset      += actual;
    length      -= (ULONG)actual;
    *read       += (ULONG)actual;
    freeRemoteRequest(request);
  }

  providerLeave(context);
  return result;
}

static void remoteProviderRelease(void * opaque, uint64_t datasetId,
    uint64_t acquisition)
{
  struct RemoteProviderContext * context = opaque;
  if (!providerEnter(context))
  {
    providerRelease(context);
    return;
  }

  struct RemoteAction action = { 0 };
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset = datasetId ?
    remoteDatasetByIdNL(datasetId) :
    remoteDatasetByAcquisitionNL(acquisition);
  if (dataset && dataset->acquisition == acquisition &&
      dataset->presentationPins)
  {
    --dataset->presentationPins;
    action = retireRemoteDatasetNL(dataset);
  }
  LG_UNLOCK(files.lock);
  performRemoteAction(action);
  providerLeave(context);
  providerRelease(context);
}

static const WindowsClipboardRemoteProviderOps remoteProviderOps =
{
  .read    = remoteProviderRead,
  .release = remoteProviderRelease,
};

static bool sendLocalCall(struct LocalJob * job, int operation,
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
        result = clipboard_fileDataBegin(&job->request, offset);
        break;
      case 1:
        result = clipboard_fileDataChunk(&job->request, offset,
            data, size, end);
        break;
      default:
        result = clipboard_fileDataEnd(&job->request, offset);
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

static bool sendLocalBuffer(struct LocalJob * job,
    const uint8_t * data, size_t size)
{
  if (!sendLocalCall(job, 0, size, NULL, 0, false))
    return false;
  size_t offset = 0;
  while (offset < size)
  {
    size_t chunk = size - offset;
    if (chunk > KVMFR_CLIPBOARD_DATA_BYTES)
      chunk = KVMFR_CLIPBOARD_DATA_BYTES;
    const bool end = chunk == size - offset;
    if (!sendLocalCall(job, 1, offset, data + offset, chunk, end))
      return false;
    offset += chunk;
  }
  return sendLocalCall(job, 2, size, NULL, 0, true);
}

static void processLocalJob(struct LocalJob * job)
{
  LG_ClipboardFileError error   = LG_CLIPBOARD_FILE_ERROR_CANCELLED;
  void                * data    = NULL;
  size_t                size    = 0;
  bool                  success = false;
  if (!atomic_load_explicit(&job->cancelled, memory_order_acquire))
  {
    if (job->request.operation == LG_CLIPBOARD_FILE_LIST)
      success = windowsClipboardLocalFilesList(job->dataset->files,
          job->request.node, &data, &size, &error);
    else
      success = windowsClipboardLocalFilesRead(job->dataset->files,
          job->request.node, job->request.offset, job->request.length,
          &data, &size, &error);
  }

  if (success && !atomic_load_explicit(
        &job->cancelled, memory_order_acquire))
  {
    success = sendLocalBuffer(job, data, size);
    if (!success)
      error = LG_CLIPBOARD_FILE_ERROR_DISCONNECTED;
  }
  free(data);
  if (!success && !atomic_load_explicit(
        &job->cancelled, memory_order_acquire) && error)
    clipboard_fileCancel(job->request.dataset, job->request.request, error);
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
      job->next        = files.activeJobs;
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

    processLocalJob(job);
    LG_LOCK(files.lock);
    struct LocalJob ** link = &files.activeJobs;
    while (*link && *link != job)
      link = &(*link)->next;
    if (*link)
      *link = job->next;
    if (job->dataset->references)
      --job->dataset->references;
    struct LocalDataset * retired = takeUnusedLocalDatasetNL(job->dataset);
    LG_UNLOCK(files.lock);
    freeLocalDatasets(retired);
    lgFreeEvent(job->ready);
    free(job);
  }
}

static void cancelLocalState(bool stop)
{
  struct LocalDataset * retired;
  LG_LOCK(files.lock);
  if (stop)
    files.stopping = true;
  while (files.localAcquisitions)
  {
    struct LocalAcquisition * item = files.localAcquisitions;
    files.localAcquisitions        = item->next;
    if (item->dataset->references)
      --item->dataset->references;
    free(item);
  }
  for (struct LocalJob * job = files.jobsHead; job; job = job->next)
    atomic_store_explicit(&job->cancelled, true, memory_order_release);
  for (struct LocalJob * job = files.activeJobs; job; job = job->next)
  {
    atomic_store_explicit(&job->cancelled, true, memory_order_release);
    lgSignalEvent(job->ready);
  }
  retired = takeAllUnusedLocalDatasetsNL();
  LG_UNLOCK(files.lock);
  freeLocalDatasets(retired);
  if (files.jobEvent)
    lgSignalEvent(files.jobEvent);
}

bool clipboardFiles_init(void)
{
  if (files.lockInitialized)
    return false;
  memset(&files, 0, sizeof(files));
  LG_LOCK_INIT(files.lock);
  files.lockInitialized = true;

  uint64_t nonce = 0;
  if (!lgRandomBytes(&nonce, sizeof(nonce)))
  {
    DEBUG_ERROR("Failed to seed clipboard file identifiers");
    goto fail;
  }
  nonce &= ~KVMFR_CLIPBOARD_TRANSFER_HELPER;
  if (!nonce)
    nonce = 1;
  files.transferSerial = nonce;
  files.datasetSerial  = (nonce ^ UINT64_C(0x4c474346494c4553)) &
    ~KVMFR_CLIPBOARD_TRANSFER_HELPER;
  if (!files.datasetSerial)
    files.datasetSerial = 1;

  files.jobsTail     = &files.jobsHead;
  files.jobEvent     = lgCreateEvent(false, 0);
  files.provider     = providerCreate();
  if (!files.jobEvent || !files.provider)
    goto fail;
  for (unsigned int index = 0; index < LOCAL_WORKERS; ++index)
    if (!lgCreateThread("clipboard-files", localWorker, NULL,
          &files.workers[index]))
      goto fail;
  files.initialized = true;
  return true;

fail:
  cancelLocalState(true);
  for (unsigned int index = 0; index < LOCAL_WORKERS; ++index)
    if (files.workers[index])
      lgJoinThread(files.workers[index], NULL);
  if (files.jobEvent)
    lgFreeEvent(files.jobEvent);
  providerDisable(files.provider);
  providerWait(files.provider);
  providerRelease(files.provider);
  files.jobEvent        = NULL;
  files.provider        = NULL;
  files.lockInitialized = false;
  LG_LOCK_FREE(files.lock);
  return false;
}

void clipboardFiles_free(void)
{
  if (!files.initialized)
    return;

  cancelLocalState(true);
  LG_LOCK(files.lock);
  struct RemoteProviderContext * provider = files.provider;
  files.provider                          = NULL;
  LG_UNLOCK(files.lock);
  providerDisable(provider);

  LG_LOCK(files.lock);
  struct RemoteRequest * requests = disconnectRemoteRequestsNL();
  LG_UNLOCK(files.lock);

  while (requests)
  {
    struct RemoteRequest * next = requests->next;
    freeRemoteRequest(requests);
    requests = next;
  }

  providerWait(provider);

  for (unsigned int index = 0; index < LOCAL_WORKERS; ++index)
    if (files.workers[index])
      lgJoinThread(files.workers[index], NULL);

  LG_LOCK(files.lock);
  struct RemoteDataset * datasets = files.remoteDatasets;
  struct LocalDataset  * local    = files.localDatasets;
  files.remoteDatasets            = NULL;
  files.remoteCurrent             = NULL;
  files.localDatasets             = NULL;
  files.localCurrent              = NULL;
  files.initialized               = false;
  LG_UNLOCK(files.lock);

  while (datasets)
  {
    struct RemoteDataset * next = datasets->next;
    if (datasets->acquired)
      clipboard_fileRelease(datasets->dataset, datasets->acquisition);
    else if (datasets->acquiring)
      clipboard_fileCancel(datasets->dataset, datasets->acquisition,
          LG_CLIPBOARD_FILE_ERROR_CANCELLED);
    windowsClipboardRemoteManifestFree(datasets->manifestData);
    free(datasets);
    datasets = next;
  }
  freeLocalDatasets(local);

  lgFreeEvent(files.jobEvent);
  files.jobEvent        = NULL;
  files.lockInitialized = false;
  LG_LOCK_FREE(files.lock);
  providerRelease(provider);
}

bool clipboardFiles_setLocal(const char * mime,
    const void * data, size_t size)
{
  (void)mime;
  (void)data;
  (void)size;
  return false;
}

bool windowsClipboardFiles_setLocalDrop(HDROP drop)
{
  LG_ClipboardFileError        error    = LG_CLIPBOARD_FILE_ERROR_NONE;
  WindowsClipboardLocalFiles * captured =
    windowsClipboardLocalFilesCapture(drop, &error);
  if (!captured)
  {
    DEBUG_WARN("Failed to capture Windows clipboard files: error=%u",
        (unsigned int)error);
    return false;
  }

  struct LocalDataset * dataset = calloc(1, sizeof(*dataset));
  if (!dataset)
  {
    windowsClipboardLocalFilesRelease(captured);
    return false;
  }
  dataset->files   = captured;
  dataset->current = true;

  LG_LOCK(files.lock);
  if (!files.initialized || files.stopping)
  {
    LG_UNLOCK(files.lock);
    freeLocalDatasets(dataset);
    return false;
  }
  dataset->dataset    = nextDatasetNL();
  dataset->next       = files.localDatasets;
  files.localDatasets = dataset;
  if (files.localCurrent)
    files.localCurrent->current = false;
  struct LocalDataset * retired = takeUnusedLocalDatasetNL(
      files.localCurrent);
  files.localCurrent  = dataset;
  LG_UNLOCK(files.lock);
  freeLocalDatasets(retired);

  clipboard_notifyFiles(dataset->dataset);
  return true;
}

void clipboardFiles_clearLocal(void)
{
  LG_LOCK(files.lock);
  if (files.localCurrent)
    files.localCurrent->current = false;
  struct LocalDataset * retired = takeUnusedLocalDatasetNL(
      files.localCurrent);
  files.localCurrent = NULL;
  LG_UNLOCK(files.lock);
  freeLocalDatasets(retired);
}

bool clipboardFiles_getRemote(const char * mime,
    char ** data, size_t * size)
{
  (void)mime;
  if (data)
    *data = NULL;
  if (size)
    *size = 0;
  return false;
}

uint64_t clipboardFiles_remotePresentationAcquire(void)
{
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset      = files.remoteCurrent;
  const uint64_t         presentation = dataset && dataset->ready &&
      dataset->acquired && !dataset->stale ? dataset->acquisition : 0;
  if (presentation)
    ++dataset->presentationPins;
  LG_UNLOCK(files.lock);
  return presentation;
}

bool windowsClipboardFiles_createRemoteDataObject(uint64_t presentation,
    IDataObject ** object)
{
  if (!presentation || !object)
    return false;
  *object = NULL;

  LG_LOCK(files.lock);
  struct RemoteDataset * dataset =
    remoteDatasetByAcquisitionNL(presentation);
  const bool             valid   = dataset && dataset->presentationPins &&
    dataset->ready && dataset->acquired && !dataset->stale && files.provider;
  HRESULT                result  = E_FAIL;
  if (valid)
  {
    providerRetain(files.provider);
    result = windowsClipboardRemoteCreateDataObject(
        dataset->manifestData, dataset->dataset, dataset->acquisition,
        &remoteProviderOps, files.provider, object);
    if (FAILED(result))
      providerRelease(files.provider);
  }
  LG_UNLOCK(files.lock);
  if (FAILED(result))
  {
    DEBUG_WARN("Failed to create the Windows clipboard file object: "
        "0x%08lx", (unsigned long)result);
    return false;
  }
  return true;
}

bool clipboardFiles_getRemotePresentation(uint64_t presentation,
    const char * mime, char ** data, size_t * size)
{
  (void)presentation;
  (void)mime;
  if (data)
    *data = NULL;
  if (size)
    *size = 0;
  return false;
}

void clipboardFiles_remotePresentationDelivered(uint64_t presentation)
{
  (void)presentation;
}

void clipboardFiles_remotePresentationRelease(uint64_t presentation)
{
  struct RemoteAction action = { 0 };
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset = remoteDatasetByAcquisitionNL(presentation);
  if (dataset && dataset->presentationPins)
  {
    --dataset->presentationPins;
    action = retireRemoteDatasetNL(dataset);
  }
  LG_UNLOCK(files.lock);
  performRemoteAction(action);
}

bool clipboardFiles_remoteReady(uint64_t dataset)
{
  LG_LOCK(files.lock);
  struct RemoteDataset * found = remoteDatasetByIdNL(dataset);
  const bool             ready = found && found->current && found->ready &&
    !found->stale;
  LG_UNLOCK(files.lock);
  return ready;
}

bool clipboardFiles_remoteOffer(uint64_t datasetId)
{
  if (!datasetId)
    return false;

  struct RemoteDataset * candidate = calloc(1, sizeof(*candidate));
  if (!candidate)
    return false;
  candidate->manifestData = windowsClipboardRemoteManifestCreate();
  if (!candidate->manifestData)
  {
    free(candidate);
    return false;
  }

  struct RemoteRequest * cancelled      = NULL;
  struct RemoteAction    previousAction = { 0 };
  LG_LOCK(files.lock);
  if (!files.initialized || files.stopping)
  {
    LG_UNLOCK(files.lock);
    windowsClipboardRemoteManifestFree(candidate->manifestData);
    free(candidate);
    return false;
  }
  if (files.remoteCurrent && files.remoteCurrent->dataset == datasetId &&
      !files.remoteCurrent->stale)
  {
    LG_UNLOCK(files.lock);
    windowsClipboardRemoteManifestFree(candidate->manifestData);
    free(candidate);
    return true;
  }

  struct RemoteDataset * dataset = remoteDatasetByIdNL(datasetId);
  if (!dataset && remoteDatasetCountNL() + localAcquisitionCountNL() >=
      KVMFR_CLIPBOARD_FILE_MAX_ACQUISITIONS)
  {
    LG_UNLOCK(files.lock);
    windowsClipboardRemoteManifestFree(candidate->manifestData);
    free(candidate);
    return false;
  }
  if (!dataset)
  {
    dataset               = candidate;
    candidate             = NULL;
    dataset->dataset      = datasetId;
    dataset->acquisition  = nextTransferNL();
    dataset->acquiring    = true;
    dataset->next         = files.remoteDatasets;
    files.remoteDatasets  = dataset;
  }

  if (files.remoteCurrent)
  {
    struct RemoteDataset * previous = files.remoteCurrent;
    previous->current = false;
    if (previous->currentRequest)
    {
      cancelled                = takeRemoteRequestNL(previous->currentRequest);
      previous->currentRequest = 0;
      previous->manifest       = false;
    }
    previousAction = retireRemoteDatasetNL(previous);
  }
  dataset->current    = true;
  files.remoteCurrent = dataset;
  LG_UNLOCK(files.lock);

  if (candidate)
  {
    windowsClipboardRemoteManifestFree(candidate->manifestData);
    free(candidate);
  }
  cancelRemoteManifest(cancelled, LG_CLIPBOARD_FILE_ERROR_CANCELLED);
  performRemoteAction(previousAction);
  if (!dataset->acquiring)
  {
    if (dataset->ready)
      clipboard_fileRemoteReady(datasetId);
    return true;
  }
  if (clipboard_fileAcquire(datasetId, dataset->acquisition))
    return true;
  failRemoteDataset(dataset, LG_CLIPBOARD_FILE_ERROR_DISCONNECTED);
  return false;
}

void clipboardFiles_remoteClear(void)
{
  struct RemoteRequest * cancelled = NULL;
  struct RemoteAction    action    = { 0 };
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset = files.remoteCurrent;
  if (dataset)
  {
    dataset->current = false;
    if (dataset->currentRequest)
    {
      cancelled               = takeRemoteRequestNL(dataset->currentRequest);
      dataset->currentRequest = 0;
      dataset->manifest       = false;
    }
    files.remoteCurrent = NULL;
    action = retireRemoteDatasetNL(dataset);
  }
  LG_UNLOCK(files.lock);
  cancelRemoteManifest(cancelled, LG_CLIPBOARD_FILE_ERROR_CANCELLED);
  performRemoteAction(action);
}

void clipboardFiles_providerUnavailable(void)
{
  if (!files.initialized)
    return;

  cancelLocalState(false);

  LG_LOCK(files.lock);
  struct RemoteRequest * request = disconnectRemoteRequestsNL();
  struct RemoteDataset * dataset = files.remoteDatasets;
  files.remoteDatasets           = NULL;
  files.remoteCurrent            = NULL;
  LG_UNLOCK(files.lock);

  while (request)
  {
    struct RemoteRequest * next = request->next;
    freeRemoteRequest(request);
    request = next;
  }
  while (dataset)
  {
    struct RemoteDataset * next = dataset->next;
    windowsClipboardRemoteManifestFree(dataset->manifestData);
    free(dataset);
    dataset = next;
  }
}

void clipboardFiles_remoteAcquired(uint64_t datasetId,
    uint64_t acquisition, LG_ClipboardFileError error)
{
  LG_LOCK(files.lock);
  struct RemoteDataset * dataset = remoteDatasetByIdNL(datasetId);
  const bool             valid   = dataset &&
    dataset->acquisition == acquisition &&
    dataset->acquiring && !dataset->stale;
  if (valid)
  {
    dataset->acquiring = false;
    dataset->acquired  = !error;
    dataset->manifest  = !error;
  }
  LG_UNLOCK(files.lock);
  if (!valid)
    return;
  if (error)
  {
    failRemoteDataset(dataset, error);
    return;
  }
  startRemoteList(dataset, KVMFR_CLIPBOARD_FILE_ROOT_NODE);
}

LG_ClipboardResult clipboardFiles_remoteDataBegin(
    const LG_ClipboardFileRequest * descriptor, uint64_t sizeHint)
{
  if (!descriptor)
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(files.lock);
  struct RemoteRequest * request       =
    remoteRequestByIdNL(descriptor->request);
  struct RemoteDataset * failedDataset = request && request->manifest ?
    request->dataset : NULL;
  const bool             valid         = request && !request->began &&
    !request->complete &&
    sameRemoteRequest(request, descriptor) &&
    (descriptor->operation != LG_CLIPBOARD_FILE_READ ||
     sizeHint == descriptor->length) &&
    (descriptor->operation != LG_CLIPBOARD_FILE_LIST ||
     sizeHint == LG_CLIPBOARD_SIZE_UNKNOWN || sizeHint <= MAX_LIST_BYTES);
  if (valid)
  {
    request->began    = true;
    request->sizeHint = sizeHint;
  }
  else if (request)
  {
    takeRemoteRequestNL(descriptor->request);
    if (!request->manifest)
    {
      request->error    = LG_CLIPBOARD_FILE_ERROR_INVALID;
      request->complete = true;
      lgSignalEvent(request->completeEvent);
    }
  }
  LG_UNLOCK(files.lock);

  if (valid)
    return LG_CLIPBOARD_RESULT_ACCEPTED;
  if (failedDataset)
  {
    cancelRemoteManifest(request, LG_CLIPBOARD_FILE_ERROR_INVALID);
    failRemoteDataset(failedDataset, LG_CLIPBOARD_FILE_ERROR_INVALID);
  }
  return LG_CLIPBOARD_RESULT_FAILED;
}

LG_ClipboardResult clipboardFiles_remoteDataChunk(
    const LG_ClipboardFileRequest * descriptor, uint64_t responseOffset,
    const void * data, size_t size)
{
  if (!descriptor)
    return LG_CLIPBOARD_RESULT_FAILED;

  const size_t responseStart = (size_t)responseOffset;
  const bool   envelopeValid = data && size &&
    (uint64_t)responseStart == responseOffset &&
    size <= SIZE_MAX - responseStart;
  LG_LOCK(files.lock);
  struct RemoteRequest * request       =
    remoteRequestByIdNL(descriptor->request);
  struct RemoteDataset * failedDataset = request && request->manifest ?
    request->dataset : NULL;
  const size_t           end           = envelopeValid ?
    responseStart + size : 0;
  bool                   valid         = envelopeValid && request &&
    request->began && !request->complete &&
    sameRemoteRequest(request, descriptor) &&
    responseOffset == request->dataSize &&
    (request->sizeHint == LG_CLIPBOARD_SIZE_UNKNOWN ||
     end <= request->sizeHint);
  if (valid && descriptor->operation == LG_CLIPBOARD_FILE_READ)
  {
    valid = end <= descriptor->length && end <= request->outputCapacity &&
      request->output;
    if (valid)
      memcpy(request->output + request->dataSize, data, size);
  }
  else if (valid)
  {
    valid = end <= MAX_LIST_BYTES && growBuffer(&request->data,
        &request->dataCapacity, end);
    if (valid)
      memcpy(request->data + request->dataSize, data, size);
  }
  if (valid)
    request->dataSize = end;
  else if (request)
  {
    takeRemoteRequestNL(descriptor->request);
    if (!request->manifest)
    {
      request->error    = LG_CLIPBOARD_FILE_ERROR_INVALID;
      request->complete = true;
      lgSignalEvent(request->completeEvent);
    }
  }
  LG_UNLOCK(files.lock);

  if (valid)
    return LG_CLIPBOARD_RESULT_ACCEPTED;
  if (failedDataset)
  {
    cancelRemoteManifest(request, LG_CLIPBOARD_FILE_ERROR_INVALID);
    failRemoteDataset(failedDataset, LG_CLIPBOARD_FILE_ERROR_INVALID);
  }
  return LG_CLIPBOARD_RESULT_FAILED;
}

LG_ClipboardResult clipboardFiles_remoteDataEnd(
    const LG_ClipboardFileRequest * descriptor, uint64_t finalSize)
{
  if (!descriptor)
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(files.lock);
  struct RemoteRequest * request  = takeRemoteRequestNL(descriptor->request);
  const bool             manifest = request && request->manifest;
  struct RemoteDataset * dataset  = manifest ? request->dataset : NULL;
  const bool             valid    = request && request->began &&
    !request->complete &&
    sameRemoteRequest(request, descriptor) &&
    request->dataSize == finalSize &&
    (request->sizeHint == LG_CLIPBOARD_SIZE_UNKNOWN ||
     request->sizeHint == finalSize) &&
    (descriptor->operation != LG_CLIPBOARD_FILE_READ ||
     finalSize == descriptor->length);
  if (request)
  {
    request->error    = valid ? LG_CLIPBOARD_FILE_ERROR_NONE :
      LG_CLIPBOARD_FILE_ERROR_INVALID;
    request->complete = true;
    if (!request->manifest)
      lgSignalEvent(request->completeEvent);
  }
  LG_UNLOCK(files.lock);

  if (!request)
    return LG_CLIPBOARD_RESULT_FAILED;
  if (manifest)
  {
    if (valid)
      continueRemoteManifest(request);
    else
    {
      freeRemoteRequest(request);
      failRemoteDataset(dataset, LG_CLIPBOARD_FILE_ERROR_INVALID);
    }
  }
  return valid ? LG_CLIPBOARD_RESULT_ACCEPTED :
    LG_CLIPBOARD_RESULT_FAILED;
}

void clipboardFiles_remoteCancel(uint64_t datasetId,
    uint64_t requestId, LG_ClipboardFileError reason)
{
  LG_LOCK(files.lock);
  struct RemoteRequest * request       = takeRemoteRequestNL(requestId);
  const bool             manifest      = request && request->manifest;
  struct RemoteDataset * failedDataset = manifest ? request->dataset : NULL;
  struct RemoteDataset * acquisition   = remoteDatasetByIdNL(datasetId);
  if (!acquisition || acquisition->acquisition != requestId ||
      !acquisition->acquiring)
    acquisition = NULL;
  else
    acquisition->acquiring = false;
  if (request)
  {
    request->error    = reason;
    request->complete = true;
    if (!request->manifest)
      lgSignalEvent(request->completeEvent);
  }
  LG_UNLOCK(files.lock);

  if (manifest)
  {
    freeRemoteRequest(request);
    failRemoteDataset(failedDataset, reason);
  }
  else if (acquisition)
    failRemoteDataset(acquisition, reason);
}

void clipboardFiles_localAcquire(uint64_t wireDataset,
    uint64_t acquisition)
{
  LG_ClipboardFileError      error    = LG_CLIPBOARD_FILE_ERROR_STALE;
  struct LocalAcquisition  * item     = calloc(1, sizeof(*item));
  LG_LOCK(files.lock);
  struct LocalDataset      * dataset  = localDatasetByIdNL(wireDataset);
  struct LocalAcquisition  * existing = files.localAcquisitions;
  while (existing && existing->acquisition != acquisition)
    existing = existing->next;
  if (!acquisition || files.stopping)
    error = LG_CLIPBOARD_FILE_ERROR_INVALID;
  else if (existing)
    error = existing->wireDataset == wireDataset ?
      LG_CLIPBOARD_FILE_ERROR_NONE : LG_CLIPBOARD_FILE_ERROR_INVALID;
  else if (!item)
    error = LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
  else if (dataset && localAcquisitionCountNL() + remoteDatasetCountNL() <
      KVMFR_CLIPBOARD_FILE_MAX_ACQUISITIONS)
  {
    item->dataset             = dataset;
    item->wireDataset         = wireDataset;
    item->acquisition         = acquisition;
    item->next                = files.localAcquisitions;
    files.localAcquisitions   = item;
    ++dataset->references;
    item  = NULL;
    error = LG_CLIPBOARD_FILE_ERROR_NONE;
  }
  else if (dataset)
    error = LG_CLIPBOARD_FILE_ERROR_NO_SPACE;
  LG_UNLOCK(files.lock);
  free(item);
  clipboard_fileAcquired(wireDataset, acquisition, error);
}

void clipboardFiles_localRelease(uint64_t wireDataset,
    uint64_t acquisition)
{
  struct LocalAcquisition  * item    = NULL;
  struct LocalDataset      * retired = NULL;
  LG_LOCK(files.lock);
  struct LocalAcquisition ** link    = &files.localAcquisitions;
  while (*link && ((*link)->wireDataset != wireDataset ||
      (*link)->acquisition != acquisition))
    link = &(*link)->next;
  if (*link)
  {
    item  = *link;
    *link = item->next;
    if (item->dataset->references)
      --item->dataset->references;
    retired = takeUnusedLocalDatasetNL(item->dataset);
  }
  LG_UNLOCK(files.lock);
  free(item);
  freeLocalDatasets(retired);
}

void clipboardFiles_localRequest(
    const LG_ClipboardFileRequest * request)
{
  if (!request || !request->dataset || !request->request ||
      (request->operation != LG_CLIPBOARD_FILE_LIST &&
       request->operation != LG_CLIPBOARD_FILE_READ) ||
      (request->operation == LG_CLIPBOARD_FILE_LIST &&
       (request->offset || request->length)) ||
      (request->operation == LG_CLIPBOARD_FILE_READ &&
       (!request->length ||
        request->length > KVMFR_CLIPBOARD_FILE_READ_BYTES ||
        request->offset > UINT64_MAX - request->length)))
  {
    if (request)
      clipboard_fileCancel(request->dataset, request->request,
          LG_CLIPBOARD_FILE_ERROR_INVALID);
    return;
  }

  struct LocalJob * job = calloc(1, sizeof(*job));
  if (!job)
  {
    clipboard_fileCancel(request->dataset, request->request,
        LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    return;
  }
  job->ready = lgCreateEvent(true, 0);
  if (!job->ready)
  {
    free(job);
    clipboard_fileCancel(request->dataset, request->request,
        LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    return;
  }
  atomic_init(&job->cancelled, false);
  job->request = *request;

  LG_ClipboardFileError error = LG_CLIPBOARD_FILE_ERROR_STALE;
  LG_LOCK(files.lock);
  job->dataset = acquiredLocalDatasetNL(request->dataset);
  if (localAcquisitionIdExistsNL(request->request) ||
      localJobIdExistsNL(request->request))
    error = LG_CLIPBOARD_FILE_ERROR_INVALID;
  else if (job->dataset && !files.stopping &&
      localJobCountNL() + remoteRequestCountNL() <
        KVMFR_CLIPBOARD_FILE_MAX_REQUESTS)
  {
    ++job->dataset->references;
    *files.jobsTail = job;
    files.jobsTail  = &job->next;
    error           = LG_CLIPBOARD_FILE_ERROR_NONE;
  }
  else if (job->dataset)
    error = LG_CLIPBOARD_FILE_ERROR_NO_SPACE;
  LG_UNLOCK(files.lock);

  if (error)
  {
    lgFreeEvent(job->ready);
    free(job);
    clipboard_fileCancel(request->dataset, request->request, error);
    return;
  }
  lgSignalEvent(files.jobEvent);
}

void clipboardFiles_localCancel(uint64_t dataset,
    uint64_t request, LG_ClipboardFileError reason)
{
  (void)reason;
  struct LocalAcquisition * acquisition = NULL;
  struct LocalDataset     * retired     = NULL;
  LG_LOCK(files.lock);
  struct LocalAcquisition ** link = &files.localAcquisitions;
  while (*link && ((*link)->wireDataset != dataset ||
      (*link)->acquisition != request))
    link = &(*link)->next;
  if (*link)
  {
    acquisition = *link;
    *link       = acquisition->next;
    if (acquisition->dataset->references)
      --acquisition->dataset->references;
    retired = takeUnusedLocalDatasetNL(acquisition->dataset);
  }
  for (struct LocalJob * job = files.jobsHead; job; job = job->next)
    if (job->request.dataset == dataset && job->request.request == request)
      atomic_store_explicit(&job->cancelled, true, memory_order_release);
  for (struct LocalJob * job = files.activeJobs; job; job = job->next)
    if (job->request.dataset == dataset && job->request.request == request)
    {
      atomic_store_explicit(&job->cancelled, true, memory_order_release);
      lgSignalEvent(job->ready);
    }
  LG_UNLOCK(files.lock);
  free(acquisition);
  freeLocalDatasets(retired);
}

void clipboardFiles_localReady(uint64_t request)
{
  LG_LOCK(files.lock);
  for (struct LocalJob * job = files.activeJobs; job; job = job->next)
    if (job->request.request == request)
      lgSignalEvent(job->ready);
  LG_UNLOCK(files.lock);
}
