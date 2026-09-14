/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

#define OBEX_TYPE_CLIENT (obex_client_get_type())
G_DECLARE_FINAL_TYPE(ObexClient, obex_client, OBEX, CLIENT, GObject)

/* One batch or incoming transfer is handled at a time. All calls are asynchronous.
 * Signals: changed(); prompt-changed(); error(message); completed(incoming, name). */
ObexClient *obex_client_new(GDBusConnection *session_bus);
void obex_client_stop(ObexClient *self);
void obex_client_set_locked(ObexClient *self, gboolean locked);
gboolean obex_client_available(ObexClient *self);
gboolean obex_client_ready(ObexClient *self);
gboolean obex_client_busy(ObexClient *self);
gboolean obex_client_incoming(ObexClient *self);
const char *obex_client_name(ObexClient *self);       /* borrowed */
const char *obex_client_status(ObexClient *self);     /* borrowed */
guint64 obex_client_transferred(ObexClient *self);
guint64 obex_client_size(ObexClient *self);
guint obex_client_file_number(ObexClient *self);
guint obex_client_file_count(ObexClient *self);

/* Paths must be readable regular files. The array and strings remain caller-owned. */
gboolean obex_client_send(ObexClient *self, const char *address,
                          GPtrArray *paths, GError **error);
void obex_client_cancel(ObexClient *self);

gboolean obex_client_has_prompt(ObexClient *self);
const char *obex_client_prompt_name(ObexClient *self); /* borrowed */
guint64 obex_client_prompt_size(ObexClient *self);
void obex_client_answer(ObexClient *self, gboolean accept);
const char *obex_client_receive_directory(ObexClient *self); /* borrowed */
/* Overrides the default Downloads/Bluetooth directory while idle (distro policy/tests). */
void obex_client_set_receive_directory(ObexClient *self, const char *directory);

/* Public for focused path-safety tests. Returns a new, currently unused path. */
char *obex_safe_receive_path(const char *directory, const char *suggested,
                             GError **error);
