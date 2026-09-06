/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "agent.h"

#define ADAPTER "/org/bluez/hci0"
#define DEVICE ADAPTER "/dev_AA_BB_CC_DD_EE_FF"
#define AGENT "/io/github/the_specter_x/Mytooth/agent"
#define MANAGER "org.bluez.AgentManager1"

typedef struct {
    GTestDBus *bus;
    GDBusConnection *service;
    GDBusConnection *connection;
    BtClient *client;
    BtAgent *agent;
    GDBusNodeInfo *info;
    GHashTable *adapter;
    GHashTable *device;
    GHashTable *battery;
    GDBusMethodInvocation *pending_pair;
    GDBusMethodInvocation *pending_scan;
    char *agent_sender;
    gboolean device_present;
    gboolean hold_pair;
    gboolean hold_scan;
    gboolean fail_connect;
    guint starts;
    guint stops;
    guint operations;
    guint errors;
} Fixture;

static const char xml[] =
    "<node><interface name='org.freedesktop.DBus.ObjectManager'>"
    "<method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method>"
    "<signal name='InterfacesAdded'><arg type='o'/><arg type='a{sa{sv}}'/></signal>"
    "<signal name='InterfacesRemoved'><arg type='o'/><arg type='as'/></signal></interface>"
    "<interface name='org.bluez.AgentManager1'>"
    "<method name='RegisterAgent'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='RequestDefaultAgent'><arg type='o' direction='in'/></method>"
    "<method name='UnregisterAgent'><arg type='o' direction='in'/></method></interface>"
    "<interface name='org.bluez.Adapter1'>"
    "<method name='StartDiscovery'/><method name='StopDiscovery'/>"
    "<method name='RemoveDevice'><arg type='o' direction='in'/></method>"
    "<property name='Powered' type='b' access='readwrite'/>"
    "<property name='Discovering' type='b' access='read'/>"
    "<property name='Discoverable' type='b' access='readwrite'/>"
    "<property name='DiscoverableTimeout' type='u' access='readwrite'/>"
    "<property name='Alias' type='s' access='readwrite'/>"
    "<property name='Address' type='s' access='read'/></interface>"
    "<interface name='org.bluez.Device1'>"
    "<method name='Pair'/><method name='CancelPairing'/><method name='Connect'/><method name='Disconnect'/>"
    "<property name='Paired' type='b' access='read'/>"
    "<property name='Connected' type='b' access='read'/>"
    "<property name='Trusted' type='b' access='readwrite'/>"
    "<property name='Blocked' type='b' access='readwrite'/>"
    "<property name='Alias' type='s' access='readwrite'/>"
    "<property name='Address' type='s' access='read'/>"
    "<property name='Adapter' type='o' access='read'/></interface>"
    "<interface name='org.bluez.Battery1'><property name='Percentage' type='y' access='read'/></interface></node>";

static void
iterate_until(gboolean (*predicate)(Fixture *), Fixture *f)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!predicate(f) && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
    g_assert_true(predicate(f));
}

static void
put(GHashTable *table, const char *key, GVariant *value)
{
    g_hash_table_replace(table, g_strdup(key), g_variant_ref_sink(value));
}

static GHashTable *
props(Fixture *f, const char *interface)
{
    if (g_str_equal(interface, BT_ADAPTER)) return f->adapter;
    if (g_str_equal(interface, BT_DEVICE)) return f->device;
    if (g_str_equal(interface, BT_BATTERY)) return f->battery;
    return NULL;
}

static GVariant *
dictionary(GHashTable *table)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    if (table) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, table);
        while (g_hash_table_iter_next(&iter, &key, &value))
            g_variant_builder_add(&builder, "{sv}", (const char *)key, (GVariant *)value);
    }
    return g_variant_builder_end(&builder);
}

static GVariant *
device_interfaces(Fixture *f)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(&builder, "{s@a{sv}}", BT_DEVICE, dictionary(f->device));
    g_variant_builder_add(&builder, "{s@a{sv}}", BT_BATTERY, dictionary(f->battery));
    return g_variant_builder_end(&builder);
}

