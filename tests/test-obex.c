/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "obex.h"
#include <glib/gstdio.h>

#define ROOT "/org/bluez/obex"
#define SESSION ROOT "/client/session0"
#define TRANSFER SESSION "/transfer0"
#define TRANSFER_INTERFACE "org.bluez.obex.Transfer1"

typedef struct {
    GTestDBus *test_bus;
    GDBusConnection *service;
    GDBusConnection *application;
    GDBusNodeInfo *info;
    ObexClient *client;
    GHashTable *transfer;
    char *agent_sender;
    char *agent_path;
    char *directory;
    guint sends;
    guint cancels;
    guint removes;
    guint completed;
} Fixture;

typedef struct { gboolean done; GVariant *reply; GError *error; } Answer;

static const char xml[] =
    "<node><interface name='org.bluez.obex.AgentManager1'>"
    "<method name='RegisterAgent'><arg type='o' direction='in'/></method>"
    "<method name='UnregisterAgent'><arg type='o' direction='in'/></method></interface>"
    "<interface name='org.bluez.obex.Client1'>"
    "<method name='CreateSession'><arg type='s' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
    "<method name='RemoveSession'><arg type='o' direction='in'/></method></interface>"
    "<interface name='org.bluez.obex.ObjectPush1'>"
    "<method name='SendFile'><arg type='s' direction='in'/><arg type='o' direction='out'/><arg type='a{sv}' direction='out'/></method></interface>"
    "<interface name='org.bluez.obex.Transfer1'><method name='Cancel'/>"
    "<property name='Status' type='s' access='read'/><property name='Name' type='s' access='read'/>"
    "<property name='Size' type='t' access='read'/><property name='Transferred' type='t' access='read'/></interface></node>";

static void
put(Fixture *fixture, const char *name, GVariant *value)
{
    g_hash_table_replace(fixture->transfer, g_strdup(name), g_variant_ref_sink(value));
}

static GVariant *
properties(Fixture *fixture)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, fixture->transfer);
    while (g_hash_table_iter_next(&iter, &key, &value))
        g_variant_builder_add(&builder, "{sv}", (const char *)key, (GVariant *)value);
    return g_variant_builder_end(&builder);
}

static void
change(Fixture *fixture, const char *name, GVariant *value)
{
    put(fixture, name, value);
    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&changed, "{sv}", name, value);
    g_dbus_connection_emit_signal(fixture->service, NULL, TRANSFER,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(sa{sv}as)", TRANSFER_INTERFACE, &changed, NULL), NULL);
}

static void
service_call(GDBusConnection *connection, const char *sender, const char *path,
             const char *interface, const char *method, GVariant *parameters,
             GDBusMethodInvocation *invocation, gpointer data)
{
    Fixture *fixture = data;
    if (g_str_equal(method, "RegisterAgent")) {
        const char *agent;
        g_variant_get(parameters, "(&o)", &agent);
        g_free(fixture->agent_sender);
        g_free(fixture->agent_path);
        fixture->agent_sender = g_strdup(sender);
        fixture->agent_path = g_strdup(agent);
    } else if (g_str_equal(method, "CreateSession")) {
        const char *address;
        g_autoptr(GVariant) options = NULL;
        g_variant_get(parameters, "(&s@a{sv})", &address, &options);
        const char *target = NULL;
        g_variant_lookup(options, "Target", "&s", &target);
        g_assert_cmpstr(address, ==, "AA:BB:CC:DD:EE:FF");
        g_assert_cmpstr(target, ==, "opp");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", SESSION));
        return;
    } else if (g_str_equal(method, "RemoveSession")) {
        fixture->removes++;
    } else if (g_str_equal(method, "SendFile")) {
        fixture->sends++;
        put(fixture, "Status", g_variant_new_string("active"));
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(o@a{sv})", TRANSFER, properties(fixture)));
        return;
    } else if (g_str_equal(method, "Cancel")) {
        fixture->cancels++;
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *
get_property(GDBusConnection *connection, const char *sender, const char *path,
             const char *interface, const char *property, GError **error, gpointer data)
{
    GVariant *value = g_hash_table_lookup(((Fixture *)data)->transfer, property);
    return value ? g_variant_ref(value) : NULL;
}

static const GDBusInterfaceVTable vtable = {
    .method_call = service_call, .get_property = get_property
};

static void
own_name(Fixture *fixture)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(fixture->service,
        "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "RequestName", g_variant_new("(su)", "org.bluez.obex", 0u), NULL,
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    g_assert_no_error(error);
}

static void
answer_received(GObject *source, GAsyncResult *result, gpointer data)
{
    Answer *answer = data;
    answer->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &answer->error);
    answer->done = TRUE;
}

static void
wait_until(gboolean (*predicate)(gpointer), gpointer data)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!predicate(data) && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
    g_assert_true(predicate(data));
}

