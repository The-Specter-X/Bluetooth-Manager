/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bluetooth.h"
#include <string.h>

#define NM_SERVICE "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define NM_MANAGER "org.freedesktop.NetworkManager"
#define NM_DEVICE "org.freedesktop.NetworkManager.Device"
#define DBUS_PROPERTIES "org.freedesktop.DBus.Properties"

struct _BtClient {
    GObject parent_instance;
    GDBusConnection *connection;
    GDBusObjectManager *manager;
    GCancellable *cancel;
    GHashTable *pending; /* path -> method; one foreground operation per object */
    char *owner;
    char *network_owner;
    char *pan_pending;
    char *pan_method;
    char *scan_wanted;
    char *scan_active;
    gboolean scan_pending;
    gboolean stopped;
    guint generation;
    guint network_generation;
    guint network_watch;
    guint scan_timeout;
    GCancellable *network_cancel;
};

G_DEFINE_TYPE(BtClient, bt_client, G_TYPE_OBJECT)
enum { CHANGED, ERROR, OPERATION, N_SIGNALS };
static guint signals[N_SIGNALS];

typedef enum { NORMAL, SCAN_START, SCAN_STOP, DISCOVERABLE_TIMEOUT } CallKind;
typedef struct {
    BtClient *client;
    char *path;
    char *method;
    guint generation;
    CallKind kind;
} Call;

typedef struct {
    BtClient *client;
    char *bluez_path;
    char *address;
    char **devices;
    guint device_index;
    guint generation;
    gboolean connect;
} PanCall;

static void reconcile_scan(BtClient *self);
static void pan_next_device(PanCall *call);

static void
changed(BtClient *self)
{
    if (!self->stopped)
        g_signal_emit(self, signals[CHANGED], 0);
}

void
bt_client_error(BtClient *self, const char *message)
{
    if (!self->stopped)
        g_signal_emit(self, signals[ERROR], 0, message);
}

static void
call_free(Call *call)
{
    g_object_unref(call->client);
    g_free(call->path);
    g_free(call->method);
    g_free(call);
}

static void
call_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Call *call = user_data;
    BtClient *self = call->client;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (self->stopped || call->generation != self->generation) {
        call_free(call);
        return;
    }
    gboolean success = reply != NULL;
    if (call->kind == SCAN_START || call->kind == SCAN_STOP) {
        self->scan_pending = FALSE;
        if (call->kind == SCAN_START && success)
            self->scan_active = g_strdup(call->path);
        if (call->kind == SCAN_START && !success && g_strcmp0(self->scan_wanted, call->path) == 0)
            g_clear_pointer(&self->scan_wanted, g_free);
        if (call->kind == SCAN_STOP) {
            g_clear_pointer(&self->scan_active, g_free);
            /* Do not loop endlessly on a daemon error. */
            if (!success)
                g_clear_pointer(&self->scan_wanted, g_free);
        }
    } else if (!g_str_equal(call->method, "CancelPairing")) {
        g_hash_table_remove(self->pending, call->path);
    }
    if (!success && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_autofree char *message = bt_error_message(error);
        bt_client_error(self, message);
        g_autofree char *remote = g_dbus_error_get_remote_error(error);
        g_debug("Bluetooth %s failed (%s)", call->method, remote ? remote : "transport error");
        if (g_str_equal(call->method, "Pair") &&
            (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
             g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY)))
            bt_client_method(self, call->path, BT_DEVICE, "CancelPairing", NULL);
    }
    if (call->kind == DISCOVERABLE_TIMEOUT && success)
        bt_client_set(self, call->path, BT_ADAPTER, "Discoverable", g_variant_new_boolean(TRUE));
    changed(self);
    g_signal_emit(self, signals[OPERATION], 0, call->path, call->method, success);
    reconcile_scan(self);
    call_free(call);
}

