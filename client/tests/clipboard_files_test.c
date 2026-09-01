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

#include "core/clipboard.h"
#include "core/clipboard_files.h"
#include "test.h"

#include "common/debug.h"
#include <LGProtocol/KVMFRClipboard.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned notices;
static uint64_t datasets[16];
static unsigned acquireCount;
static uint64_t acquiredDataset;
static uint64_t acquiredRequest;
static unsigned releaseCount;
static uint64_t releasedDataset;
static uint64_t releasedAcquisition;
static unsigned requestCount;
static LG_ClipboardFileRequest remoteRequest;
static unsigned remoteReadyCount;
static unsigned remoteFailedCount;
static uint64_t remoteFailedDataset;
static bool remoteRequestSucceeds = true;
static uint8_t * responseData;
static size_t responseSize;
static size_t responseCapacity;
static bool responseOpen;
static bool responseComplete;
static bool responseWireEnded;
static unsigned cancelCount;
static uint64_t cancelledDataset;
static uint64_t cancelledRequest;
static LG_ClipboardFileError cancelledReason;

void clipboard_notifyFiles(uint64_t dataset)
{
  CHECK(notices < sizeof(datasets) / sizeof(datasets[0]));
  datasets[notices++] = dataset;
}

bool clipboard_fileAcquire(uint64_t dataset, uint64_t acquisition)
{
  ++acquireCount;
  acquiredDataset = dataset;
  acquiredRequest = acquisition;
  return true;
}

bool clipboard_fileAcquired(uint64_t dataset, uint64_t acquisition,
    LG_ClipboardFileError error)
{
  (void)dataset;
  (void)acquisition;
  (void)error;
  return false;
}

bool clipboard_fileRelease(uint64_t dataset, uint64_t acquisition)
{
  ++releaseCount;
  releasedDataset = dataset;
  releasedAcquisition = acquisition;
  return true;
}

bool clipboard_fileRequest(const LG_ClipboardFileRequest * request)
{
  CHECK(request);
  ++requestCount;
  remoteRequest = *request;
  return remoteRequestSucceeds;
}

