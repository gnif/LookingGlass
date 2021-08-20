/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include "portal.h"
#include "common/debug.h"
#include "common/stringutils.h"
#include <string.h>
#include <stdlib.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

struct Portal
{
  GDBusConnection * conn;
  GDBusProxy      * screenCast;
  char            * senderName;
};

struct DBusCallback
{
  guint id;
  bool completed;
  void * opaque;
};


struct Portal * portal_create(void)
{
  struct Portal * portal = calloc(1, sizeof(*portal));
  if (!portal)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return NULL;
  }

  g_autoptr(GError) err = NULL;
  portal->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (err)
  {
    DEBUG_ERROR("Failed to get dbus session: %s", err->message);
    goto fail;
  }

  const gchar * uniqueName = g_dbus_connection_get_unique_name(portal->conn);
  if (!uniqueName)
  {
    DEBUG_ERROR("Failed to get dbus connection unique name");
    goto fail;
  }

  portal->senderName = strdup(uniqueName + 1);
  if (!portal->senderName)
  {
    DEBUG_ERROR("Failed to allocate memory");
    goto fail;
  }

  char * ptr = portal->senderName;
  while ((ptr = strchr(ptr, '.')))
    *ptr++ = '_';

  portal->screenCast = g_dbus_proxy_new_sync(portal->conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
    "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
    "org.freedesktop.portal.ScreenCast", NULL, &err);
  if (err)
  {
    DEBUG_ERROR("Failed to get ScreenCast portal: %s", err->message);
    goto fail;
  }

  return portal;

fail:
  portal_free(portal);
  return NULL;
}

void portal_free(struct Portal * portal)
{
  if (!portal)
    return;

  if (portal->screenCast)
    g_object_unref(portal->screenCast);

  free(portal->senderName);

  if (portal->conn)
    g_object_unref(portal->conn);

  free(portal);
}

static void callbackRegister(struct Portal * portal, struct DBusCallback * data,
    const char * path, GDBusSignalCallback func)
{
  data->id = g_dbus_connection_signal_subscribe(portal->conn, "org.freedesktop.portal.Desktop",
    "org.freedesktop.portal.Request", "Response", path, NULL,
    G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, func, data, NULL);
}

static void callbackUnregister(struct Portal * portal, struct DBusCallback * data)
{
  if (!data->id)
    return;
  g_dbus_connection_signal_unsubscribe(portal->conn, data->id);
}

static bool getRequestPath(struct Portal * portal, char ** path, char ** token)
{
  static uint64_t counter = 0;

  alloc_sprintf(token, "lg%" PRIu64, counter);
  if (!*token)
    return false;

  alloc_sprintf(path, "/org/freedesktop/portal/desktop/request/%s/lg%" PRIu64,
    portal->senderName, counter);
  if (!path)
  {
    free(*token);
    *token = NULL;
    return false;
  }

  return true;
}

static bool getSessionToken(char ** token)
{
  static uint64_t counter = 0;

  alloc_sprintf(token, "lg%" PRIu64, counter);
  return !!*token;
}

static void createSessionCallback(GDBusConnection * conn, const char * senderName,
  const char *objectPath, const char * interfaceName, const char * signalName,
  GVariant * params, void * opaque)
{
  struct DBusCallback * callback = opaque;
  uint32_t status;
  g_autoptr(GVariant) result = NULL;
  g_variant_get(params, "(u@a{sv})", &status, &result);

  if (status != 0)
    DEBUG_ERROR("Failed to create ScreenCast: %" PRIu32, status);
  else
    g_variant_lookup(result, "session_handle", "s", callback->opaque);

  callback->completed = true;
}

bool portal_createScreenCastSession(struct Portal * portal, char ** handle)
{
  g_autoptr(GError) err = NULL;
  char * requestPath = NULL;
  char * requestToken = NULL;
  char * sessionToken = NULL;
  bool result = false;
  struct DBusCallback callback = {0, .opaque = handle};

  if (!getRequestPath(portal, &requestPath, &requestToken))
  {
    DEBUG_ERROR("Failed to get request path and token");
    goto cleanup;
  }

  if (!getSessionToken(&sessionToken))
  {
    DEBUG_ERROR("Failed to get session token");
    goto cleanup;
  }

  *handle = NULL;
  callbackRegister(portal, &callback, requestPath, createSessionCallback);

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(requestToken));
  g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(sessionToken));

  g_autoptr(GVariant) response = g_dbus_proxy_call_sync(portal->screenCast, "CreateSession",
    g_variant_new("(a{sv})", &builder), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);

  if (err)
  {
    DEBUG_ERROR("Failed to create ScreenCast session: %s", err->message);
    goto cleanup;
  }

  while (!callback.completed)
    g_main_context_iteration(NULL, TRUE);

  if (*handle)
    result = true;

cleanup:
  callbackUnregister(portal, &callback);
  free(requestPath);
  free(requestToken);
  free(sessionToken);
  return result;
}

