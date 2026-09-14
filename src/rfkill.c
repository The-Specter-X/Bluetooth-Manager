/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rfkill.h"
#include <glib-unix.h>
#include <linux/rfkill.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

typedef struct { struct rfkill_event event; char *name; } Switch;
struct Rfkill { int fd; guint watch; GHashTable *switches; GSourceFunc changed; gpointer data; };

static void
switch_free(gpointer data)
{
    Switch *item = data;
    g_free(item->name);
    g_free(item);
}

static gboolean
read_events(gint fd, GIOCondition condition, gpointer data)
{
    Rfkill *radio = data;
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        radio->watch = 0;
        g_hash_table_remove_all(radio->switches);
        radio->changed(radio->data);
        return G_SOURCE_REMOVE;
    }
    struct rfkill_event event;
    ssize_t size;
    while ((size = read(fd, &event, sizeof event)) >= (ssize_t)RFKILL_EVENT_SIZE_V1) {
        if (event.op == RFKILL_OP_DEL) {
            g_hash_table_remove(radio->switches, GUINT_TO_POINTER(event.idx));
        } else if (event.type == RFKILL_TYPE_BLUETOOTH &&
                   (event.op == RFKILL_OP_ADD || event.op == RFKILL_OP_CHANGE)) {
            Switch *item = g_new0(Switch, 1);
            item->event = event;
            g_autofree char *path = g_strdup_printf("/sys/class/rfkill/rfkill%u/name", event.idx);
            if (g_file_get_contents(path, &item->name, NULL, NULL))
                g_strchomp(item->name);
            g_hash_table_replace(radio->switches, GUINT_TO_POINTER(event.idx), item);
        }
    }
    radio->changed(radio->data);
    return G_SOURCE_CONTINUE;
}

Rfkill *
rfkill_new(GSourceFunc changed, gpointer data)
{
    Rfkill *radio = g_new0(Rfkill, 1);
    radio->switches = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, switch_free);
    radio->changed = changed;
    radio->data = data;
    radio->fd = open("/dev/rfkill", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (radio->fd >= 0)
        radio->watch = g_unix_fd_add(radio->fd, G_IO_IN | G_IO_HUP | G_IO_ERR, read_events, radio);
    return radio;
}

RadioState
rfkill_state(Rfkill *radio, const char *adapter_path)
{
    if (!radio || !adapter_path)
        return RADIO_UNKNOWN;
    const char *name = strrchr(adapter_path, '/');
    name = name ? name + 1 : adapter_path;
    RadioState state = RADIO_UNKNOWN;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, radio->switches);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Switch *item = value;
        /* Platform Bluetooth switches affect all adapters; hci switches affect one. */
        if (item->name && g_str_has_prefix(item->name, "hci") && !g_str_equal(item->name, name))
            continue;
        if (item->event.hard)
            return RADIO_HARD_BLOCKED;
        if (item->event.soft)
            state = RADIO_SOFT_BLOCKED;
        else if (state == RADIO_UNKNOWN)
            state = RADIO_CLEAR;
    }
    return state;
}

gboolean
rfkill_unblock(Rfkill *radio, const char *adapter_path, GError **error)
{
    int fd = open("/dev/rfkill", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        g_set_error_literal(error, G_FILE_ERROR, g_file_error_from_errno(errno),
            "Your session cannot unblock the Bluetooth radio. Check the system's rfkill permissions.");
        return FALSE;
    }
    const char *name = strrchr(adapter_path, '/');
    name = name ? name + 1 : adapter_path;
    gboolean ok = TRUE;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, radio->switches);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Switch *item = value;
        if (item->name && g_str_has_prefix(item->name, "hci") && !g_str_equal(item->name, name))
            continue;
        if (!item->event.soft)
            continue;
        struct rfkill_event event = { .idx = item->event.idx, .type = RFKILL_TYPE_BLUETOOTH,
                                     .op = RFKILL_OP_CHANGE, .soft = 0 };
        if (write(fd, &event, RFKILL_EVENT_SIZE_V1) != RFKILL_EVENT_SIZE_V1) {
            g_set_error_literal(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not unblock the Bluetooth radio.");
            ok = FALSE;
            break;
        }
    }
    close(fd);
    return ok;
}

void
rfkill_free(Rfkill *radio)
{
    if (!radio)
        return;
    if (radio->watch)
        g_source_remove(radio->watch);
    if (radio->fd >= 0)
        close(radio->fd);
    g_hash_table_unref(radio->switches);
    g_free(radio);
}