static void
change(Fixture *f, const char *path, const char *interface, const char *property, GVariant *value)
{
    put(props(f, interface), property, value);
    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&changed, "{sv}", property, value);
    g_dbus_connection_emit_signal(f->service, NULL, path, "org.freedesktop.DBus.Properties",
        "PropertiesChanged", g_variant_new("(sa{sv}as)", interface, &changed, NULL), NULL);
}

static void
remove_device(Fixture *f)
{
    f->device_present = FALSE;
    const char *interfaces[] = { BT_DEVICE, BT_BATTERY, NULL };
    g_dbus_connection_emit_signal(f->service, NULL, "/", "org.freedesktop.DBus.ObjectManager",
        "InterfacesRemoved", g_variant_new("(o^as)", DEVICE, interfaces), NULL);
}

static void
service_call(GDBusConnection *connection, const char *sender, const char *path,
             const char *interface, const char *method, GVariant *parameters,
             GDBusMethodInvocation *invocation, gpointer data)
{
    Fixture *f = data;
    if (g_str_equal(method, "GetManagedObjects")) {
        GVariantBuilder objects, interfaces;
        g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
        g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_add(&interfaces, "{s@a{sv}}", BT_ADAPTER, dictionary(f->adapter));
        g_variant_builder_add(&objects, "{oa{sa{sv}}}", ADAPTER, &interfaces);
        g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_add(&interfaces, "{s@a{sv}}", MANAGER, dictionary(NULL));
        g_variant_builder_add(&objects, "{oa{sa{sv}}}", "/org/bluez", &interfaces);
        if (f->device_present)
            g_variant_builder_add(&objects, "{o@a{sa{sv}}}", DEVICE, device_interfaces(f));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{oa{sa{sv}}})", &objects));
        return;
    }
    if (g_str_equal(method, "RegisterAgent")) {
        const char *agent_path, *capability;
        g_variant_get(parameters, "(&o&s)", &agent_path, &capability);
        g_assert_cmpstr(agent_path, ==, AGENT);
        g_assert_cmpstr(capability, ==, "KeyboardDisplay");
        g_free(f->agent_sender);
        f->agent_sender = g_strdup(sender);
    } else if (g_str_equal(method, "Pair")) {
        if (f->hold_pair) {
            f->pending_pair = g_object_ref(invocation);
            return;
        }
        change(f, DEVICE, BT_DEVICE, "Paired", g_variant_new_boolean(TRUE));
    } else if (g_str_equal(method, "CancelPairing")) {
        if (f->pending_pair) {
            g_dbus_method_invocation_return_dbus_error(f->pending_pair, "org.bluez.Error.AuthenticationCanceled", "Cancelled");
            g_clear_object(&f->pending_pair);
        }
    } else if (g_str_equal(method, "Connect")) {
        if (f->fail_connect) {
            g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.ConnectionAttemptFailed", "No response");
            return;
        }
        change(f, DEVICE, BT_DEVICE, "Connected", g_variant_new_boolean(TRUE));
    } else if (g_str_equal(method, "Disconnect")) {
        change(f, DEVICE, BT_DEVICE, "Connected", g_variant_new_boolean(FALSE));
    } else if (g_str_equal(method, "StartDiscovery")) {
        f->starts++;
        if (f->hold_scan) {
            f->pending_scan = g_object_ref(invocation);
            return;
        }
        change(f, ADAPTER, BT_ADAPTER, "Discovering", g_variant_new_boolean(TRUE));
    } else if (g_str_equal(method, "StopDiscovery")) {
        f->stops++;
        change(f, ADAPTER, BT_ADAPTER, "Discovering", g_variant_new_boolean(FALSE));
    } else if (g_str_equal(method, "RemoveDevice")) {
        remove_device(f);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *
get_property(GDBusConnection *connection, const char *sender, const char *path,
             const char *interface, const char *property, GError **error, gpointer data)
{
    GHashTable *table = props(data, interface);
    GVariant *value = table ? g_hash_table_lookup(table, property) : NULL;
    return value ? g_variant_ref(value) : NULL;
}

static gboolean
set_property(GDBusConnection *connection, const char *sender, const char *path,
             const char *interface, const char *property, GVariant *value, GError **error, gpointer data)
{
    change(data, path, interface, property, value);
    return TRUE;
}

static const GDBusInterfaceVTable vtable = {
    .method_call = service_call, .get_property = get_property, .set_property = set_property
};

static void
own_name(Fixture *f, gboolean acquire)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(f->service,
        "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        acquire ? "RequestName" : "ReleaseName",
        acquire ? g_variant_new("(su)", "org.bluez", 0u) : g_variant_new("(s)", "org.bluez"),
        NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    g_assert_no_error(error);
}

static gboolean ready(Fixture *f) { return bt_agent_is_default(f->agent); }
static gboolean unavailable(Fixture *f) { return bt_client_owner(f->client) == NULL; }
static gboolean idle(Fixture *f) { return !bt_client_busy(f->client, DEVICE) && !bt_client_busy(f->client, ADAPTER); }
static gboolean pairing(Fixture *f) { return f->pending_pair != NULL; }
static gboolean scanning(Fixture *f) { return f->pending_scan != NULL; }
static gboolean stopped_scan(Fixture *f) { return f->stops > 0; }
static gboolean no_device(Fixture *f) { g_autoptr(GDBusProxy) p = bt_client_proxy(f->client, DEVICE, BT_DEVICE); return !p; }
static gboolean has_device(Fixture *f) { g_autoptr(GDBusProxy) p = bt_client_proxy(f->client, DEVICE, BT_DEVICE); return p != NULL; }
static void error_signal(BtClient *client, const char *message, gpointer data) { ((Fixture *)data)->errors++; }
static void operation_signal(BtClient *client, const char *path, const char *method, gboolean success, gpointer data) { ((Fixture *)data)->operations++; }

static void
setup(Fixture *f, gconstpointer data)
{
    f->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(f->bus);
    g_autoptr(GError) error = NULL;
    GDBusConnectionFlags flags = G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION;
    f->service = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(f->bus), flags, NULL, NULL, &error);
    g_assert_no_error(error);
    f->connection = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(f->bus), flags, NULL, NULL, &error);
    g_assert_no_error(error);
    f->adapter = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
    f->device = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
    f->battery = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
    put(f->adapter, "Powered", g_variant_new_boolean(TRUE));
    put(f->adapter, "Discovering", g_variant_new_boolean(FALSE));
    put(f->adapter, "Discoverable", g_variant_new_boolean(FALSE));
    put(f->adapter, "DiscoverableTimeout", g_variant_new_uint32(0));
    put(f->adapter, "Alias", g_variant_new_string("Test adapter"));
    put(f->adapter, "Address", g_variant_new_string("11:22:33:44:55:66"));
    put(f->device, "Alias", g_variant_new_string("<b>Untrusted device name</b>"));
    put(f->device, "Address", g_variant_new_string("AA:BB:CC:DD:EE:FF"));
    put(f->device, "Adapter", g_variant_new_object_path(ADAPTER));
    const char *booleans[] = { "Paired", "Connected", "Trusted", "Blocked" };
    for (guint i = 0; i < G_N_ELEMENTS(booleans); i++)
        put(f->device, booleans[i], g_variant_new_boolean(FALSE));
    put(f->battery, "Percentage", g_variant_new_byte(73));
    f->device_present = TRUE;
    f->info = g_dbus_node_info_new_for_xml(xml, &error);
    g_assert_no_error(error);
    const char *paths[] = { "/", "/org/bluez", ADAPTER, DEVICE, DEVICE };
    for (guint i = 0; i < G_N_ELEMENTS(paths); i++) {
        guint registration = g_dbus_connection_register_object(f->service, paths[i], f->info->interfaces[i], &vtable, f, NULL, &error);
        g_assert_no_error(error);
        g_assert_cmpuint(registration, >, 0);
    }
    own_name(f, TRUE);
    f->client = bt_client_new(f->connection);
    f->agent = bt_agent_new(f->client);
    bt_agent_set_locked(f->agent, FALSE);
    g_signal_connect(f->client, "error", G_CALLBACK(error_signal), f);
    g_signal_connect(f->client, "operation", G_CALLBACK(operation_signal), f);
    iterate_until(ready, f);
}