static void
send_call_full(BtClient *self, const char *path, const char *interface,
               const char *method, const char *operation, GVariant *parameters,
               const GVariantType *reply_type, CallKind kind)
{
    /* Sink inputs even when a request cannot start. */
    g_autoptr(GVariant) args = parameters ? g_variant_ref_sink(parameters) : g_variant_ref_sink(g_variant_new("()"));
    if (self->stopped || !self->owner) {
        bt_client_error(self, "The Bluetooth service is unavailable.");
        return;
    }
    gboolean foreground = kind == NORMAL || kind == DISCOVERABLE_TIMEOUT;
    gboolean cancel_pairing = g_str_equal(method, "CancelPairing");
    if (foreground && !cancel_pairing && bt_client_busy(self, path)) {
        bt_client_error(self, "An operation is already in progress for this device or adapter.");
        return;
    }
    Call *call = g_new0(Call, 1);
    call->client = g_object_ref(self);
    call->path = g_strdup(path);
    call->method = g_strdup(operation ? operation : method);
    call->generation = self->generation;
    call->kind = kind;
    if (foreground && !cancel_pairing)
        g_hash_table_insert(self->pending, g_strdup(path), g_strdup(operation ? operation : method));
    /* Address the unique owner so queued operations never target a restarted daemon. */
    g_dbus_connection_call(self->connection, self->owner, path, interface, method,
                           args, reply_type, G_DBUS_CALL_FLAGS_NO_AUTO_START,
                           g_str_equal(method, "Pair") ? 120000 : 30000,
                           self->cancel, call_done, call);
    changed(self);
}

static void
send_call(BtClient *self, const char *path, const char *interface,
          const char *method, GVariant *parameters, CallKind kind)
{
    send_call_full(self, path, interface, method, NULL, parameters,
                   G_VARIANT_TYPE_UNIT, kind);
}

void
bt_client_method(BtClient *self, const char *path, const char *interface,
                 const char *method, GVariant *parameters)
{
    send_call(self, path, interface, method, parameters, NORMAL);
}

void
bt_client_set(BtClient *self, const char *path, const char *interface,
              const char *property, GVariant *value)
{
    send_call(self, path, "org.freedesktop.DBus.Properties", "Set",
              g_variant_new("(ssv)", interface, property, value), NORMAL);
}

void
bt_client_discoverable(BtClient *self, const char *path, gboolean enabled)
{
    if (!enabled) {
        bt_client_set(self, path, BT_ADAPTER, "Discoverable", g_variant_new_boolean(FALSE));
        return;
    }
    /* Set the daemon-side timeout first; it survives an application crash. */
    send_call(self, path, "org.freedesktop.DBus.Properties", "Set",
              g_variant_new("(ssv)", BT_ADAPTER, "DiscoverableTimeout", g_variant_new_uint32(120)),
              DISCOVERABLE_TIMEOUT);
}

static void
pan_call_free(PanCall *call)
{
    g_object_unref(call->client);
    g_free(call->bluez_path);
    g_free(call->address);
    g_strfreev(call->devices);
    g_free(call);
}

static gboolean
pan_call_current(PanCall *call)
{
    BtClient *self = call->client;
    return !self->stopped && call->generation == self->network_generation &&
        g_strcmp0(call->bluez_path, self->pan_pending) == 0;
}

static void
pan_call_finish(PanCall *call, gboolean success, const char *message, const GError *error)
{
    BtClient *self = call->client;
    if (!pan_call_current(call)) {
        pan_call_free(call);
        return;
    }
    g_hash_table_remove(self->pending, call->bluez_path);
    g_clear_pointer(&self->pan_pending, g_free);
    g_clear_pointer(&self->pan_method, g_free);
    if (!success) {
        g_autofree char *fallback = error ? bt_error_message(error) : NULL;
        bt_client_error(self, message ? message : fallback);
        if (error) {
            g_autofree char *remote = g_dbus_error_get_remote_error(error);
            g_debug("NetworkManager PAN operation failed (%s)",
                remote ? remote : "transport error");
        }
    }
    changed(self);
    g_signal_emit(self, signals[OPERATION], 0, call->bluez_path,
        call->connect ? "ConnectNetwork" : "DisconnectNetwork", success);
    pan_call_free(call);
}

