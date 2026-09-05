/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "agent.h"
#include <string.h>

#define AGENT_PATH "/io/github/the_specter_x/Mytooth/agent"
#define AGENT_MANAGER "org.bluez.AgentManager1"

static const char agent_xml[] =
    "<node><interface name='org.bluez.Agent1'>"
    "<method name='Release'/>"
    "<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
    "<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
    "<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='Cancel'/></interface></node>";

struct _BtAgent {
    GObject parent_instance;
    BtClient *client;
    GDBusNodeInfo *info;
    GDBusMethodInvocation *invocation;
    GCancellable *cancel;
    char *owner;
    char *device;
    char *text;
    guint registration;
    guint timeout;
    guint generation;
    gboolean ready;
    gboolean is_default;
    gboolean locked;
    gboolean stopped;
    BtPrompt prompt;
};
G_DEFINE_TYPE(BtAgent, bt_agent, G_TYPE_OBJECT)
enum { PROMPT_CHANGED, CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
clear_prompt(BtAgent *self, const char *error_name)
{
    if (self->timeout) {
        g_source_remove(self->timeout);
        self->timeout = 0;
    }
    if (self->invocation) {
        g_dbus_method_invocation_return_dbus_error(self->invocation,
            error_name ? error_name : "org.bluez.Error.Canceled", "Request ended");
        g_clear_object(&self->invocation);
    }
    g_clear_pointer(&self->device, g_free);
    g_clear_pointer(&self->text, g_free);
    self->prompt = BT_PROMPT_NONE;
    g_signal_emit(self, signals[PROMPT_CHANGED], 0);
}

static gboolean
prompt_expired(gpointer data)
{
    BtAgent *self = data;
    self->timeout = 0;
    if (self->prompt == BT_PROMPT_DISPLAY && self->device)
        bt_client_method(self->client, self->device, BT_DEVICE, "CancelPairing", NULL);
    clear_prompt(self, "org.bluez.Error.Canceled");
    bt_client_error(self->client, "The pairing request timed out. Start pairing again to retry.");
    return G_SOURCE_REMOVE;
}

static void
handle_method(GDBusConnection *connection, const char *sender,
              const char *path, const char *interface, const char *method,
              GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
    BtAgent *self = data;
    /* Agent objects are public on the system bus. Only the current BlueZ owner may call them. */
    if (self->stopped || !self->owner || g_strcmp0(sender, self->owner) != 0) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Not the Bluetooth service");
        return;
    }
    if (g_str_equal(method, "Cancel") || g_str_equal(method, "Release")) {
        clear_prompt(self, "org.bluez.Error.Canceled");
        if (g_str_equal(method, "Release")) {
            self->ready = FALSE;
            self->is_default = FALSE;
            g_signal_emit(self, signals[CHANGED], 0);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    const char *device;
    g_variant_get_child(parameters, 0, "&o", &device);
    g_autoptr(GDBusProxy) proxy = bt_client_proxy(self->client, device, BT_DEVICE);
    if (self->locked || !proxy || bt_boolean(proxy, "Blocked")) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Session locked or device unavailable");
        return;
    }
    gboolean display = g_str_equal(method, "DisplayPinCode") || g_str_equal(method, "DisplayPasskey");
    gboolean update = display && self->prompt == BT_PROMPT_DISPLAY && g_strcmp0(self->device, device) == 0;
    if (self->prompt != BT_PROMPT_NONE && !update) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Another pairing request is open");
        return;
    }
    if (!update)
        self->device = g_strdup(device);
    g_free(self->text);
    self->text = NULL;
    if (g_str_equal(method, "RequestPinCode")) {
        self->prompt = BT_PROMPT_PIN;
        self->text = g_strdup("Enter the PIN supplied with this device (1–16 bytes).");
    } else if (g_str_equal(method, "RequestPasskey")) {
        self->prompt = BT_PROMPT_PASSKEY;
        self->text = g_strdup("Enter the device's passkey (up to six digits).");
    } else if (g_str_equal(method, "RequestConfirmation")) {
        guint32 key;
        g_variant_get_child(parameters, 1, "u", &key);
        self->prompt = BT_PROMPT_CONFIRM;
        self->text = g_strdup_printf("Does this code match the code on your device?\n\n%06u", key);
    } else if (g_str_equal(method, "RequestAuthorization")) {
        self->prompt = BT_PROMPT_AUTHORIZE;
        self->text = g_strdup("This device wants to pair with your computer. Allow pairing?");
    } else if (g_str_equal(method, "AuthorizeService")) {
        const char *uuid;
        g_variant_get_child(parameters, 1, "&s", &uuid);
        self->prompt = BT_PROMPT_AUTHORIZE;
        self->text = g_strdup_printf("Allow this device to use this Bluetooth service?\n\n%.128s", uuid);
    } else if (g_str_equal(method, "DisplayPinCode")) {
        const char *pin;
        g_variant_get_child(parameters, 1, "&s", &pin);
        self->prompt = BT_PROMPT_DISPLAY;
        self->text = g_strdup_printf("Type this PIN on the device, then press Enter:\n\n%.16s", pin);
    } else if (g_str_equal(method, "DisplayPasskey")) {
        guint32 key;
        guint16 entered;
        g_variant_get_child(parameters, 1, "u", &key);
        g_variant_get_child(parameters, 2, "q", &entered);
        self->prompt = BT_PROMPT_DISPLAY;
        self->text = g_strdup_printf("Type this code on the device, then press Enter:\n\n%06u\n\n%u of 6 digits entered", key, MIN(entered, 6));
    } else {
        clear_prompt(self, NULL);
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotSupported", "Unknown agent method");
        return;
    }
    if (display)
        g_dbus_method_invocation_return_value(invocation, NULL);
    else
        self->invocation = g_object_ref(invocation);
    if (!self->timeout)
        self->timeout = g_timeout_add_seconds(90, prompt_expired, self);
    g_signal_emit(self, signals[PROMPT_CHANGED], 0);
}