static void
teardown(Fixture *f, gconstpointer data)
{
    bt_agent_stop(f->agent);
    bt_client_stop(f->client);
    if (f->pending_pair) {
        g_dbus_method_invocation_return_dbus_error(f->pending_pair, "org.bluez.Error.Canceled", "test ended");
        g_clear_object(&f->pending_pair);
    }
    if (f->pending_scan) {
        g_dbus_method_invocation_return_value(f->pending_scan, NULL);
        g_clear_object(&f->pending_scan);
    }
    g_dbus_connection_close_sync(f->connection, NULL, NULL);
    g_dbus_connection_close_sync(f->service, NULL, NULL);
    for (int i = 0; i < 30; i++) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
    g_object_unref(f->agent);
    g_object_unref(f->client);
    g_object_unref(f->connection);
    g_object_unref(f->service);
    g_dbus_node_info_unref(f->info);
    g_hash_table_unref(f->adapter);
    g_hash_table_unref(f->device);
    g_hash_table_unref(f->battery);
    g_free(f->agent_sender);
    g_test_dbus_down(f->bus);
    g_object_unref(f->bus);
}

static void
test_model(Fixture *f, gconstpointer data)
{
    g_autoptr(GPtrArray) adapters = bt_client_list(f->client, BT_ADAPTER);
    g_autoptr(GPtrArray) devices = bt_client_list(f->client, BT_DEVICE);
    g_assert_cmpuint(adapters->len, ==, 1);
    g_assert_cmpuint(devices->len, ==, 1);
    g_autoptr(GDBusProxy) battery = bt_client_proxy(f->client, DEVICE, BT_BATTERY);
    g_assert_cmpint(bt_battery(battery), ==, 73);
    bt_client_method(f->client, DEVICE, BT_DEVICE, "Pair", NULL);
    iterate_until(idle, f);
    g_assert_true(bt_boolean(g_ptr_array_index(devices, 0), "Paired"));
    g_assert_false(bt_boolean(g_ptr_array_index(devices, 0), "Trusted"));
    bt_client_method(f->client, DEVICE, BT_DEVICE, "Connect", NULL);
    iterate_until(idle, f);
    g_assert_true(bt_boolean(g_ptr_array_index(devices, 0), "Connected"));
    bt_client_method(f->client, DEVICE, BT_DEVICE, "Disconnect", NULL);
    iterate_until(idle, f);
    g_assert_false(bt_boolean(g_ptr_array_index(devices, 0), "Connected"));
    bt_client_set(f->client, DEVICE, BT_DEVICE, "Trusted", g_variant_new_boolean(TRUE));
    iterate_until(idle, f);
    g_assert_true(bt_boolean(g_ptr_array_index(devices, 0), "Trusted"));
    bt_client_method(f->client, ADAPTER, BT_ADAPTER, "RemoveDevice", g_variant_new("(o)", DEVICE));
    iterate_until(no_device, f);
}