static GVariant *
pan_connection_settings(const char *address)
{
    guint8 bytes[6];
    for (guint i = 0; i < G_N_ELEMENTS(bytes); i++) {
        int high = g_ascii_xdigit_value(address[i * 3]);
        int low = g_ascii_xdigit_value(address[i * 3 + 1]);
        if (high < 0 || low < 0 || (i < 5 && address[i * 3 + 2] != ':'))
            return NULL;
        bytes[i] = (guint8)((high << 4) | low);
    }

    GVariantBuilder settings, section;
    g_variant_builder_init(&settings, G_VARIANT_TYPE("a{sa{sv}}"));

    g_variant_builder_init(&section, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&section, "{sv}", "id",
        g_variant_new_string("Mytooth Bluetooth tethering"));
    g_variant_builder_add(&section, "{sv}", "type", g_variant_new_string("bluetooth"));
    g_variant_builder_add(&section, "{sv}", "autoconnect", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&settings, "{s@a{sv}}", "connection", g_variant_builder_end(&section));

    g_variant_builder_init(&section, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&section, "{sv}", "bdaddr",
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, bytes, G_N_ELEMENTS(bytes), sizeof(bytes[0])));
    g_variant_builder_add(&section, "{sv}", "type", g_variant_new_string("panu"));
    g_variant_builder_add(&settings, "{s@a{sv}}", "bluetooth", g_variant_builder_end(&section));

    g_variant_builder_init(&section, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&section, "{sv}", "method", g_variant_new_string("auto"));
    g_variant_builder_add(&settings, "{s@a{sv}}", "ipv4", g_variant_builder_end(&section));

    g_variant_builder_init(&section, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&section, "{sv}", "method", g_variant_new_string("auto"));
    g_variant_builder_add(&settings, "{s@a{sv}}", "ipv6", g_variant_builder_end(&section));
    return g_variant_ref_sink(g_variant_builder_end(&settings));
}

static void
pan_action_done(GObject *source, GAsyncResult *result, gpointer data)
{
    PanCall *call = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    pan_call_finish(call, reply != NULL, NULL, error);
}

static void
pan_start_action(PanCall *call, const char *device_path)
{
    BtClient *self = call->client;
    if (call->connect) {
        g_autoptr(GVariant) settings = pan_connection_settings(call->address);
        if (!settings) {
            pan_call_finish(call, FALSE, "The device has an invalid Bluetooth address.", NULL);
            return;
        }
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        /* A PAN started from a device action should not leave a saved profile behind. */
        g_variant_builder_add(&options, "{sv}", "persist", g_variant_new_string("volatile"));
        g_dbus_connection_call(self->connection, self->network_owner, NM_PATH, NM_MANAGER,
            "AddAndActivateConnection2",
            g_variant_new("(@a{sa{sv}}ooa{sv})", g_steal_pointer(&settings),
                device_path, "/", &options), G_VARIANT_TYPE("(ooa{sv})"),
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, self->network_cancel,
            pan_action_done, call);
    } else {
        g_dbus_connection_call(self->connection, self->network_owner, device_path, NM_DEVICE,
            "Disconnect", NULL, G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START,
            30000, self->network_cancel, pan_action_done, call);
    }
}

static void
pan_device_properties_done(GObject *source, GAsyncResult *result, gpointer data)
{
    PanCall *call = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!pan_call_current(call)) {
        pan_call_free(call);
        return;
    }
    if (!reply) {
        /* A placeholder can disappear while the list is being inspected. */
        call->device_index++;
        pan_next_device(call);
        return;
    }
    g_autoptr(GVariant) properties = NULL;
    g_variant_get(reply, "(@a{sv})", &properties);
    g_autoptr(GVariant) address = g_variant_lookup_value(properties, "HwAddress",
        G_VARIANT_TYPE_STRING);
    g_autoptr(GVariant) type = g_variant_lookup_value(properties, "DeviceType",
        G_VARIANT_TYPE_UINT32);
    if (address && type && g_variant_get_uint32(type) == 5 &&
        g_ascii_strcasecmp(g_variant_get_string(address, NULL), call->address) == 0) {
        pan_start_action(call, call->devices[call->device_index]);
        return;
    }
    call->device_index++;
    pan_next_device(call);
}

