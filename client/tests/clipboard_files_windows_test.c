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
#include "test.h"

static struct
{
  uint64_t              dataset;
  uint64_t              transfer;
  LG_ClipboardFileError error;
  unsigned int          acquired;
  unsigned int          cancelled;
}
trace;

bool clipboard_fileAcquired(uint64_t dataset, uint64_t acquisition,
    LG_ClipboardFileError error)
{
  trace.dataset  = dataset;
  trace.transfer = acquisition;
  trace.error    = error;
  ++trace.acquired;
  return true;
}

bool clipboard_fileCancel(uint64_t dataset, uint64_t request,
    LG_ClipboardFileError reason)
{
  trace.dataset  = dataset;
  trace.transfer = request;
  trace.error    = reason;
  ++trace.cancelled;
  return true;
}

int main(void)
{
  const LG_ClipboardFileRequest request =
  {
    .dataset   = UINT64_C(0x1234),
    .request   = UINT64_C(0x5678),
    .node      = 1,
    .operation = LG_CLIPBOARD_FILE_READ,
  };

  CHECK(clipboardFiles_init());
  CHECK(!clipboardFiles_remoteOffer(request.dataset));
  CHECK(!clipboardFiles_remoteReady(request.dataset));

  char  * data = (char *)(uintptr_t)1;
  size_t  size = 1;
  CHECK(!clipboardFiles_getRemote("text/uri-list", &data, &size));
  CHECK(!data);
  CHECK(size == 0);

  data = (char *)(uintptr_t)1;
  size = 1;
  CHECK(!clipboardFiles_getRemotePresentation(
      1, "text/uri-list", &data, &size));
  CHECK(!data);
  CHECK(size == 0);

  clipboardFiles_localAcquire(request.dataset, request.request);
  CHECK(trace.acquired == 1);
  CHECK(trace.dataset == request.dataset);
  CHECK(trace.transfer == request.request);
  CHECK(trace.error == LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED);

  clipboardFiles_localRequest(&request);
  CHECK(trace.cancelled == 1);
  CHECK(trace.dataset == request.dataset);
  CHECK(trace.transfer == request.request);
  CHECK(trace.error == LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED);

  clipboardFiles_localRequest(NULL);
  CHECK(trace.cancelled == 1);
  CHECK(clipboardFiles_remoteDataBegin(&request, 0) ==
      LG_CLIPBOARD_RESULT_FAILED);
  CHECK(clipboardFiles_remoteDataChunk(&request, 0, NULL, 0) ==
      LG_CLIPBOARD_RESULT_FAILED);
  CHECK(clipboardFiles_remoteDataEnd(&request, 0) ==
      LG_CLIPBOARD_RESULT_FAILED);
  clipboardFiles_free();
  return 0;
}
