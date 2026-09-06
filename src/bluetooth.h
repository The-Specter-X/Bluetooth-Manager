/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "util.h"

#define BT_TYPE_CLIENT (bt_client_get_type())
G_DECLARE_FINAL_TYPE(BtClient, bt_client, BT, CLIENT, GObject)

/* All methods and signals run on the creating main context.
 * Signals: changed(); error(message); operation(path, method, success).
 * Caller owns returned arrays/proxies. Array elements are owned by the array. */
BtClient *bt_client_new(GDBusConnection *connection);
void bt_client_stop(BtClient *self);
GDBusConnection *bt_client_connection(BtClient *self); /* borrowed */
const char *bt_client_owner(BtClient *self); /* borrowed; NULL while unavailable */
GPtrArray *bt_client_list(BtClient *self, const char *interface);
GDBusProxy *bt_client_proxy(BtClient *self, const char *path, const char *interface);
gboolean bt_client_busy(BtClient *self, const char *path);
const char *bt_client_pending(BtClient *self, const char *path);
void bt_client_method(BtClient *self, const char *path, const char *interface,
                      const char *method, GVariant *parameters);
void bt_client_set(BtClient *self, const char *path, const char *interface,
                   const char *property, GVariant *value);
void bt_client_discoverable(BtClient *self, const char *path, gboolean enabled);
void bt_client_scan(BtClient *self, const char *adapter_path); /* NULL stops our scan */
const char *bt_client_scan_path(BtClient *self);
void bt_client_error(BtClient *self, const char *message);