static void
pan_next_device(PanCall *call)
{
    if (!pan_call_current(call)) {
        pan_call_free(call);
        return;
    }
    if (!call->devices || !call->devices[call->device_index]) {
        pan_call_finish(call, FALSE,
            "NetworkManager has not exposed this device for Bluetooth tethering.", NULL);
        return;
    }
    BtClient *self = call->client;
    g_dbus_connection_call(self->connection, self->network_owner,
        call->devices[call->device_index], DBUS_PROPERTIES, "GetAll",
        g_variant_new("(s)", NM_DEVICE), G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000, self->network_cancel,
        pan_device_properties_done, call);
}

static void
pan_devices_done(GObject *source, GAsyncResult *result, gpointer data)
{
    PanCall *call = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!pan_call_current(call)) {
        pan_call_free(call);
        return;
    }
    if (!reply) {
        pan_call_finish(call, FALSE, NULL, error);
        return;
    }
    g_variant_get(reply, "(^ao)", &call->devices);
    pan_next_device(call);
}

void
bt_client_pan(BtClient *self, const char *device_path, gboolean connect)
{
    if (self->stopped || !self->owner || !self->network_owner) {
        bt_client_error(self, "NetworkManager is unavailable.");
        return;
    }
    if (!device_path || self->pan_pending || bt_client_busy(self, device_path)) {
        bt_client_error(self, "Another network operation is already in progress.");
        return;
    }
    g_autoptr(GDBusProxy) device = bt_client_proxy(self, device_path, BT_DEVICE);
    g_autofree char *address = bt_string(device, "Address", "");
    if (!device || strlen(address) != 17) {
        bt_client_error(self, "The Bluetooth device is no longer available.");
        return;
    }
    PanCall *call = g_new0(PanCall, 1);
    call->client = g_object_ref(self);
    call->bluez_path = g_strdup(device_path);
    call->address = g_steal_pointer(&address);
    call->generation = self->network_generation;
    call->connect = connect;
    self->pan_pending = g_strdup(device_path);
    self->pan_method = g_strdup(connect ? "ConnectNetwork" : "DisconnectNetwork");
    g_hash_table_insert(self->pending, g_strdup(device_path), g_strdup(self->pan_method));
    changed(self);
    g_dbus_connection_call(self->connection, self->network_owner, NM_PATH, NM_MANAGER,
        "GetAllDevices", NULL, G_VARIANT_TYPE("(ao)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
        10000, self->network_cancel, pan_devices_done, call);
}

gboolean
bt_client_pan_available(BtClient *self)
{
    return self->network_owner != NULL;
}

static void
reconcile_scan(BtClient *self)
{
    if (self->stopped || !self->owner || self->scan_pending)
        return;
    if (self->scan_active && g_strcmp0(self->scan_active, self->scan_wanted) != 0) {
        self->scan_pending = TRUE;
        send_call(self, self->scan_active, BT_ADAPTER, "StopDiscovery", NULL, SCAN_STOP);
    } else if (!self->scan_active && self->scan_wanted) {
        self->scan_pending = TRUE;
        send_call(self, self->scan_wanted, BT_ADAPTER, "StartDiscovery", NULL, SCAN_START);
    }
}

static gboolean
scan_expired(gpointer data)
{
    BtClient *self = data;
    self->scan_timeout = 0;
    bt_client_scan(self, NULL);
    return G_SOURCE_REMOVE;
}

void
bt_client_scan(BtClient *self, const char *adapter_path)
{
    g_free(self->scan_wanted);
    self->scan_wanted = g_strdup(adapter_path);
    if (self->scan_timeout)
        g_source_remove(self->scan_timeout);
    self->scan_timeout = adapter_path ? g_timeout_add_seconds(30, scan_expired, self) : 0;
    reconcile_scan(self);
    changed(self);
}

const char *bt_client_scan_path(BtClient *self) { return self->scan_wanted; }
GDBusConnection *bt_client_connection(BtClient *self) { return self->connection; }
const char *bt_client_owner(BtClient *self) { return self->owner; }
gboolean bt_client_busy(BtClient *self, const char *path) { return path && g_hash_table_contains(self->pending, path); }
const char *bt_client_pending(BtClient *self, const char *path) { return path ? g_hash_table_lookup(self->pending, path) : NULL; }

GDBusProxy *
bt_client_proxy(BtClient *self, const char *path, const char *interface)
{
    if (!self->manager || !self->owner || !path)
        return NULL;
    return G_DBUS_PROXY(g_dbus_object_manager_get_interface(self->manager, path, interface));
}

GPtrArray *
bt_client_list(BtClient *self, const char *interface)
{
    GPtrArray *array = g_ptr_array_new_with_free_func(g_object_unref);
    if (!self->manager || !self->owner)
        return array;
    GList *objects = g_dbus_object_manager_get_objects(self->manager);
    for (GList *l = objects; l; l = l->next) {
        GDBusInterface *proxy = g_dbus_object_get_interface(l->data, interface);
        if (proxy)
            g_ptr_array_add(array, proxy);
    }
    g_list_free_full(objects, g_object_unref);
    return array;
}

static void
network_cancel_pending(BtClient *self, gboolean report)
{
    self->network_generation++;
    g_cancellable_cancel(self->network_cancel);
    g_clear_object(&self->network_cancel);
    self->network_cancel = g_cancellable_new();
    if (!self->pan_pending)
        return;
    g_autofree char *path = g_steal_pointer(&self->pan_pending);
    g_autofree char *method = g_steal_pointer(&self->pan_method);
    g_hash_table_remove(self->pending, path);
    if (report && !self->stopped) {
        bt_client_error(self, "NetworkManager stopped during the tethering operation.");
        g_signal_emit(self, signals[OPERATION], 0, path, method, FALSE);
    }
    changed(self);
}

static void
network_appeared(GDBusConnection *connection, const char *name,
                 const char *owner, gpointer data)
{
    BtClient *self = data;
    if (g_strcmp0(self->network_owner, owner) == 0)
        return;
    network_cancel_pending(self, self->network_owner != NULL);
    g_free(self->network_owner);
    self->network_owner = g_strdup(owner);
    g_debug("NetworkManager available");
    changed(self);
}

static void
network_vanished(GDBusConnection *connection, const char *name, gpointer data)
{
    BtClient *self = data;
    gboolean was_available = self->network_owner != NULL;
    network_cancel_pending(self, was_available);
    g_clear_pointer(&self->network_owner, g_free);
    if (was_available)
        g_debug("NetworkManager unavailable");
    changed(self);
}

static void
owner_changed(GObject *manager, GParamSpec *pspec, gpointer data)
{
    BtClient *self = data;
    g_free(self->owner);
    self->owner = g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(manager));
    self->generation++;
    g_cancellable_cancel(self->cancel);
    g_clear_object(&self->cancel);
    self->cancel = g_cancellable_new();
    g_hash_table_remove_all(self->pending);
    g_clear_pointer(&self->scan_active, g_free);
    g_clear_pointer(&self->scan_wanted, g_free);
    self->scan_pending = FALSE;
    network_cancel_pending(self, FALSE);
    if (self->scan_timeout) {
        g_source_remove(self->scan_timeout);
        self->scan_timeout = 0;
    }
    g_debug("Bluetooth service %s", self->owner ? "available" : "unavailable");
    changed(self);
}