LG_ClipboardResult clipboard_fileDataBegin(
    const LG_ClipboardFileRequest * request, uint64_t sizeHint)
{
  CHECK(request);
  (void)sizeHint;
  responseSize      = 0;
  responseOpen      = true;
  responseComplete  = false;
  responseWireEnded = false;
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

LG_ClipboardResult clipboard_fileDataChunk(
    const LG_ClipboardFileRequest * request, uint64_t responseOffset,
    const void * data, size_t size, bool end)
{
  CHECK(request);
  CHECK(responseOpen);
  CHECK(responseOffset == responseSize);
  CHECK(data || !size);
  CHECK(size <= SIZE_MAX - responseSize);
  const size_t wanted = responseSize + size;
  if (wanted > responseCapacity)
  {
    uint8_t * resized = realloc(responseData, wanted);
    CHECK(resized);
    responseData     = resized;
    responseCapacity = wanted;
  }
  if (size)
    memcpy(responseData + responseSize, data, size);
  responseSize = wanted;
  if (end)
    responseWireEnded = true;
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

LG_ClipboardResult clipboard_fileDataEnd(
    const LG_ClipboardFileRequest * request, uint64_t finalSize)
{
  CHECK(request);
  CHECK(responseOpen);
  CHECK(finalSize == responseSize);
  CHECK(!finalSize || responseWireEnded);
  responseOpen     = false;
  responseComplete = true;
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

bool clipboard_fileCancel(uint64_t dataset, uint64_t request,
    LG_ClipboardFileError reason)
{
  ++cancelCount;
  cancelledDataset = dataset;
  cancelledRequest = request;
  cancelledReason = reason;
  return true;
}

void clipboard_fileRemoteReady(uint64_t dataset)
{
  CHECK(dataset == acquiredDataset);
  ++remoteReadyCount;
}

void clipboard_fileRemoteFailed(uint64_t dataset)
{
  CHECK(dataset);
  ++remoteFailedCount;
  remoteFailedDataset = dataset;
}

static bool runLocalRequest(uint64_t dataset, uint64_t node,
    LG_ClipboardFileOperation operation, uint64_t offset, uint32_t length)
{
  static uint64_t requestId = 1;
  responseSize       = 0;
  responseOpen       = false;
  responseComplete   = false;
  responseWireEnded  = false;
  cancelCount        = 0;
  cancelledDataset   = 0;
  cancelledRequest   = 0;
  cancelledReason    = LG_CLIPBOARD_FILE_ERROR_NONE;
  const LG_ClipboardFileRequest request =
  {
    .dataset   = dataset,
    .request   = requestId++,
    .node      = node,
    .offset    = offset,
    .length    = length,
    .operation = operation,
  };
  const bool result = clipboardFiles_testLocalRequest(&request);
  CHECK(!result || responseComplete);
  CHECK(!result || !responseSize || responseWireEnded);
  CHECK(result ? cancelCount == 0U : cancelCount == 1U);
  if (!result)
  {
    CHECK(cancelledDataset == dataset);
    CHECK(cancelledRequest == request.request);
    CHECK(cancelledReason != LG_CLIPBOARD_FILE_ERROR_NONE);
  }
  return result;
}

static bool responseEntry(const char * name, KVMFRClipboardFileEntry * result)
{
  size_t offset = 0;
  while (offset < responseSize)
  {
    CHECK(responseSize - offset >= sizeof(*result));
    KVMFRClipboardFileEntry entry;
    memcpy(&entry, responseData + offset, sizeof(entry));
    const uint64_t bytes = KVMFR_CLIPBOARD_FILE_ENTRY_BYTES(
        entry.nameLength);
    CHECK(bytes <= SIZE_MAX);
    CHECK((size_t)bytes <= responseSize - offset);
    const char * entryName = (const char *)responseData + offset +
      sizeof(entry);
    if (strlen(name) == entry.nameLength &&
        !memcmp(entryName, name, entry.nameLength))
    {
      *result = entry;
      return true;
    }
    offset += (size_t)bytes;
  }
  CHECK(offset == responseSize);
  return false;
}

static void createFile(const char * path, const char * data)
{
  const int fd = open(path,
      O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
  CHECK(fd >= 0);
  const size_t length = strlen(data);
  CHECK(write(fd, data, length) == (ssize_t)length);
  CHECK(close(fd) == 0);
}

static void testLocalLifecycle(void)
{
  char directory[] = "/tmp/lg-clipboard-files-XXXXXX";
  CHECK(mkdtemp(directory));
  char path[256];
  CHECK(snprintf(path, sizeof(path), "%s/item.txt", directory) > 0);
  const int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
  CHECK(fd >= 0);
  CHECK(write(fd, "clipboard", 9) == 9);
  CHECK(close(fd) == 0);

  CHECK(clipboardFiles_testInit(UINT64_C(0x1234000012340000)));
  char uri[320];
  const int uriLength = snprintf(uri, sizeof(uri), "file://%s\r\n", path);
  CHECK(uriLength > 0 && (size_t)uriLength < sizeof(uri));
  CHECK(clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)uriLength));
  CHECK(notices == 1);
  CHECK(datasets[0] != 0);
  CHECK(kvmfrClipboardTransferFromClient(datasets[0]));

  clipboardFiles_clearLocal();
  char gnome[336];
  const int gnomeLength = snprintf(
      gnome, sizeof(gnome), "copy\nfile://%s\n", path);
  CHECK(gnomeLength > 0 && (size_t)gnomeLength < sizeof(gnome));
  CHECK(clipboardFiles_setLocal("x-special/gnome-copied-files",
        gnome, (size_t)gnomeLength));
  CHECK(notices == 2);
  CHECK(datasets[1] != datasets[0]);

  clipboardFiles_clearLocal();
  CHECK(clipboardFiles_setLocal("x-special/mate-copied-files",
        gnome, (size_t)gnomeLength));
  CHECK(notices == 3);
  CHECK(datasets[2] != datasets[0]);
  CHECK(datasets[2] != datasets[1]);

  static const char invalid[] = "file:///does/not/exist\n";
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", invalid, sizeof(invalid) - 1U));
  clipboardFiles_clearLocal();
  clipboardFiles_free();
  clipboardFiles_free();

  CHECK(clipboardFiles_testInit(UINT64_C(0x5678000056780000)));
  CHECK(clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)uriLength));
  CHECK(notices == 4);
  CHECK(datasets[3] != datasets[0]);
  CHECK(datasets[3] != datasets[1]);
  CHECK(datasets[3] != datasets[2]);
  clipboardFiles_free();

  CHECK(unlink(path) == 0);
  CHECK(rmdir(directory) == 0);
}