static gboolean receiver_ready(gpointer data) { return obex_client_ready(((Fixture *)data)->client); }
static gboolean sent(gpointer data) {
    Fixture *fixture = data;
    return fixture->sends > 0 &&
        g_strcmp0(obex_client_status(fixture->client), "active") == 0;
}
static gboolean prompted(gpointer data) { return obex_client_has_prompt(((Fixture *)data)->client); }
static gboolean finished(gpointer data) { return ((Fixture *)data)->completed > 0; }
static gboolean removed(gpointer data) { return ((Fixture *)data)->removes > 0; }
static gboolean answered(gpointer data) { return ((Answer *)data)->done; }
static gboolean cancelled(gpointer data) { Fixture *f = data; return f->cancels > 0 && f->removes > 0; }

static void
completed(ObexClient *client, gboolean incoming, const char *name, gpointer data)
{
    ((Fixture *)data)->completed++;
}

static void
setup(Fixture *fixture, gconstpointer data)
{
    fixture->test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(fixture->test_bus);
    GDBusConnectionFlags flags = G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                 G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION;
    g_autoptr(GError) error = NULL;
    fixture->service = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(fixture->test_bus), flags, NULL, NULL, &error);
    g_assert_no_error(error);
    fixture->application = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(fixture->test_bus), flags, NULL, NULL, &error);
    g_assert_no_error(error);
    fixture->transfer = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify)g_variant_unref);
    put(fixture, "Status", g_variant_new_string("queued"));
    put(fixture, "Name", g_variant_new_string("photo.jpg"));
    put(fixture, "Size", g_variant_new_uint64(4));
    put(fixture, "Transferred", g_variant_new_uint64(0));
    fixture->info = g_dbus_node_info_new_for_xml(xml, &error);
    g_assert_no_error(error);
    const char *paths[] = { ROOT, ROOT, SESSION, TRANSFER };
    for (guint i = 0; i < G_N_ELEMENTS(paths); i++) {
        guint id = g_dbus_connection_register_object(fixture->service, paths[i],
            fixture->info->interfaces[i], &vtable, fixture, NULL, &error);
        g_assert_no_error(error);
        g_assert_cmpuint(id, >, 0);
    }
    own_name(fixture);
    fixture->directory = g_dir_make_tmp("mytooth-obex-XXXXXX", &error);
    g_assert_no_error(error);
    fixture->client = obex_client_new(fixture->application);
    obex_client_set_receive_directory(fixture->client, fixture->directory);
    obex_client_set_locked(fixture->client, FALSE);
    g_signal_connect(fixture->client, "completed", G_CALLBACK(completed), fixture);
    wait_until(receiver_ready, fixture);
}

static void
teardown(Fixture *fixture, gconstpointer data)
{
    obex_client_stop(fixture->client);
    g_dbus_connection_close_sync(fixture->application, NULL, NULL);
    g_dbus_connection_close_sync(fixture->service, NULL, NULL);
    for (guint i = 0; i < 30; i++) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
    g_object_unref(fixture->client);
    g_object_unref(fixture->application);
    g_object_unref(fixture->service);
    g_dbus_node_info_unref(fixture->info);
    g_hash_table_unref(fixture->transfer);
    g_free(fixture->agent_sender);
    g_free(fixture->agent_path);
    g_assert_cmpint(g_rmdir(fixture->directory), ==, 0);
    g_free(fixture->directory);
    g_test_dbus_down(fixture->test_bus);
    g_object_unref(fixture->test_bus);
}

