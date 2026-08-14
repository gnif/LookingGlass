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
#include "main.h"
#include "clipboard.h"
#include "test.h"

#include "common/debug.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_DATA     32U
#define TEST_ALARM_S 5U

struct AppState  g_state;
struct AppParams g_params;

static struct
{
  unsigned int grab;
  unsigned int request;
  unsigned int dataStart;
  unsigned int data;
  PSDataType   requestType;
  PSDataType   dataType;
  size_t       dataSize;
  uint8_t      dataBuf[MAX_DATA];

  unsigned int        notice;
  LG_ClipboardRequest localRequest;

  unsigned int     reply;
  LG_ClipboardData replyType;
  uint32_t         replySize;
  uint8_t          replyBuf[MAX_DATA];
}
test;

bool purespice_clipboardRequest(PSDataType type)
{
  ++test.request;
  test.requestType = type;
  return true;
}

bool purespice_clipboardGrab(PSDataType types[], int count)
{
  CHECK(types);
  CHECK(count > 0);
  ++test.grab;
  return true;
}

bool purespice_clipboardRelease(void)
{
  return true;
}

bool purespice_clipboardDataStart(PSDataType type, size_t size)
{
  ++test.dataStart;
  test.dataType = type;
  test.dataSize = size;
  return true;
}

bool purespice_clipboardData(
    PSDataType type, uint8_t * data, size_t size)
{
  CHECK(type == test.dataType);
  CHECK(size == test.dataSize);
  CHECK(size <= sizeof(test.dataBuf));
  ++test.data;
  if (size)
    memcpy(test.dataBuf, data, size);
  return true;
}

static void cbNotice(LG_ClipboardData type)
{
  CHECK(type == LG_CLIPBOARD_DATA_TEXT);
  ++test.notice;
}

static void cbRelease(void)
{
}

static void cbRequest(
    LG_ClipboardRequest request, LG_ClipboardData type)
{
  CHECK(type == LG_CLIPBOARD_DATA_TEXT);
  test.localRequest = request;
}

static struct LG_DisplayServerOps displayOps =
{
  .cbNotice  = cbNotice,
  .cbRelease = cbRelease,
  .cbRequest = cbRequest,
};

static void reply(void * opaque, LG_ClipboardData type,
    const uint8_t * data, uint32_t size)
{
  (void)opaque;
  ++test.reply;
  test.replyType = type;
  test.replySize = size;
  CHECK(size <= sizeof(test.replyBuf));
  if (size)
    memcpy(test.replyBuf, data, size);
}

static SpiceClipboard * setup(void)
{
  SpiceClipboard * clipboard;
  CHECK(spiceClipboard_init(&clipboard));
  spiceClipboard_setCallbackTarget(clipboard);
  spiceClipboard_setAvailable(clipboard, true);

  g_state.ds                = &displayOps;
  g_params.clipboardToVM    = true;
  g_params.clipboardToLocal = true;
  CHECK(lgClipboard_init());
  lgClipboard_setLocalAvailable(true);
  lgClipboard_setFallback(spiceClipboard_getOps(), clipboard);
  return clipboard;
}

static void teardown(SpiceClipboard ** clipboard)
{
  lgClipboard_free();
  spiceClipboard_setCallbackTarget(NULL);
  spiceClipboard_free(clipboard);
}

static void testOutbound(void)
{
  SpiceClipboard * clipboard = setup();
  const LG_ClipboardData types[] = { LG_CLIPBOARD_DATA_TEXT };
  lgClipboard_notifyTypes(types, 1);
  CHECK(test.grab == 1);

  spiceClipboard_request(SPICE_DATA_TEXT);
  CHECK(test.localRequest != LG_CLIPBOARD_REQUEST_INVALID);
  const uint8_t first[]  = { 1, 2 };
  const uint8_t second[] = { 3, 4 };
  CHECK(lgClipboard_dataBegin(test.localRequest,
      LG_CLIPBOARD_DATA_TEXT, LG_CLIPBOARD_SIZE_UNKNOWN) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(lgClipboard_dataChunk(test.localRequest,
      0, first, sizeof(first)) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(lgClipboard_dataChunk(test.localRequest,
      sizeof(first), second, sizeof(second)) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(lgClipboard_dataEnd(test.localRequest, 4) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);

  CHECK(test.dataStart == 1);
  CHECK(test.data == 1);
  CHECK(test.dataType == SPICE_DATA_TEXT);
  CHECK(test.dataSize == 4);
  CHECK(memcmp(test.dataBuf, "\x01\x02\x03\x04", 4) == 0);
  teardown(&clipboard);
}

static void testInbound(void)
{
  SpiceClipboard * clipboard = setup();
  spiceClipboard_notice(SPICE_DATA_TEXT);
  CHECK(test.notice == 1);
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, reply, NULL));
  CHECK(test.request == 1);
  CHECK(test.requestType == SPICE_DATA_TEXT);

  uint8_t data[] = "one\r\ntwo";
  spiceClipboard_data(SPICE_DATA_TEXT, data, sizeof(data) - 1);
  CHECK(test.reply == 1);
  CHECK(test.replyType == LG_CLIPBOARD_DATA_TEXT);
  CHECK(test.replySize == 7);
  CHECK(memcmp(test.replyBuf, "one\ntwo", 7) == 0);
  teardown(&clipboard);
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "outbound", testOutbound },
  { "inbound" , testInbound  },
};

int main(int argc, char ** argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <case>\n", argv[0]);
    return EXIT_FAILURE;
  }

  alarm(TEST_ALARM_S);
  debug_init();
  for (unsigned int i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    if (strcmp(argv[1], tests[i].name) == 0)
    {
      tests[i].run();
      return EXIT_SUCCESS;
    }

  fprintf(stderr, "unknown test: %s\n", argv[1]);
  return EXIT_FAILURE;
}