static void testLocalUriValidation(void)
{
  char directory[] = "/tmp/lg-clipboard-uri-XXXXXX";
  CHECK(mkdtemp(directory));

  char plain[256];
  char spaced[256];
  char link[256];
  char alias[256];
  CHECK(snprintf(plain, sizeof(plain), "%s/item.txt", directory) > 0);
  CHECK(snprintf(spaced, sizeof(spaced),
        "%s/item space.txt", directory) > 0);
  CHECK(snprintf(link, sizeof(link), "%s/link.txt", directory) > 0);
  CHECK(snprintf(alias, sizeof(alias), "%s/alias", directory) > 0);
  createFile(plain, "plain");
  createFile(spaced, "spaced");
  CHECK(symlink("item.txt", link) == 0);
  CHECK(symlink(".", alias) == 0);

  CHECK(clipboardFiles_testInit(UINT64_C(0x684b2452684b2452)));
  const unsigned before = notices;
  char uri[512];
  int length = snprintf(uri, sizeof(uri),
      "FILE://LOCALHOST%s/item%%20space.txt\r\n", directory);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  clipboardFiles_clearLocal();

  length = snprintf(uri, sizeof(uri), "file://%s%%\n", plain);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  length = snprintf(uri, sizeof(uri), "file://%s%%0\n", plain);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  length = snprintf(uri, sizeof(uri), "file://%s%%GG\n", plain);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  length = snprintf(uri, sizeof(uri), "file://%s%%00suffix\n", plain);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  length = snprintf(uri, sizeof(uri), "file://%s?query\n", plain);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  length = snprintf(uri, sizeof(uri), "file://%s#fragment\n", plain);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  length = snprintf(uri, sizeof(uri), "file://example.invalid%s\n", plain);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  static const char root[] = "file:///\n";
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", root, sizeof(root) - 1U));
  length = snprintf(uri, sizeof(uri), "file://%s/\n", directory);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  clipboardFiles_clearLocal();
  length = snprintf(uri, sizeof(uri), "file://%s/./item.txt\n", directory);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  length = snprintf(uri, sizeof(uri), "file://%s//item.txt\n", directory);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  length = snprintf(uri, sizeof(uri), "file://%s\n", link);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  length = snprintf(uri, sizeof(uri),
      "file://%s/alias/item.txt\n", directory);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  length = snprintf(uri, sizeof(uri), "file://%s\n", spaced);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)length));
  length = snprintf(uri, sizeof(uri),
      "cutx\nfile://%s\n", plain);
  CHECK(length > 0 && (size_t)length < sizeof(uri));
  CHECK(!clipboardFiles_setLocal(
        "x-special/gnome-copied-files", uri, (size_t)length));
  CHECK(!clipboardFiles_setLocal(
        "x-special/mate-copied-files", uri, (size_t)length));
  CHECK(notices == before + 2U);

  clipboardFiles_free();
  CHECK(unlink(alias) == 0);
  CHECK(unlink(link) == 0);
  CHECK(unlink(spaced) == 0);
  CHECK(unlink(plain) == 0);
  CHECK(rmdir(directory) == 0);
}