static void
object_changed(GDBusObjectManager *manager, GDBusObject *object, gpointer data)
{
    BtClient *self = data;
    const char *path = g_dbus_object_get_object_path(object);
    g_autoptr(GDBusObject) existing = g_dbus_object_manager_get_object(manager, path);
    if (!existing && g_strcmp0(path, self->scan_wanted) == 0)
        bt_client_scan(self, NULL);
    changed(self);
}

static void
interface_changed(GDBusObjectManager *manager, GDBusObject *object,
                   GDBusInterface *interface, gpointer data)
{
    changed(data);
}

static void
properties_changed(GDBusObjectManagerClient *manager, GDBusObjectProxy *object,
                    GDBusProxy *proxy, GVariant *properties,
                    const char *const *invalidated, gpointer data)
{
    BtClient *self = data;
    if (g_str_equal(g_dbus_proxy_get_interface_name(proxy), BT_ADAPTER) &&
        !bt_boolean(proxy, "Powered") &&
        g_strcmp0(g_dbus_proxy_get_object_path(proxy), self->scan_wanted) == 0)
        bt_client_scan(self, NULL);
    changed(self);
}

static void
manager_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    BtClient *self = data;
    g_autoptr(GError) error = NULL;
    GDBusObjectManager *manager = g_dbus_object_manager_client_new_finish(result, &error);
    if (self->stopped) {
        g_clear_object(&manager);
    } else if (!manager) {
        bt_client_error(self, "Could not load the Bluetooth service. Quit and reopen Mytooth to retry.");
    } else {
        self->manager = manager;
        g_signal_connect(manager, "notify::name-owner", G_CALLBACK(owner_changed), self);
        g_signal_connect(manager, "object-added", G_CALLBACK(object_changed), self);
        g_signal_connect(manager, "object-removed", G_CALLBACK(object_changed), self);
        g_signal_connect(manager, "interface-added", G_CALLBACK(interface_changed), self);
        g_signal_connect(manager, "interface-removed", G_CALLBACK(interface_changed), self);
        g_signal_connect(manager, "interface-proxy-properties-changed", G_CALLBACK(properties_changed), self);
        owner_changed(G_OBJECT(manager), NULL, self);
    }
    g_object_unref(self);
}

