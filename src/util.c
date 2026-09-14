/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"
#include <string.h>

char *
bt_string(GDBusProxy *proxy, const char *property, const char *fallback)
{
    g_autoptr(GVariant) value = proxy ? g_dbus_proxy_get_cached_property(proxy, property) : NULL;
    if (value && (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) ||
                  g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH)))
        return g_variant_dup_string(value, NULL);
    return g_strdup(fallback);
}

gboolean
bt_boolean(GDBusProxy *proxy, const char *property)
{
    g_autoptr(GVariant) value = proxy ? g_dbus_proxy_get_cached_property(proxy, property) : NULL;
    return value && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value);
}

int
bt_battery(GDBusProxy *proxy)
{
    g_autoptr(GVariant) value = proxy ? g_dbus_proxy_get_cached_property(proxy, "Percentage") : NULL;
    if (!value || !g_variant_is_of_type(value, G_VARIANT_TYPE_BYTE))
        return -1;
    int level = g_variant_get_byte(value);
    return level <= 100 ? level : -1;
}

char *
bt_error_message(const GError *error)
{
    if (!error)
        return g_strdup("The Bluetooth operation failed.");
    g_autofree char *name = g_dbus_error_get_remote_error(error);
    const struct { const char *suffix; const char *message; } errors[] = {
        {"AuthenticationCanceled", "Pairing was cancelled."},
        {"AuthenticationRejected", "The device rejected pairing. Check its pairing mode and try again."},
        {"AuthenticationTimeout", "The device did not respond to pairing in time."},
        {"AuthenticationFailed", "Pairing failed. Check the code and try again."},
        {"ConnectionAttemptFailed", "Could not connect. Make sure the device is nearby and available."},
        {"NotReady", "Bluetooth is off or blocked. Check the adapter and radio switch."},
        {"NotAuthorized", "Your session is not authorized to perform this Bluetooth operation."},
        {"AccessDenied", "Your session does not have permission for this operation."},
        {"NotSupported", "This operation is not supported by the device or Bluetooth stack."},
        {"InProgress", "Another Bluetooth operation is still in progress."},
        {"AlreadyExists", "The device is already paired."},
        {"AlreadyConnected", "The device is already connected."},
        {"NotConnected", "The device is already disconnected."},
        {"DoesNotExist", "The device is no longer available."},
        {"UnknownObject", "The device or adapter was removed."},
        {"ServiceUnknown", "The Bluetooth service is unavailable."},
        {"NameHasNoOwner", "The Bluetooth service stopped. Try again when it returns."},
        {"NoReply", "The Bluetooth operation timed out. Check the device and try again."},
        {"Canceled", "The operation was cancelled."},
        {"Rejected", "The request was rejected."},
    };
    for (guint i = 0; name && i < G_N_ELEMENTS(errors); i++) {
        const char *last = strrchr(name, '.');
        if (last && g_str_equal(last + 1, errors[i].suffix))
            return g_strdup(errors[i].message);
    }
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
        return g_strdup("The Bluetooth operation timed out. Check the device and try again.");
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return g_strdup("The operation was cancelled.");
    /* Do not echo arbitrary daemon error strings: they can contain addresses. */
    return g_strdup("The Bluetooth operation failed. Check the device and try again.");
}

gboolean
bt_valid_pin(const char *text)
{
    if (!text || !g_utf8_validate(text, -1, NULL))
        return FALSE;
    gsize size = strlen(text);
    if (size < 1 || size > 16)
        return FALSE;
    for (const char *p = text; *p; p = g_utf8_next_char(p))
        if (g_unichar_iscntrl(g_utf8_get_char(p)))
            return FALSE;
    return TRUE;
}

gboolean
bt_parse_passkey(const char *text, guint32 *value)
{
    if (!text || !*text || strlen(text) > 6)
        return FALSE;
    guint32 number = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9')
            return FALSE;
        number = number * 10 + (guint32)(*p - '0');
    }
    if (value)
        *value = number;
    return TRUE;
}