static void testLocalSnapshot(void)
{
  char directory[] = "/tmp/lg-clipboard-snapshot-XXXXXX";
  CHECK(mkdtemp(directory));
  char stable[256];
  char changed[256];
  CHECK(snprintf(stable, sizeof(stable),
        "%s/stable.txt", directory) > 0);
  CHECK(snprintf(changed, sizeof(changed),
        "%s/changed.txt", directory) > 0);
  createFile(stable, "stable");
  createFile(changed, "before");

  CHECK(clipboardFiles_testInit(UINT64_C(0x1a72021b1a72021b)));
  char uri[320];
  const int uriLength = snprintf(
      uri, sizeof(uri), "file://%s\r\n", directory);
  CHECK(uriLength > 0 && (size_t)uriLength < sizeof(uri));
  CHECK(clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)uriLength));
  const uint64_t dataset = datasets[notices - 1U];

  CHECK(runLocalRequest(dataset, KVMFR_CLIPBOARD_FILE_ROOT_NODE,
        LG_CLIPBOARD_FILE_LIST, 0, 0));
  const char * base = strrchr(directory, '/');
  CHECK(base && base[1]);
  KVMFRClipboardFileEntry rootEntry;
  CHECK(responseEntry(base + 1, &rootEntry));
  CHECK(rootEntry.type == KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY);

  CHECK(runLocalRequest(dataset, rootEntry.node,
        LG_CLIPBOARD_FILE_LIST, 0, 0));
  KVMFRClipboardFileEntry stableEntry;
  KVMFRClipboardFileEntry changedEntry;
  CHECK(responseEntry("stable.txt", &stableEntry));
  CHECK(responseEntry("changed.txt", &changedEntry));

  char moved[320];
  CHECK(snprintf(moved, sizeof(moved), "%s-moved", directory) > 0);
  CHECK(rename(directory, moved) == 0);
  CHECK(mkdir(directory, 0700) == 0);
  char replacement[384];
  CHECK(snprintf(replacement, sizeof(replacement),
        "%s/stable.txt", directory) > 0);
  createFile(replacement, "replacement");

  CHECK(runLocalRequest(dataset, stableEntry.node,
        LG_CLIPBOARD_FILE_READ, 0, (uint32_t)stableEntry.size));
  CHECK(responseSize == strlen("stable"));
  CHECK(!memcmp(responseData, "stable", responseSize));
  clipboardFiles_testForceLocalEof();
  CHECK(!runLocalRequest(dataset, stableEntry.node,
        LG_CLIPBOARD_FILE_READ, 0, (uint32_t)stableEntry.size));
  CHECK(cancelledReason == LG_CLIPBOARD_FILE_ERROR_IO);

  char movedChanged[384];
  CHECK(snprintf(movedChanged, sizeof(movedChanged),
        "%s/changed.txt", moved) > 0);
  const int changedFd = open(movedChanged,
      O_WRONLY | O_APPEND | O_CLOEXEC);
  CHECK(changedFd >= 0);
  CHECK(write(changedFd, "-modified", 9) == 9);
  CHECK(close(changedFd) == 0);
  CHECK(!runLocalRequest(dataset, changedEntry.node,
        LG_CLIPBOARD_FILE_READ, 0, 64));
  CHECK(cancelledReason == LG_CLIPBOARD_FILE_ERROR_INVALID);
  CHECK(!runLocalRequest(dataset, changedEntry.node,
        LG_CLIPBOARD_FILE_READ, 0, (uint32_t)changedEntry.size));
  CHECK(cancelledReason == LG_CLIPBOARD_FILE_ERROR_STALE);

  char movedStable[384];
  char oldStable[384];
  CHECK(snprintf(movedStable, sizeof(movedStable),
        "%s/stable.txt", moved) > 0);
  CHECK(snprintf(oldStable, sizeof(oldStable),
        "%s/stable.old", moved) > 0);
  CHECK(rename(movedStable, oldStable) == 0);
  createFile(movedStable, "stable");
  CHECK(!runLocalRequest(dataset, stableEntry.node,
        LG_CLIPBOARD_FILE_READ, 0, 64));
  CHECK(cancelledReason == LG_CLIPBOARD_FILE_ERROR_INVALID);
  CHECK(!runLocalRequest(dataset, stableEntry.node,
        LG_CLIPBOARD_FILE_READ, 0, (uint32_t)stableEntry.size));
  CHECK(cancelledReason == LG_CLIPBOARD_FILE_ERROR_STALE);

  clipboardFiles_free();
  CHECK(unlink(replacement) == 0);
  CHECK(rmdir(directory) == 0);
  CHECK(unlink(movedStable) == 0);
  CHECK(unlink(oldStable) == 0);
  CHECK(unlink(movedChanged) == 0);
  CHECK(rmdir(moved) == 0);
}

static void testLocalUnsupportedEntry(void)
{
  char directory[] = "/tmp/lg-clipboard-unsupported-XXXXXX";
  CHECK(mkdtemp(directory));
  char target[256];
  char link[256];
  char fifo[256];
  CHECK(snprintf(target, sizeof(target),
        "%s/target.txt", directory) > 0);
  CHECK(snprintf(link, sizeof(link), "%s/link.txt", directory) > 0);
  CHECK(snprintf(fifo, sizeof(fifo), "%s/special", directory) > 0);
  createFile(target, "target");
  CHECK(symlink("target.txt", link) == 0);
  CHECK(mkfifo(fifo, 0600) == 0);

  CHECK(clipboardFiles_testInit(UINT64_C(0x62d1147362d11473)));
  char uri[320];
  const int uriLength = snprintf(
      uri, sizeof(uri), "file://%s\r\n", directory);
  CHECK(uriLength > 0 && (size_t)uriLength < sizeof(uri));
  CHECK(clipboardFiles_setLocal(
        "text/uri-list", uri, (size_t)uriLength));
  const uint64_t dataset = datasets[notices - 1U];
  CHECK(runLocalRequest(dataset, KVMFR_CLIPBOARD_FILE_ROOT_NODE,
        LG_CLIPBOARD_FILE_LIST, 0, 0));
  const char * base = strrchr(directory, '/');
  CHECK(base && base[1]);
  KVMFRClipboardFileEntry rootEntry;
  CHECK(responseEntry(base + 1, &rootEntry));
  CHECK(!runLocalRequest(dataset, rootEntry.node,
        LG_CLIPBOARD_FILE_LIST, 0, 0));
  CHECK(cancelledReason == LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED);

  clipboardFiles_free();
  CHECK(unlink(fifo) == 0);
  CHECK(unlink(link) == 0);
  CHECK(unlink(target) == 0);
  CHECK(rmdir(directory) == 0);
}