BtClient *
bt_client_new(GDBusConnection *connection)
{
    BtClient *self = g_object_new(BT_TYPE_CLIENT, NULL);
    self->connection = g_object_ref(connection);
    self->network_watch = g_bus_watch_name_on_connection(connection, NM_SERVICE,
        G_BUS_NAME_WATCHER_FLAGS_AUTO_START, network_appeared, network_vanished,
        self, NULL);
    g_dbus_object_manager_client_new(connection, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
        "org.bluez", "/", NULL, NULL, NULL, self->cancel, manager_ready, g_object_ref(self));
    return self;
}

void
bt_client_stop(BtClient *self)
{
    if (self->stopped)
        return;
    self->stopped = TRUE;
    if (self->scan_timeout) {
        g_source_remove(self->scan_timeout);
        self->scan_timeout = 0;
    }
    /* Dropping our private bus connection at exit also releases discovery ownership. */
    if (self->scan_active && self->owner)
        g_dbus_connection_call(self->connection, self->owner, self->scan_active,
            BT_ADAPTER, "StopDiscovery", NULL, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
            3000, NULL, NULL, NULL);
    g_cancellable_cancel(self->cancel);
    if (self->manager)
        g_signal_handlers_disconnect_by_data(self->manager, self);
    if (self->network_watch) {
        g_bus_unwatch_name(self->network_watch);
        self->network_watch = 0;
    }
    network_cancel_pending(self, FALSE);
}

static void
bt_client_finalize(GObject *object)
{
    BtClient *self = BT_CLIENT(object);
    bt_client_stop(self);
    g_clear_object(&self->manager);
    g_clear_object(&self->connection);
    g_clear_object(&self->cancel);
    g_clear_object(&self->network_cancel);
    g_hash_table_unref(self->pending);
    g_free(self->owner);
    g_free(self->network_owner);
    g_free(self->pan_pending);
    g_free(self->pan_method);
    g_free(self->scan_wanted);
    g_free(self->scan_active);
    G_OBJECT_CLASS(bt_client_parent_class)->finalize(object);
}

static void
bt_client_class_init(BtClientClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = bt_client_finalize;
    signals[CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[ERROR] = g_signal_new("error", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
    signals[OPERATION] = g_signal_new("operation", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
}

static void
bt_client_init(BtClient *self)
{
    self->cancel = g_cancellable_new();
    self->network_cancel = g_cancellable_new();
    self->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}
