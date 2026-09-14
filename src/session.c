/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "session.h"

#define SCREENSAVER "org.cinnamon.ScreenSaver"
#define SCREENSAVER_PATH "/org/cinnamon/ScreenSaver"

struct Session {
    guint refs;
    GDBusConnection *bus;
    BtClient *client;
    BtAgent *agent;
    GCancellable *cancel;
    char *owner;
    guint watch;
    guint active_subscription;
    guint sleep_subscription;
    guint generation;
    gboolean stopped;
};

static Session *session_ref(Session *s) { s->refs++; return s; }
static void
session_unref(Session *s)
{
    if (--s->refs)
        return;
    g_object_unref(s->bus);
    g_object_unref(s->client);
    g_object_unref(s->agent);
    g_object_unref(s->cancel);
    g_free(s->owner);
    g_free(s);
}

typedef struct { Session *session; guint generation; } Query;

static void
active_received(GObject *source, GAsyncResult *result, gpointer data)
{
    Query *query = data;
    Session *s = query->session;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
    if (!s->stopped && query->generation == s->generation) {
        gboolean active = TRUE;
        if (reply)
            g_variant_get(reply, "(b)", &active);
        bt_agent_set_locked(s->agent, active);
    }
    session_unref(s);
    g_free(query);
}

static void
query_active(Session *s)
{
    bt_agent_set_locked(s->agent, TRUE);
    s->generation++;
    if (!s->owner)
        return;
    Query *query = g_new0(Query, 1);
    query->session = session_ref(s);
    query->generation = s->generation;
    g_dbus_connection_call(s->bus, s->owner, SCREENSAVER_PATH, SCREENSAVER, "GetActive",
        NULL, G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, s->cancel,
        active_received, query);
}

static void
active_changed(GDBusConnection *bus, const char *sender, const char *path,
               const char *interface, const char *signal, GVariant *parameters, gpointer data)
{
    Session *s = data;
    if (!s->owner || g_strcmp0(sender, s->owner) != 0 || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)")))
        return;
    gboolean active;
    g_variant_get(parameters, "(b)", &active);
    s->generation++; /* An old GetActive reply must not undo a newer lock signal. */
    bt_agent_set_locked(s->agent, active);
    if (active)
        bt_client_scan(s->client, NULL);
}

static void
appeared(GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
    Session *s = data;
    g_free(s->owner);
    s->owner = g_strdup(owner);
    query_active(s);
}

static void
vanished(GDBusConnection *bus, const char *name, gpointer data)
{
    Session *s = data;
    g_clear_pointer(&s->owner, g_free);
    query_active(s);
}

static void
sleep_changed(GDBusConnection *bus, const char *sender, const char *path,
              const char *interface, const char *signal, GVariant *parameters, gpointer data)
{
    Session *s = data;
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)")))
        return;
    gboolean sleeping;
    g_variant_get(parameters, "(b)", &sleeping);
    if (sleeping) {
        s->generation++;
        bt_agent_set_locked(s->agent, TRUE);
        bt_client_scan(s->client, NULL);
    } else {
        query_active(s);
    }
}

Session *
session_new(GDBusConnection *bus, BtClient *client, BtAgent *agent)
{
    Session *s = g_new0(Session, 1);
    s->refs = 1;
    s->bus = g_object_ref(bus);
    s->client = g_object_ref(client);
    s->agent = g_object_ref(agent);
    s->cancel = g_cancellable_new();
    s->active_subscription = g_dbus_connection_signal_subscribe(bus, SCREENSAVER, SCREENSAVER,
        "ActiveChanged", SCREENSAVER_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, active_changed, s, NULL);
    s->watch = g_bus_watch_name_on_connection(bus, SCREENSAVER, G_BUS_NAME_WATCHER_FLAGS_NONE, appeared, vanished, s, NULL);
    s->sleep_subscription = g_dbus_connection_signal_subscribe(bt_client_connection(client),
        "org.freedesktop.login1", "org.freedesktop.login1.Manager", "PrepareForSleep",
        "/org/freedesktop/login1", NULL, G_DBUS_SIGNAL_FLAGS_NONE, sleep_changed, s, NULL);
    return s;
}

void
session_free(Session *s)
{
    if (!s)
        return;
    s->stopped = TRUE;
    g_cancellable_cancel(s->cancel);
    g_bus_unwatch_name(s->watch);
    g_dbus_connection_signal_unsubscribe(s->bus, s->active_subscription);
    g_dbus_connection_signal_unsubscribe(bt_client_connection(s->client), s->sleep_subscription);
    session_unref(s);
}