static uint64_t completeRemoteOffer(uint64_t dataset, const char * name)
{
  const unsigned previousAcquires = acquireCount;
  const unsigned previousRequests = requestCount;
  const unsigned previousReady = remoteReadyCount;
  CHECK(clipboardFiles_remoteOffer(dataset));
  CHECK(acquireCount == previousAcquires + 1U);
  CHECK(acquiredDataset == dataset);
  const uint64_t acquisition = acquiredRequest;

  clipboardFiles_remoteAcquired(dataset, acquisition,
      LG_CLIPBOARD_FILE_ERROR_NONE);
  CHECK(requestCount == previousRequests + 1U);
  CHECK(remoteRequest.dataset == dataset);
  CHECK(remoteRequest.node == KVMFR_CLIPBOARD_FILE_ROOT_NODE);
  CHECK(remoteRequest.operation == LG_CLIPBOARD_FILE_LIST);

  const size_t nameLength = strlen(name);
  const uint64_t wireSize = KVMFR_CLIPBOARD_FILE_ENTRY_BYTES(nameLength);
  CHECK(wireSize <= SIZE_MAX);
  uint8_t * wire = calloc(1, (size_t)wireSize);
  CHECK(wire);
  const KVMFRClipboardFileEntry entry =
  {
    .node       = 2,
    .size       = 9,
    .createdNs  = 1,
    .modifiedNs = 2,
    .type       = KVMFR_CLIPBOARD_FILE_TYPE_REGULAR,
    .nameLength = (uint16_t)nameLength,
  };
  memcpy(wire, &entry, sizeof(entry));
  memcpy(wire + sizeof(entry), name, nameLength);
  CHECK(clipboardFiles_remoteDataBegin(
        &remoteRequest, wireSize) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(clipboardFiles_remoteDataChunk(&remoteRequest, 0,
        wire, (size_t)wireSize) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(clipboardFiles_remoteDataEnd(
        &remoteRequest, wireSize) == LG_CLIPBOARD_RESULT_ACCEPTED);
  free(wire);
  CHECK(remoteReadyCount == previousReady + 1U);
  CHECK(clipboardFiles_remoteReady(dataset));
  return acquisition;
}

static uint64_t beginRemoteOffer(uint64_t dataset)
{
  const unsigned previousAcquires = acquireCount;
  CHECK(clipboardFiles_remoteOffer(dataset));
  CHECK(acquireCount == previousAcquires + 1U);
  CHECK(acquiredDataset == dataset);
  return acquiredRequest;
}

static LG_ClipboardFileRequest beginRemoteList(uint64_t dataset,
    uint64_t * acquisition)
{
  const unsigned previousRequests = requestCount;
  *acquisition = beginRemoteOffer(dataset);
  clipboardFiles_remoteAcquired(dataset, *acquisition,
      LG_CLIPBOARD_FILE_ERROR_NONE);
  CHECK(requestCount == previousRequests + 1U);
  CHECK(remoteRequest.dataset == dataset);
  CHECK(remoteRequest.operation == LG_CLIPBOARD_FILE_LIST);
  return remoteRequest;
}

static void checkRemoteFailure(uint64_t dataset,
    unsigned previousFailures, unsigned previousReleases,
    bool acquired)
{
  CHECK(remoteFailedCount == previousFailures + 1U);
  CHECK(remoteFailedDataset == dataset);
  CHECK(releaseCount == previousReleases + (acquired ? 1U : 0U));
  if (acquired)
    CHECK(releasedDataset == dataset);
  CHECK(clipboardFiles_testRemoteDatasetCount() == 0U);
}

static void testRemoteFailures(void)
{
  CHECK(clipboardFiles_testInit(UINT64_C(0x7315000073150000)));
  acquireCount = 0;
  releaseCount = 0;
  requestCount = 0;
  remoteFailedCount = 0;
  remoteFailedDataset = 0;
  remoteRequestSucceeds = true;

  uint64_t dataset = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x73150001);
  uint64_t acquisition = beginRemoteOffer(dataset);
  unsigned previousFailures = remoteFailedCount;
  unsigned previousReleases = releaseCount;
  clipboardFiles_remoteAcquired(dataset, acquisition,
      LG_CLIPBOARD_FILE_ERROR_ACCESS);
  checkRemoteFailure(dataset, previousFailures, previousReleases, false);

  dataset += 1U;
  acquisition = beginRemoteOffer(dataset);
  previousFailures = remoteFailedCount;
  previousReleases = releaseCount;
  clipboardFiles_remoteCancel(dataset, acquisition,
      LG_CLIPBOARD_FILE_ERROR_CANCELLED);
  checkRemoteFailure(dataset, previousFailures, previousReleases, false);

  dataset += 1U;
  LG_ClipboardFileRequest request = beginRemoteList(dataset, &acquisition);
  previousFailures = remoteFailedCount;
  previousReleases = releaseCount;
  clipboardFiles_remoteCancel(dataset, request.request,
      LG_CLIPBOARD_FILE_ERROR_CANCELLED);
  checkRemoteFailure(dataset, previousFailures, previousReleases, true);

  dataset += 1U;
  acquisition = beginRemoteOffer(dataset);
  previousFailures = remoteFailedCount;
  previousReleases = releaseCount;
  remoteRequestSucceeds = false;
  clipboardFiles_remoteAcquired(dataset, acquisition,
      LG_CLIPBOARD_FILE_ERROR_NONE);
  remoteRequestSucceeds = true;
  checkRemoteFailure(dataset, previousFailures, previousReleases, true);

  dataset += 1U;
  request = beginRemoteList(dataset, &acquisition);
  LG_ClipboardFileRequest malformed = request;
  malformed.node += 1U;
  previousFailures = remoteFailedCount;
  previousReleases = releaseCount;
  CHECK(clipboardFiles_remoteDataBegin(&malformed, 0) ==
      LG_CLIPBOARD_RESULT_FAILED);
  checkRemoteFailure(dataset, previousFailures, previousReleases, true);

  dataset += 1U;
  request = beginRemoteList(dataset, &acquisition);
  CHECK(clipboardFiles_remoteDataBegin(&request, 1) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  previousFailures = remoteFailedCount;
  previousReleases = releaseCount;
  CHECK(clipboardFiles_remoteDataChunk(&request, 0, NULL, 1) ==
      LG_CLIPBOARD_RESULT_FAILED);
  checkRemoteFailure(dataset, previousFailures, previousReleases, true);

  dataset += 1U;
  request = beginRemoteList(dataset, &acquisition);
  CHECK(clipboardFiles_remoteDataBegin(&request, 1) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  previousFailures = remoteFailedCount;
  previousReleases = releaseCount;
  CHECK(clipboardFiles_remoteDataEnd(&request, 0) ==
      LG_CLIPBOARD_RESULT_FAILED);
  checkRemoteFailure(dataset, previousFailures, previousReleases, true);

  dataset += 1U;
  request = beginRemoteList(dataset, &acquisition);
  static const char invalidName[] = "bad/name";
  const size_t wireSize = (size_t)KVMFR_CLIPBOARD_FILE_ENTRY_BYTES(
      sizeof(invalidName) - 1U);
  uint8_t * wire = calloc(1, wireSize);
  CHECK(wire);
  const KVMFRClipboardFileEntry entry =
  {
    .node       = 2,
    .size       = 1,
    .type       = KVMFR_CLIPBOARD_FILE_TYPE_REGULAR,
    .nameLength = sizeof(invalidName) - 1U,
  };
  memcpy(wire, &entry, sizeof(entry));
  memcpy(wire + sizeof(entry), invalidName, sizeof(invalidName) - 1U);
  CHECK(clipboardFiles_remoteDataBegin(&request, wireSize) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(clipboardFiles_remoteDataChunk(&request, 0, wire, wireSize) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  previousFailures = remoteFailedCount;
  previousReleases = releaseCount;
  CHECK(clipboardFiles_remoteDataEnd(&request, wireSize) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  free(wire);
  checkRemoteFailure(dataset, previousFailures, previousReleases, true);

  clipboardFiles_free();
}

static void checkRemoteUri(uint64_t presentation, const char * name)
{
  char * uri = NULL;
  size_t size = 0;
  CHECK(clipboardFiles_getRemotePresentation(
        presentation, "text/uri-list", &uri, &size));
  CHECK(uri);
  CHECK(size == strlen(uri));
  CHECK(strstr(uri, name));
  free(uri);
}

static void checkRemoteMate(uint64_t presentation, const char * name)
{
  char * gnome = NULL;
  char * mate = NULL;
  size_t gnomeSize = 0;
  size_t mateSize = 0;
  CHECK(clipboardFiles_getRemotePresentation(presentation,
        "x-special/gnome-copied-files", &gnome, &gnomeSize));
  CHECK(clipboardFiles_getRemotePresentation(presentation,
        "x-special/mate-copied-files", &mate, &mateSize));
  CHECK(gnome);
  CHECK(mate);
  CHECK(gnomeSize == mateSize);
  CHECK(memcmp(gnome, mate, mateSize) == 0);
  CHECK(mateSize >= sizeof("copy\n") - 1U);
  CHECK(memcmp(mate, "copy\n", sizeof("copy\n") - 1U) == 0);
  CHECK(strstr(mate, name));
  free(mate);
  free(gnome);
}

typedef struct ReleasePresentationTask
{
  pthread_barrier_t * barrier;
  uint64_t            presentation;
}
ReleasePresentationTask;

static void * releasePresentation(void * opaque)
{
  ReleasePresentationTask * task = opaque;
  const int result = pthread_barrier_wait(task->barrier);
  CHECK(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
  clipboardFiles_remotePresentationRelease(task->presentation);
  return NULL;
}

static void releasePresentationConcurrently(uint64_t presentation)
{
  pthread_barrier_t barrier;
  CHECK(pthread_barrier_init(&barrier, NULL, 3) == 0);
  ReleasePresentationTask task =
  {
    .barrier      = &barrier,
    .presentation = presentation,
  };
  pthread_t first;
  pthread_t second;
  CHECK(pthread_create(&first, NULL, releasePresentation, &task) == 0);
  CHECK(pthread_create(&second, NULL, releasePresentation, &task) == 0);
  const int result = pthread_barrier_wait(&barrier);
  CHECK(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
  CHECK(pthread_join(first, NULL) == 0);
  CHECK(pthread_join(second, NULL) == 0);
  CHECK(pthread_barrier_destroy(&barrier) == 0);
}

static void testRemoteLifecycle(void)
{
  CHECK(clipboardFiles_testFuseStopWake());
  CHECK(clipboardFiles_testInit(UINT64_C(0x7311000073110000)));
  CHECK(clipboardFiles_testUnsentRemoteOwnership());
  acquireCount = 0;
  releaseCount = 0;
  requestCount = 0;
  remoteReadyCount = 0;
  cancelCount = 0;

  const uint64_t firstDataset = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x73110001);
  const uint64_t first = completeRemoteOffer(firstDataset, "first.txt");

  CHECK(clipboardFiles_testRemoteRead(first, 2, 0, 9));
  LG_ClipboardFileRequest shortRead = remoteRequest;
  CHECK(clipboardFiles_remoteDataBegin(&shortRead, 8) ==
      LG_CLIPBOARD_RESULT_FAILED);

  CHECK(clipboardFiles_testRemoteRead(first, 2, 0, 9));
  shortRead = remoteRequest;
  CHECK(clipboardFiles_remoteDataBegin(&shortRead, 9) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  const uint8_t shortData[8] = { 0 };
  CHECK(clipboardFiles_remoteDataChunk(&shortRead, 0,
        shortData, sizeof(shortData)) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(clipboardFiles_remoteDataEnd(&shortRead, sizeof(shortData)) ==
      LG_CLIPBOARD_RESULT_FAILED);

  const uint64_t firstPresentation =
    clipboardFiles_remotePresentationAcquire();
  CHECK(firstPresentation == first);
  checkRemoteUri(firstPresentation, "first.txt");
  checkRemoteMate(firstPresentation, "first.txt");

  const uint64_t secondDataset = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x73110002);
  completeRemoteOffer(secondDataset, "second.txt");
  CHECK(clipboardFiles_testRemoteDatasetCount() == 2U);
  CHECK(releaseCount == 0U);
  checkRemoteUri(firstPresentation, "first.txt");
  clipboardFiles_remotePresentationRelease(firstPresentation);
  CHECK(clipboardFiles_testRemoteDatasetCount() == 1U);
  CHECK(releaseCount == 1U);
  CHECK(releasedDataset == firstDataset);
  CHECK(releasedAcquisition == first);

  const uint64_t secondPresentation =
    clipboardFiles_remotePresentationAcquire();
  CHECK(secondPresentation != 0);
  checkRemoteUri(secondPresentation, "second.txt");
  clipboardFiles_remotePresentationDelivered(secondPresentation);
  clipboardFiles_remotePresentationRelease(secondPresentation);
  const uint64_t thirdDataset = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x73110003);
  completeRemoteOffer(thirdDataset, "third.txt");
  CHECK(clipboardFiles_testRemoteDatasetCount() == 2U);
  CHECK(releaseCount == 1U);
  CHECK(clipboardFiles_testBeginRemoteLookup(secondPresentation));
  clipboardFiles_testExpireRemoteDeliveries();
  CHECK(clipboardFiles_testRemoteDatasetCount() == 2U);
  CHECK(releaseCount == 1U);
  clipboardFiles_testEndRemoteLookup(secondPresentation);
  CHECK(clipboardFiles_testRemoteDatasetCount() == 1U);
  CHECK(releaseCount == 2U);
  CHECK(releasedDataset == secondDataset);
  clipboardFiles_remoteClear();
  CHECK(clipboardFiles_testRemoteDatasetCount() == 0U);
  CHECK(releaseCount == 3U);

  for (uint64_t i = 0; i < 10; ++i)
  {
    const uint64_t dataset = KVMFR_CLIPBOARD_TRANSFER_HELPER |
      (UINT64_C(0x73120000) + i);
    const uint64_t acquisition =
      completeRemoteOffer(dataset, "grace.txt");
    const uint64_t presentation =
      clipboardFiles_remotePresentationAcquire();
    CHECK(presentation == acquisition);
    checkRemoteUri(presentation, "grace.txt");
    clipboardFiles_remotePresentationDelivered(presentation);
    clipboardFiles_remotePresentationRelease(presentation);
    CHECK(clipboardFiles_testRemoteDatasetCount() <= 5U);
  }
  clipboardFiles_remoteClear();
  clipboardFiles_testExpireRemoteDeliveries();
  CHECK(clipboardFiles_testRemoteDatasetCount() == 0U);

  for (uint64_t i = 0; i < 16; ++i)
  {
    const uint64_t oldDataset = KVMFR_CLIPBOARD_TRANSFER_HELPER |
      (UINT64_C(0x73130000) + i * 2U);
    const uint64_t oldAcquisition =
      completeRemoteOffer(oldDataset, "old.txt");
    const uint64_t presentation =
      clipboardFiles_remotePresentationAcquire();
    CHECK(presentation == oldAcquisition);
    const uint64_t currentDataset = oldDataset + 1U;
    completeRemoteOffer(currentDataset, "current.txt");
    const unsigned previousReleases = releaseCount;
    releasePresentationConcurrently(presentation);
    CHECK(releaseCount == previousReleases + 1U);
    CHECK(releasedDataset == oldDataset);
    CHECK(clipboardFiles_testRemoteDatasetCount() == 1U);
    clipboardFiles_remoteClear();
    CHECK(clipboardFiles_testRemoteDatasetCount() == 0U);
  }

  const uint64_t pendingDataset = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x73140001);
  CHECK(clipboardFiles_remoteOffer(pendingDataset));
  CHECK(acquiredDataset == pendingDataset);
  const uint64_t pendingAcquisition = acquiredRequest;
  const uint64_t replacementDataset = pendingDataset + 1U;
  const unsigned previousCancels = cancelCount;
  CHECK(clipboardFiles_remoteOffer(replacementDataset));
  CHECK(cancelCount == previousCancels + 1U);
  CHECK(cancelledDataset == pendingDataset);
  CHECK(cancelledRequest == pendingAcquisition);
  CHECK(cancelledReason == LG_CLIPBOARD_FILE_ERROR_CANCELLED);
  CHECK(clipboardFiles_testRemoteDatasetCount() == 1U);
  clipboardFiles_remoteClear();
  CHECK(clipboardFiles_testRemoteDatasetCount() == 0U);
  clipboardFiles_free();
}

static int testMount(void)
{
  if (access("/dev/fuse", R_OK | W_OK) < 0)
    return 77;
  if (!getenv("XDG_RUNTIME_DIR"))
    CHECK(setenv("XDG_RUNTIME_DIR", "/tmp", 1) == 0);
  CHECK(clipboardFiles_init());
  clipboardFiles_free();
  return 0;
}

int main(int argc, char * argv[])
{
  debug_init();

  if (argc == 2 && !strcmp(argv[1], "mount"))
    return testMount();
  if (argc == 2 && !strcmp(argv[1], "failures"))
  {
    testRemoteFailures();
    return 0;
  }
  testLocalLifecycle();
  testLocalUriValidation();
  testLocalSnapshot();
  testLocalUnsupportedEntry();
  testRemoteLifecycle();
  free(responseData);
  return 0;
}