static void
test_restart(Fixture *f, gconstpointer data)
{
    f->hold_pair = TRUE;
    bt_client_method(f->client, DEVICE, BT_DEVICE, "Pair", NULL);
    iterate_until(pairing, f);
    own_name(f, FALSE);
    iterate_until(unavailable, f);
    g_assert_false(bt_client_busy(f->client, DEVICE));
    g_autoptr(GPtrArray) devices = bt_client_list(f->client, BT_DEVICE);
    g_assert_cmpuint(devices->len, ==, 0);
    own_name(f, TRUE);
    iterate_until(ready, f);
    g_dbus_method_invocation_return_value(f->pending_pair, NULL);
    g_clear_object(&f->pending_pair);
    bt_client_method(f->client, DEVICE, BT_DEVICE, "Connect", NULL);
    iterate_until(idle, f);
    g_assert_cmpuint(f->operations, ==, 1); /* Only the new Connect completion is emitted. */
}

static void
test_cancel(Fixture *f, gconstpointer data)
{
    f->hold_pair = TRUE;
    bt_client_method(f->client, DEVICE, BT_DEVICE, "Pair", NULL);
    iterate_until(pairing, f);
    bt_client_method(f->client, DEVICE, BT_DEVICE, "Connect", NULL);
    g_assert_cmpuint(f->errors, ==, 1); /* Duplicate operation prevented. */
    bt_client_method(f->client, DEVICE, BT_DEVICE, "CancelPairing", NULL);
    iterate_until(idle, f);
    g_assert_null(f->pending_pair);
}

