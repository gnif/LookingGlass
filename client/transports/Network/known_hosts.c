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

#include "known_hosts.h"
#include "platform.h"

#include "common/debug.h"
#include "common/filesystem.h"
#include "common/paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_KNOWN_HOSTS_FILE "network_known_hosts"
#define NETWORK_KNOWN_HOST_LINE  1024U

static char * knownHostsPath(const char * suffix)
{
  const char   * directory    = lgConfigDir();
  const size_t   dirLength    = strlen(directory);
  const size_t   nameLength   = strlen(NETWORK_KNOWN_HOSTS_FILE);
  const size_t   suffixLength = strlen(suffix);
  char         * path         = malloc(
      dirLength + 1U + nameLength + suffixLength + 1U);
  if (!path)
    return NULL;

  snprintf(path, dirLength + 1U + nameLength + suffixLength + 1U,
      "%s/%s%s", directory, NETWORK_KNOWN_HOSTS_FILE, suffix);
  return path;
}

static bool validField(const char * value)
{
  if (!value || !*value)
    return false;

  for (const unsigned char * c = (const unsigned char *)value; *c; ++c)
    if (*c == '\r' || *c == '\n' || *c == '\t')
      return false;
  return true;
}

static bool completeLine(const char * line, FILE * file)
{
  const size_t length = strlen(line);
  return (length && line[length - 1U] == '\n') || feof(file);
}

static bool writeLine(FILE * file, const char * line)
{
  const size_t length = strlen(line);
  if (!length)
    return false;
  return fputs(line, file) >= 0 &&
    (line[length - 1U] == '\n' || fputc('\n', file) != EOF);
}

bool networkKnownHostsInit(NetworkKnownHosts * knownHosts)
{
  if (!knownHosts)
    return false;

  memset(knownHosts, 0, sizeof(*knownHosts));
  LG_LOCK_INIT(knownHosts->lock);
  knownHosts->path = knownHostsPath("");
  return knownHosts->path != NULL;
}

void networkKnownHostsFree(NetworkKnownHosts * knownHosts)
{
  if (!knownHosts)
    return;

  LG_LOCK_FREE(knownHosts->lock);
  free(knownHosts->path);
  knownHosts->path = NULL;
}

NetworkKnownHostResult networkKnownHostsCheck(NetworkKnownHosts * knownHosts,
    const char * endpoint, const char * fingerprint)
{
  if (!knownHosts || !knownHosts->path || !validField(endpoint) ||
      !validField(fingerprint))
    return NETWORK_KNOWN_HOST_ERROR;

  LG_LOCK(knownHosts->lock);
  FILE * file = lgOpenFile(knownHosts->path, "r");
  if (!file)
  {
    const LGFileType type = lgPathType(knownHosts->path);
    LG_UNLOCK(knownHosts->lock);
    return type == LG_FILE_TYPE_MISSING ?
      NETWORK_KNOWN_HOST_UNKNOWN : NETWORK_KNOWN_HOST_ERROR;
  }

  NetworkKnownHostResult result = NETWORK_KNOWN_HOST_UNKNOWN;
  char                   line[NETWORK_KNOWN_HOST_LINE];
  while (fgets(line, sizeof(line), file))
  {
    if (!completeLine(line, file))
    {
      result = NETWORK_KNOWN_HOST_ERROR;
      break;
    }

    char * separator = strchr(line, '\t');
    if (!separator)
      continue;

    *separator++ = '\0';
    separator[strcspn(separator, "\r\n")] = '\0';
    if (strcmp(line, endpoint) != 0)
      continue;

    if (strcmp(separator, fingerprint) != 0)
    {
      result = NETWORK_KNOWN_HOST_CHANGED;
      break;
    }
    result = NETWORK_KNOWN_HOST_MATCH;
  }

  if (ferror(file))
    result = NETWORK_KNOWN_HOST_ERROR;

  fclose(file);
  LG_UNLOCK(knownHosts->lock);
  return result;
}

static bool rewriteKnownHosts(NetworkKnownHosts * knownHosts,
    const char * endpoint, const char * fingerprint)
{
  if (!knownHosts || !knownHosts->path || !validField(endpoint) ||
      (fingerprint && !validField(fingerprint)))
    return false;

  char * temporary = knownHostsPath(".tmp");
  if (!temporary)
    return false;

  LG_LOCK(knownHosts->lock);
  FILE * source = lgOpenFile(knownHosts->path, "r");
  FILE * target = lgOpenFile(temporary, "w");
  bool   found  = false;
  bool   result = target &&
    (source || lgPathType(knownHosts->path) == LG_FILE_TYPE_MISSING);
  char   line[NETWORK_KNOWN_HOST_LINE];
  while (result && source && fgets(line, sizeof(line), source))
  {
    if (!completeLine(line, source))
    {
      result = false;
      break;
    }

    char * separator = strchr(line, '\t');
    if (!separator)
    {
      result = writeLine(target, line);
      continue;
    }

    *separator++ = '\0';
    if (strcmp(line, endpoint) != 0)
    {
      *--separator = '\t';
      result = writeLine(target, line);
      continue;
    }

    if (!fingerprint)
      continue;

    separator[strcspn(separator, "\r\n")] = '\0';
    if (strcmp(separator, fingerprint) != 0)
    {
      result = false;
      break;
    }
    found = true;
    result = fprintf(target, "%s\t%s\n", endpoint, fingerprint) > 0;
  }

  if (source && ferror(source))
    result = false;
  if (result && fingerprint && !found)
    result = fprintf(target, "%s\t%s\n", endpoint, fingerprint) > 0;

  if (target)
  {
    result = result && fflush(target) == 0;
    result = fclose(target) == 0 && result;
  }
  if (source)
    fclose(source);

  if (result)
    result = networkPlatformReplaceFile(temporary, knownHosts->path);

  if (!result)
    lgDeleteFile(temporary);
  LG_UNLOCK(knownHosts->lock);
  free(temporary);
  return result;
}

bool networkKnownHostsRemember(NetworkKnownHosts * knownHosts,
    const char * endpoint, const char * fingerprint)
{
  const bool result = rewriteKnownHosts(
      knownHosts, endpoint, fingerprint);
  if (!result)
    DEBUG_ERROR("Failed to persist the network server fingerprint");
  return result;
}

bool networkKnownHostsForget(NetworkKnownHosts * knownHosts,
    const char * endpoint)
{
  return rewriteKnownHosts(knownHosts, endpoint, NULL);
}

void networkFingerprintFormat(
    const uint8_t fingerprint[NETWORK_FINGERPRINT_SIZE],
    char text[NETWORK_FINGERPRINT_TEXT_SIZE])
{
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < NETWORK_FINGERPRINT_SIZE; ++i)
  {
    text[i * 2U    ] = digits[fingerprint[i] >> 4];
    text[i * 2U + 1] = digits[fingerprint[i] & 0xf];
  }
  text[NETWORK_FINGERPRINT_TEXT_SIZE - 1U] = '\0';
}