static void
test_send(Fixture *fixture, gconstpointer data)
{
    g_autofree char *path = g_build_filename(fixture->directory, "out.txt", NULL);
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_set_contents(path, "test", 4, &error));
    g_assert_no_error(error);
    g_autoptr(GPtrArray) paths = g_ptr_array_new();
    g_ptr_array_add(paths, path);
    g_assert_true(obex_client_send(fixture->client, "AA:BB:CC:DD:EE:FF", paths, &error));
    g_assert_no_error(error);
    wait_until(sent, fixture);
    change(fixture, "Transferred", g_variant_new_uint64(4));
    change(fixture, "Status", g_variant_new_string("complete"));
    wait_until(finished, fixture);
    wait_until(removed, fixture);
    g_assert_cmpuint(fixture->removes, ==, 1);
    g_assert_cmpint(g_remove(path), ==, 0);
}

static void
test_cancel(Fixture *fixture, gconstpointer data)
{
    g_autofree char *path = g_build_filename(fixture->directory, "out.txt", NULL);
    g_assert_true(g_file_set_contents(path, "test", 4, NULL));
    g_autoptr(GPtrArray) paths = g_ptr_array_new();
    g_ptr_array_add(paths, path);
    g_assert_true(obex_client_send(fixture->client, "AA:BB:CC:DD:EE:FF", paths, NULL));
    wait_until(sent, fixture);
    obex_client_cancel(fixture->client);
    wait_until(cancelled, fixture);
    g_assert_false(obex_client_busy(fixture->client));
    g_assert_cmpint(g_remove(path), ==, 0);
}

static void
test_receive(Fixture *fixture, gconstpointer data)
{
    Answer answer = {0};
    g_dbus_connection_call(fixture->service, fixture->agent_sender, fixture->agent_path,
        "org.bluez.obex.Agent1", "AuthorizePush", g_variant_new("(o)", TRANSFER),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, answer_received, &answer);
    wait_until(prompted, fixture);
    g_assert_cmpstr(obex_client_prompt_name(fixture->client), ==, "photo.jpg");
    obex_client_answer(fixture->client, TRUE);
    wait_until(answered, &answer);
    g_assert_no_error(answer.error);
    const char *destination;
    g_variant_get(answer.reply, "(&s)", &destination);
    g_autofree char *parent = g_path_get_dirname(destination);
    g_assert_cmpstr(parent, ==, fixture->directory);
    g_variant_unref(answer.reply);
    change(fixture, "Transferred", g_variant_new_uint64(4));
    change(fixture, "Status", g_variant_new_string("complete"));
    wait_until(finished, fixture);
}

static void
test_locked_and_spoofed(Fixture *fixture, gconstpointer data)
{
    obex_client_set_locked(fixture->client, TRUE);
    Answer locked = {0};
    g_dbus_connection_call(fixture->service, fixture->agent_sender, fixture->agent_path,
        "org.bluez.obex.Agent1", "AuthorizePush", g_variant_new("(o)", TRANSFER),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, answer_received, &locked);
    wait_until(answered, &locked);
    g_assert_null(locked.reply);
    g_clear_error(&locked.error);
    obex_client_set_locked(fixture->client, FALSE);
    Answer spoofed = {0};
    g_dbus_connection_call(fixture->application, fixture->agent_sender, fixture->agent_path,
        "org.bluez.obex.Agent1", "AuthorizePush", g_variant_new("(o)", TRANSFER),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, answer_received, &spoofed);
    wait_until(answered, &spoofed);
    g_assert_null(spoofed.reply);
    g_clear_error(&spoofed.error);
    g_assert_false(obex_client_has_prompt(fixture->client));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/obex/send", Fixture, NULL, setup, test_send, teardown);
    g_test_add("/obex/cancel", Fixture, NULL, setup, test_cancel, teardown);
    g_test_add("/obex/receive-approval", Fixture, NULL, setup, test_receive, teardown);
    g_test_add("/obex/reject-locked-and-spoofed", Fixture, NULL, setup, test_locked_and_spoofed, teardown);
    return g_test_run();
}