static void
test_scan_race(Fixture *f, gconstpointer data)
{
    f->hold_scan = TRUE;
    bt_client_scan(f->client, ADAPTER);
    iterate_until(scanning, f);
    bt_client_scan(f->client, NULL); /* User closes Add Device before StartDiscovery returns. */
    g_dbus_method_invocation_return_value(f->pending_scan, NULL);
    g_clear_object(&f->pending_scan);
    iterate_until(stopped_scan, f);
    g_assert_cmpuint(f->starts, ==, 1);
    g_assert_cmpuint(f->stops, ==, 1);
    g_assert_null(bt_client_scan_path(f->client));
}

typedef struct { gboolean done; GVariant *reply; GError *error; } Answer;
static void
answer_received(GObject *source, GAsyncResult *result, gpointer data)
{
    Answer *answer = data;
    answer->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &answer->error);
    answer->done = TRUE;
}

static void
await_answer(Answer *answer)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!answer->done && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
    g_assert_true(answer->done);
}

static gboolean prompted(Fixture *f) { return bt_agent_prompt(f->agent) != BT_PROMPT_NONE; }

static void
test_agent(Fixture *f, gconstpointer data)
{
    const char *method = data;
    Answer answer = {0};
    GVariant *args = g_str_equal(method, "RequestConfirmation") ? g_variant_new("(ou)", DEVICE, 123u) :
        g_str_equal(method, "AuthorizeService") ? g_variant_new("(os)", DEVICE, "0000110b-0000-1000-8000-00805f9b34fb") :
        g_variant_new("(o)", DEVICE);
    g_dbus_connection_call(f->service, f->agent_sender, AGENT, "org.bluez.Agent1", method,
        args, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, answer_received, &answer);
    iterate_until(prompted, f);
    if (g_str_equal(method, "RequestPinCode")) {
        g_assert_false(bt_agent_answer(f->agent, TRUE, ""));
        g_assert_true(bt_agent_answer(f->agent, TRUE, "0000"));
    } else if (g_str_equal(method, "RequestPasskey")) {
        g_assert_false(bt_agent_answer(f->agent, TRUE, "-1"));
        g_assert_true(bt_agent_answer(f->agent, TRUE, "000123"));
    } else {
        g_assert_true(bt_agent_answer(f->agent, TRUE, NULL));
    }
    await_answer(&answer);
    g_assert_no_error(answer.error);
    g_assert_nonnull(answer.reply);
    if (g_str_equal(method, "RequestPasskey")) {
        guint32 value;
        g_variant_get(answer.reply, "(u)", &value);
        g_assert_cmpuint(value, ==, 123);
    }
    g_variant_unref(answer.reply);
}

static void
test_lock(Fixture *f, gconstpointer data)
{
    Answer answer = {0};
    g_dbus_connection_call(f->service, f->agent_sender, AGENT, "org.bluez.Agent1", "RequestAuthorization",
        g_variant_new("(o)", DEVICE), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, answer_received, &answer);
    iterate_until(prompted, f);
    bt_agent_set_locked(f->agent, TRUE);
    await_answer(&answer);
    g_assert_null(answer.reply);
    g_autofree char *remote = g_dbus_error_get_remote_error(answer.error);
    g_assert_cmpstr(remote, ==, "org.bluez.Error.Rejected");
    g_clear_error(&answer.error);
    g_assert_cmpint(bt_agent_prompt(f->agent), ==, BT_PROMPT_NONE);
}

static void
test_spoof(Fixture *f, gconstpointer data)
{
    Answer answer = {0};
    g_dbus_connection_call(f->connection, f->agent_sender, AGENT, "org.bluez.Agent1", "RequestAuthorization",
        g_variant_new("(o)", DEVICE), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, answer_received, &answer);
    await_answer(&answer);
    g_assert_null(answer.reply);
    g_autofree char *remote = g_dbus_error_get_remote_error(answer.error);
    g_assert_cmpstr(remote, ==, "org.bluez.Error.Rejected");
    g_clear_error(&answer.error);
    g_assert_cmpint(bt_agent_prompt(f->agent), ==, BT_PROMPT_NONE);
}