static const GDBusInterfaceVTable agent_vtable = { .method_call = handle_method };

gboolean
bt_agent_answer(BtAgent *self, gboolean accept, const char *text)
{
    guint32 key = 0;
    if (self->prompt == BT_PROMPT_NONE)
        return FALSE;
    if (accept && !self->locked) {
        if (self->prompt == BT_PROMPT_PIN && !bt_valid_pin(text))
            return FALSE;
        if (self->prompt == BT_PROMPT_PASSKEY && !bt_parse_passkey(text, &key))
            return FALSE;
        if (self->invocation) {
            GVariant *reply = NULL;
            if (self->prompt == BT_PROMPT_PIN)
                reply = g_variant_new("(s)", text);
            else if (self->prompt == BT_PROMPT_PASSKEY)
                reply = g_variant_new("(u)", key);
            g_dbus_method_invocation_return_value(self->invocation, reply);
            g_clear_object(&self->invocation);
        }
    } else if (self->prompt == BT_PROMPT_DISPLAY && self->device) {
        bt_client_method(self->client, self->device, BT_DEVICE, "CancelPairing", NULL);
    }
    clear_prompt(self, "org.bluez.Error.Rejected");
    return TRUE;
}

typedef struct { BtAgent *agent; guint generation; gboolean make_default; } Registration;

static void register_call(BtAgent *self, gboolean make_default);

static void
registered(GObject *source, GAsyncResult *result, gpointer data)
{
    Registration *request = data;
    BtAgent *self = request->agent;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!self->stopped && request->generation == self->generation) {
        if (request->make_default)
            self->is_default = reply != NULL;
        else
            self->ready = reply != NULL;
        if (reply && !request->make_default)
            register_call(self, TRUE);
        else if (!reply && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            bt_client_error(self->client, request->make_default ?
                "Mytooth could not become the default pairing agent. Incoming requests may go to another manager." :
                "The pairing agent could not register. Use Retry pairing agent in the menu.");
        g_signal_emit(self, signals[CHANGED], 0);
    }
    g_object_unref(self);
    g_free(request);
}

static void
register_call(BtAgent *self, gboolean make_default)
{
    Registration *request = g_new0(Registration, 1);
    request->agent = g_object_ref(self);
    request->generation = self->generation;
    request->make_default = make_default;
    g_dbus_connection_call(bt_client_connection(self->client), self->owner,
        "/org/bluez", AGENT_MANAGER, make_default ? "RequestDefaultAgent" : "RegisterAgent",
        make_default ? g_variant_new("(o)", AGENT_PATH) : g_variant_new("(os)", AGENT_PATH, "KeyboardDisplay"),
        G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000, self->cancel, registered, request);
}

void
bt_agent_retry(BtAgent *self)
{
    if (self->owner && !self->stopped && self->registration)
        register_call(self, self->ready);
}

