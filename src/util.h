/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

#define MYTOOTH_ID "io.github.the_specter_x.Mytooth"
#define BT_ADAPTER "org.bluez.Adapter1"
#define BT_DEVICE "org.bluez.Device1"
#define BT_BATTERY "org.bluez.Battery1"

/* Property strings are newly allocated. Missing/invalid properties use defaults. */
char *bt_string(GDBusProxy *proxy, const char *property, const char *fallback);
gboolean bt_boolean(GDBusProxy *proxy, const char *property);
int bt_battery(GDBusProxy *proxy);
char *bt_error_message(const GError *error);
gboolean bt_valid_pin(const char *text);
gboolean bt_parse_passkey(const char *text, guint32 *value);