static void
test_display(Fixture *f, gconstpointer data)
{
    for (guint16 entered = 0; entered < 7; entered++) {
        Answer answer = {0};
        g_dbus_connection_call(f->service, f->agent_sender, AGENT, "org.bluez.Agent1", "DisplayPasskey",
            g_variant_new("(ouq)", DEVICE, 7u, entered), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, answer_received, &answer);
        await_answer(&answer);
        g_assert_no_error(answer.error);
        g_variant_unref(answer.reply);
        g_assert_nonnull(strstr(bt_agent_text(f->agent), "000007"));
    }
    Answer cancel = {0};
    g_dbus_connection_call(f->service, f->agent_sender, AGENT, "org.bluez.Agent1", "Cancel",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, answer_received, &cancel);
    await_answer(&cancel);
    g_assert_no_error(cancel.error);
    g_variant_unref(cancel.reply);
    g_assert_cmpint(bt_agent_prompt(f->agent), ==, BT_PROMPT_NONE);
}

static void
test_hotplug(Fixture *f, gconstpointer data)
{
    remove_device(f);
    iterate_until(no_device, f);
    f->device_present = TRUE;
    g_dbus_connection_emit_signal(f->service, NULL, "/", "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded", g_variant_new("(o@a{sa{sv}})", DEVICE, device_interfaces(f)), NULL);
    iterate_until(has_device, f);
}

static gboolean visible(Fixture *f) { g_autoptr(GDBusProxy) p = bt_client_proxy(f->client, ADAPTER, BT_ADAPTER); return bt_boolean(p, "Discoverable"); }

static void
test_visibility(Fixture *f, gconstpointer data)
{
    bt_client_discoverable(f->client, ADAPTER, TRUE);
    iterate_until(visible, f);
    g_assert_cmpuint(g_variant_get_uint32(g_hash_table_lookup(f->adapter, "DiscoverableTimeout")), ==, 120);
}

static void
test_connection_failure(Fixture *f, gconstpointer data)
{
    f->fail_connect = TRUE;
    bt_client_method(f->client, DEVICE, BT_DEVICE, "Connect", NULL);
    iterate_until(idle, f);
    g_assert_cmpuint(f->errors, ==, 1);
    g_assert_false(g_variant_get_boolean(g_hash_table_lookup(f->device, "Connected")));
    f->fail_connect = FALSE;
    bt_client_method(f->client, DEVICE, BT_DEVICE, "Connect", NULL);
    iterate_until(idle, f);
    g_assert_true(g_variant_get_boolean(g_hash_table_lookup(f->device, "Connected")));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/bluez/model", Fixture, NULL, setup, test_model, teardown);
    g_test_add("/bluez/restart-pending-operation", Fixture, NULL, setup, test_restart, teardown);
    g_test_add("/bluez/cancel-pairing", Fixture, NULL, setup, test_cancel, teardown);
    g_test_add("/bluez/scan-start-stop-race", Fixture, NULL, setup, test_scan_race, teardown);
    g_test_add("/bluez/hotplug", Fixture, NULL, setup, test_hotplug, teardown);
    g_test_add("/bluez/visibility-timeout", Fixture, NULL, setup, test_visibility, teardown);
    g_test_add("/bluez/connection-failure-retry", Fixture, NULL, setup, test_connection_failure, teardown);
    g_test_add("/agent/pin", Fixture, "RequestPinCode", setup, test_agent, teardown);
    g_test_add("/agent/passkey", Fixture, "RequestPasskey", setup, test_agent, teardown);
    g_test_add("/agent/confirmation", Fixture, "RequestConfirmation", setup, test_agent, teardown);
    g_test_add("/agent/authorization", Fixture, "RequestAuthorization", setup, test_agent, teardown);
    g_test_add("/agent/service", Fixture, "AuthorizeService", setup, test_agent, teardown);
    g_test_add("/agent/display-progress-cancel", Fixture, NULL, setup, test_display, teardown);
    g_test_add("/agent/lock-rejects-prompt", Fixture, NULL, setup, test_lock, teardown);
    g_test_add("/agent/reject-spoofed-sender", Fixture, NULL, setup, test_spoof, teardown);
    return g_test_run();
}