static void
client_changed(BtClient *client, gpointer data)
{
    BtAgent *self = data;
    const char *owner = bt_client_owner(client);
    if (g_strcmp0(owner, self->owner) != 0) {
        self->generation++;
        g_cancellable_cancel(self->cancel);
        g_clear_object(&self->cancel);
        self->cancel = g_cancellable_new();
        clear_prompt(self, "org.bluez.Error.Canceled");
        g_free(self->owner);
        self->owner = g_strdup(owner);
        self->ready = FALSE;
        self->is_default = FALSE;
        if (owner && self->registration)
            register_call(self, FALSE);
        g_signal_emit(self, signals[CHANGED], 0);
    }
    if (self->device) {
        g_autoptr(GDBusProxy) proxy = bt_client_proxy(client, self->device, BT_DEVICE);
        if (!proxy || (self->prompt == BT_PROMPT_DISPLAY && bt_boolean(proxy, "Paired")))
            clear_prompt(self, "org.bluez.Error.Canceled");
    }
}

static void
operation_done(BtClient *client, const char *path, const char *method, gboolean success, gpointer data)
{
    BtAgent *self = data;
    if ((g_str_equal(method, "Pair") || g_str_equal(method, "CancelPairing")) &&
        g_strcmp0(path, self->device) == 0)
        clear_prompt(self, "org.bluez.Error.Canceled");
}

BtAgent *
bt_agent_new(BtClient *client)
{
    BtAgent *self = g_object_new(BT_TYPE_AGENT, NULL);
    self->client = g_object_ref(client);
    self->info = g_dbus_node_info_new_for_xml(agent_xml, NULL);
    g_autoptr(GError) error = NULL;
    self->registration = g_dbus_connection_register_object(bt_client_connection(client), AGENT_PATH,
        self->info->interfaces[0], &agent_vtable, self, NULL, &error);
    if (!self->registration)
        bt_client_error(client, "Could not export the pairing agent.");
    g_signal_connect(client, "changed", G_CALLBACK(client_changed), self);
    g_signal_connect(client, "operation", G_CALLBACK(operation_done), self);
    client_changed(client, self);
    return self;
}

void
bt_agent_set_locked(BtAgent *self, gboolean locked)
{
    self->locked = locked;
    if (locked && self->prompt != BT_PROMPT_NONE)
        bt_agent_answer(self, FALSE, NULL);
    g_signal_emit(self, signals[CHANGED], 0);
}

gboolean bt_agent_ready(BtAgent *self) { return self->ready; }
gboolean bt_agent_is_default(BtAgent *self) { return self->is_default; }
gboolean bt_agent_locked(BtAgent *self) { return self->locked; }
BtPrompt bt_agent_prompt(BtAgent *self) { return self->prompt; }
const char *bt_agent_device(BtAgent *self) { return self->device; }
const char *bt_agent_text(BtAgent *self) { return self->text; }

void
bt_agent_stop(BtAgent *self)
{
    if (self->stopped)
        return;
    self->stopped = TRUE;
    self->generation++;
    clear_prompt(self, "org.bluez.Error.Canceled");
    g_cancellable_cancel(self->cancel);
    g_signal_handlers_disconnect_by_data(self->client, self);
    if (self->owner && self->ready)
        g_dbus_connection_call(bt_client_connection(self->client), self->owner, "/org/bluez",
            AGENT_MANAGER, "UnregisterAgent", g_variant_new("(o)", AGENT_PATH), NULL,
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL, NULL, NULL);
    if (self->registration) {
        g_dbus_connection_unregister_object(bt_client_connection(self->client), self->registration);
        self->registration = 0;
    }
}

static void
bt_agent_finalize(GObject *object)
{
    BtAgent *self = BT_AGENT(object);
    bt_agent_stop(self);
    g_clear_object(&self->client);
    g_clear_object(&self->cancel);
    g_clear_pointer(&self->info, g_dbus_node_info_unref);
    g_free(self->owner);
    G_OBJECT_CLASS(bt_agent_parent_class)->finalize(object);
}

static void
bt_agent_class_init(BtAgentClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = bt_agent_finalize;
    signals[PROMPT_CHANGED] = g_signal_new("prompt-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
bt_agent_init(BtAgent *self)
{
    self->cancel = g_cancellable_new();
    self->locked = TRUE; /* Session watcher must establish that prompts are safe to show. */
}
