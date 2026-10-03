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

#ifndef _H_LG_PLATFORM_WINDOWS_CLIPBOARD_FILES_WIN32_
#define _H_LG_PLATFORM_WINDOWS_CLIPBOARD_FILES_WIN32_

#include "interface/clipboard.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <windows.h>
#include <ole2.h>
#include <shellapi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WindowsClipboardLocalFiles     WindowsClipboardLocalFiles;
typedef struct WindowsClipboardRemoteManifest WindowsClipboardRemoteManifest;

typedef struct WindowsClipboardRemoteProviderOps
{
  HRESULT (*read   )(void * opaque, uint64_t dataset, uint64_t acquisition,
      uint64_t node, uint64_t offset, void * data, ULONG length,
      ULONG * read);
  void    (*release)(void * opaque, uint64_t dataset, uint64_t acquisition);
}
WindowsClipboardRemoteProviderOps;

/* Capture copies the selection metadata. List and read return malloc-owned
 * buffers which the caller releases with free(). */
WindowsClipboardLocalFiles * windowsClipboardLocalFilesCapture(HDROP drop,
    LG_ClipboardFileError * error);
void windowsClipboardLocalFilesRetain(WindowsClipboardLocalFiles * files);
void windowsClipboardLocalFilesRelease(WindowsClipboardLocalFiles * files);
bool windowsClipboardLocalFilesList(WindowsClipboardLocalFiles * files,
    uint64_t node, void ** data, size_t * size,
    LG_ClipboardFileError * error);
bool windowsClipboardLocalFilesRead(WindowsClipboardLocalFiles * files,
    uint64_t node, uint64_t offset, uint32_t length, void ** data,
    size_t * size, LG_ClipboardFileError * error);

WindowsClipboardRemoteManifest * windowsClipboardRemoteManifestCreate(void);
void windowsClipboardRemoteManifestFree(
    WindowsClipboardRemoteManifest * manifest);
/* Call NextDirectory before each LIST request, then append the complete
 * response for the returned node. The first returned node is the wire root. */
bool windowsClipboardRemoteManifestAppendList(
    WindowsClipboardRemoteManifest * manifest, uint64_t parent,
    const void * data, size_t size, LG_ClipboardFileError * error);
bool windowsClipboardRemoteManifestNextDirectory(
    WindowsClipboardRemoteManifest * manifest, uint64_t * node);
bool windowsClipboardRemoteManifestHasEntries(
    const WindowsClipboardRemoteManifest * manifest);

/* The data object owns one provider lease after a successful return. The
 * release callback runs once after both the object and all of its streams are
 * released. A failed call does not invoke it. */
HRESULT windowsClipboardRemoteCreateDataObject(
    WindowsClipboardRemoteManifest * manifest, uint64_t dataset,
    uint64_t acquisition, const WindowsClipboardRemoteProviderOps * ops,
    void * opaque, IDataObject ** object);

#ifdef __cplusplus
}
#endif

#endif