void portal_destroySession(struct Portal * portal, char ** sessionHandle)
{
  if (!*sessionHandle)
    return;

  g_dbus_connection_call_sync(portal->conn, "org.freedesktop.portal.Desktop",
    *sessionHandle, "org.freedesktop.portal.Session", "Close", NULL, NULL,
    G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
  g_free(*sessionHandle);
  sessionHandle = NULL;
}

static void selectSourceCallback(GDBusConnection * conn, const char * senderName,
  const char *objectPath, const char * interfaceName, const char * signalName,
  GVariant * params, void * opaque)
{
  struct DBusCallback * callback = opaque;
  uint32_t status;
  g_autoptr(GVariant) result = NULL;
  g_variant_get(params, "(u@a{sv})", &status, &result);

  if (status != 0)
    DEBUG_ERROR("Failed select sources: %" PRIu32, status);
  else
    callback->opaque = (void *) 1;

  callback->completed = true;
}

bool portal_selectSource(struct Portal * portal, const char * sessionHandle)
{
  g_autoptr(GError) err = NULL;
  bool result = false;
  char * requestPath = NULL;
  char * requestToken = NULL;
  struct DBusCallback callback = {0};

  if (!getRequestPath(portal, &requestPath, &requestToken))
  {
    DEBUG_ERROR("Failed to get request path and token");
    goto cleanup;
  }

  callbackRegister(portal, &callback, requestPath, selectSourceCallback);

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(PIPEWIRE_CAPTURE_DESKTOP));
  g_variant_builder_add(&builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(requestToken));

  g_autoptr(GVariant) cursorModes_ = g_dbus_proxy_get_cached_property(
    portal->screenCast, "AvailableCursorModes");
  uint32_t cursorModes = cursorModes_ ? g_variant_get_uint32(cursorModes_) : 0;

  // TODO: support mode 4 (separate cursor)
  if (cursorModes & 2)
  {
    DEBUG_INFO("Cursor mode      : embedded");
    g_variant_builder_add(&builder, "{sv}", "cursor_mode", g_variant_new_uint32(2));
  }
  else if (cursorModes & 1)
  {
    DEBUG_INFO("Cursor mode      : none");
    g_variant_builder_add(&builder, "{sv}", "cursor_mode", g_variant_new_uint32(1));
  }
  else
  {
    DEBUG_ERROR("No known cursor mode found");
    goto cleanup;
  }

  g_autoptr(GVariant) response = g_dbus_proxy_call_sync(portal->screenCast, "SelectSources",
    g_variant_new("(oa{sv})", sessionHandle, &builder), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);

  if (err)
  {
    DEBUG_ERROR("Failed to call SelectSources: %s", err->message);
    goto cleanup;
  }

  while (!callback.completed)
    g_main_context_iteration(NULL, TRUE);

  result = callback.opaque;

cleanup:
  callbackUnregister(portal, &callback);
  free(requestPath);
  free(requestToken);
  return result;
}

static void pipewireNodeCallback(GDBusConnection * conn, const char * senderName,
  const char *objectPath, const char * interfaceName, const char * signalName,
  GVariant * params, void * opaque)
{
  struct DBusCallback * callback = opaque;
  callback->completed = true;

  uint32_t status;
  g_autoptr(GVariant) result = NULL;
  g_variant_get(params, "(u@a{sv})", &status, &result);

  if (status != 0)
  {
    DEBUG_ERROR("Failed start screencast: %" PRIu32, status);
    return;
  }

  g_autoptr(GVariant) streams = g_variant_lookup_value(result, "streams", G_VARIANT_TYPE_ARRAY);

  GVariantIter iter;
  g_variant_iter_init(&iter, streams);
  size_t count = g_variant_iter_n_children(&iter);

  if (count != 1)
  {
    DEBUG_WARN("Received more than one stream, discarding all but last one");
    while (count-- > 1)
    {
      g_autoptr(GVariant) prop = NULL;
      uint32_t node;
      g_variant_iter_loop(&iter, "(u@a{sv})", &node, &prop);
    }
  }

  g_autoptr(GVariant) prop = NULL;
  g_variant_iter_loop(&iter, "(u@a{sv})", callback->opaque, &prop);
}

uint32_t portal_getPipewireNode(struct Portal * portal, const char * sessionHandle)
{
  g_autoptr(GError) err = NULL;
  char * requestPath = NULL;
  char * requestToken = NULL;
  uint32_t result = 0;
  struct DBusCallback callback = {0, .opaque = &result};

  if (!getRequestPath(portal, &requestPath, &requestToken))
  {
    DEBUG_ERROR("Failed to get request path and token");
    goto cleanup;
  }

  callbackRegister(portal, &callback, requestPath, pipewireNodeCallback);

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(requestToken));

  g_autoptr(GVariant) response = g_dbus_proxy_call_sync(portal->screenCast, "Start",
    g_variant_new("(osa{sv})", sessionHandle, "", &builder), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);

  if (err)
  {
    DEBUG_ERROR("Failed to call Start on session: %s", err->message);
    goto cleanup;
  }

  while (!callback.completed)
    g_main_context_iteration(NULL, TRUE);

cleanup:
  callbackUnregister(portal, &callback);
  free(requestPath);
  free(requestToken);
  return result;
}

int portal_openPipewireRemote(struct Portal * portal, const char * sessionHandle)
{
  g_autoptr(GError) err = NULL;
  g_autoptr(GUnixFDList) fdList = NULL;

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);

  g_autoptr(GVariant) response = g_dbus_proxy_call_with_unix_fd_list_sync(portal->screenCast,
    "OpenPipeWireRemote", g_variant_new("(oa{sv})", sessionHandle, &builder),
    G_DBUS_CALL_FLAGS_NONE, -1, NULL, &fdList, NULL, &err);

  if (err)
  {
    DEBUG_ERROR("Failed to call OpenPipeWireRemote on session: %s", err->message);
    return -1;
  }

  gint32 index;
  g_variant_get(response, "(h)", &index, &err);
  if (err)
  {
    DEBUG_ERROR("Failed to get pipewire fd index: %s", err->message);
    return -1;
  }

  int fd = g_unix_fd_list_get(fdList, index, &err);
  if (err)
  {
    DEBUG_ERROR("Failed to get pipewire fd: %s", err->message);
    return -1;
  }

  return fd;
}
