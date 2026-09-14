/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "obex.h"
#include <errno.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#define OBEX_SERVICE "org.bluez.obex"
#define OBEX_ROOT "/org/bluez/obex"
#define OBEX_CLIENT_INTERFACE "org.bluez.obex.Client1"
#define OBEX_PUSH "org.bluez.obex.ObjectPush1"
#define OBEX_TRANSFER "org.bluez.obex.Transfer1"
#define OBEX_MANAGER "org.bluez.obex.AgentManager1"
#define OBEX_AGENT "org.bluez.obex.Agent1"
#define OBEX_AGENT_PATH "/io/github/the_specter_x/Mytooth/obex_agent"

static const char agent_xml[] =
    "<node><interface name='org.bluez.obex.Agent1'>"
    "<method name='Release'/><method name='Cancel'/>"
    "<method name='AuthorizePush'><arg type='o' direction='in'/>"
    "<arg type='s' direction='out'/></method></interface></node>";

struct _ObexClient {
    GObject parent_instance;
    GDBusConnection *bus;
    GDBusNodeInfo *info;
    GCancellable *cancel;
    GDBusMethodInvocation *invocation;
    GPtrArray *files;
    char *owner;
    char *address;
    char *session;
    char *transfer;
    char *name;
    char *status;
    char *prompt_name;
    char *receive_directory;
    guint watch;
    guint registration;
    guint signal_subscription;
    guint prompt_timeout;
    guint generation;
    guint operation;
    guint file_index;
    guint64 transferred;
    guint64 size;
    guint64 prompt_size;
    gboolean ready;
    gboolean locked;
    gboolean incoming;
    gboolean stopped;
};

G_DEFINE_TYPE(ObexClient, obex_client, G_TYPE_OBJECT)
enum { CHANGED, PROMPT_CHANGED, ERROR, COMPLETED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void changed(ObexClient *self) { if (!self->stopped) g_signal_emit(self, signals[CHANGED], 0); }
static void send_next(ObexClient *self);
static void reset_transfer(ObexClient *self);

static gboolean
valid_address(const char *address)
{
    if (!address || strlen(address) != 17)
        return FALSE;
    for (guint i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (address[i] != ':') return FALSE;
        } else if (!g_ascii_isxdigit(address[i])) {
            return FALSE;
        }
    }
    return TRUE;
}

static void
report_error(ObexClient *self, const char *message)
{
    if (!self->stopped)
        g_signal_emit(self, signals[ERROR], 0, message);
}

static char *
safe_name(const char *suggested)
{
    if (!suggested || !g_utf8_validate(suggested, -1, NULL))
        return g_strdup("Bluetooth file");
    g_autofree char *base = g_path_get_basename(suggested);
    GString *clean = g_string_sized_new(MIN(strlen(base), 200));
    for (const char *p = base; *p && clean->len < 200; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (c == '/' || c == '\\' || g_unichar_iscntrl(c))
            continue;
        char bytes[6];
        gint length = g_unichar_to_utf8(c, bytes);
        if (clean->len + (gsize)length > 200)
            break;
        g_string_append_len(clean, bytes, length);
    }
    g_strstrip(clean->str);
    if (!clean->str[0] || g_str_equal(clean->str, ".") || g_str_equal(clean->str, "..")) {
        g_string_assign(clean, "Bluetooth file");
    }
    return g_string_free(clean, FALSE);
}

char *
obex_safe_receive_path(const char *directory, const char *suggested, GError **error)
{
    g_return_val_if_fail(directory != NULL, NULL);
    struct stat st;
    if (g_mkdir_with_parents(directory, 0700) != 0 && errno != EEXIST) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "Could not create the Bluetooth download directory");
        return NULL;
    }
    if (lstat(directory, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) ||
        st.st_uid != geteuid()) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                            "The Bluetooth download location is not a safe owned directory");
        return NULL;
    }
    if (g_chmod(directory, 0700) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "Could not protect the Bluetooth download directory");
        return NULL;
    }

    g_autofree char *name = safe_name(suggested);
    g_autofree char *stem = NULL;
    const char *extension = "";
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        stem = g_strndup(name, dot - name);
        extension = dot;
    } else {
        stem = g_strdup(name);
    }
    for (guint n = 0; n < 10000; n++) {
        g_autofree char *candidate = n == 0 ? g_strdup(name) :
            g_strdup_printf("%s (%u)%s", stem, n, extension);
        char *path = g_build_filename(directory, candidate, NULL);
        if (lstat(path, &st) != 0 && errno == ENOENT)
            return path;
        g_free(path);
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "Could not choose an unused Bluetooth download name");
    return NULL;
}

static void
clear_prompt(ObexClient *self, const char *error_name)
{
    if (self->prompt_timeout) {
        g_source_remove(self->prompt_timeout);
        self->prompt_timeout = 0;
    }
    if (self->invocation) {
        g_dbus_method_invocation_return_dbus_error(self->invocation,
            error_name ? error_name : "org.bluez.obex.Error.Canceled", "Request ended");
        g_clear_object(&self->invocation);
    }
    g_clear_pointer(&self->prompt_name, g_free);
    self->prompt_size = 0;
    if (!self->stopped)
        g_signal_emit(self, signals[PROMPT_CHANGED], 0);
}

static gboolean
prompt_expired(gpointer data)
{
    ObexClient *self = data;
    self->prompt_timeout = 0;
    clear_prompt(self, "org.bluez.obex.Error.Rejected");
    self->operation++;
    reset_transfer(self);
    report_error(self, "The incoming file request timed out.");
    return G_SOURCE_REMOVE;
}

static void
reset_transfer(ObexClient *self)
{
    g_clear_pointer(&self->address, g_free);
    g_clear_pointer(&self->session, g_free);
    g_clear_pointer(&self->transfer, g_free);
    g_clear_pointer(&self->name, g_free);
    g_clear_pointer(&self->status, g_free);
    g_clear_pointer(&self->files, g_ptr_array_unref);
    self->file_index = 0;
    self->transferred = 0;
    self->size = 0;
    self->incoming = FALSE;
    changed(self);
}

static void
remove_session(ObexClient *self)
{
    if (self->owner && self->session)
        g_dbus_connection_call(self->bus, self->owner, OBEX_ROOT, OBEX_CLIENT_INTERFACE,
            "RemoveSession", g_variant_new("(o)", self->session), NULL,
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, NULL, NULL);
}

static void
fail_transfer(ObexClient *self, const char *message)
{
    remove_session(self);
    self->operation++;
    reset_transfer(self);
    report_error(self, message);
}

static void
complete_transfer(ObexClient *self)
{
    gboolean incoming = self->incoming;
    g_autofree char *name = g_strdup(self->name ? self->name : "Bluetooth file");
    remove_session(self);
    self->operation++;
    reset_transfer(self);
    if (!self->stopped)
        g_signal_emit(self, signals[COMPLETED], 0, incoming, name);
}

static void
read_transfer_properties(ObexClient *self, GVariant *properties)
{
    const char *text;
    guint64 value;
    if (g_variant_lookup(properties, "Name", "&s", &text)) {
        g_free(self->name);
        self->name = safe_name(text);
    }
    if (g_variant_lookup(properties, "Status", "&s", &text)) {
        g_free(self->status);
        self->status = g_strdup(text);
    }
    if (g_variant_lookup(properties, "Size", "t", &value))
        self->size = value;
    if (g_variant_lookup(properties, "Transferred", "t", &value))
        self->transferred = value;
}

static void
transfer_state(ObexClient *self, GVariant *properties)
{
    read_transfer_properties(self, properties);
    changed(self);
    if (!self->status)
        return;
    if (g_str_equal(self->status, "complete")) {
        if (self->incoming) {
            complete_transfer(self);
        } else if (self->files && ++self->file_index < self->files->len) {
            g_clear_pointer(&self->transfer, g_free);
            self->transferred = self->size = 0;
            send_next(self);
        } else {
            complete_transfer(self);
        }
    } else if (g_str_equal(self->status, "error")) {
        fail_transfer(self, "The Bluetooth file transfer failed.");
    }
}

static void
properties_changed(GDBusConnection *bus, const char *sender, const char *path,
                   const char *interface, const char *signal, GVariant *parameters,
                   gpointer data)
{
    ObexClient *self = data;
    if (!self->owner || g_strcmp0(sender, self->owner) != 0 ||
        g_strcmp0(path, self->transfer) != 0)
        return;
    const char *changed_interface;
    g_autoptr(GVariant) values = NULL;
    g_autoptr(GVariant) invalidated = NULL;
    g_variant_get(parameters, "(&s@a{sv}@as)", &changed_interface, &values, &invalidated);
    if (g_str_equal(changed_interface, OBEX_TRANSFER))
        transfer_state(self, values);
}

typedef struct { ObexClient *client; guint generation; guint operation; char *path; } Request;

static void
request_free(Request *request)
{
    g_object_unref(request->client);
    g_free(request->path);
    g_free(request);
}

static void
send_file_done(GObject *source, GAsyncResult *result, gpointer data)
{
    Request *request = data;
    ObexClient *self = request->client;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!self->stopped && request->generation == self->generation &&
        request->operation == self->operation) {
        if (!reply) {
            if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                fail_transfer(self, "Could not start the Bluetooth file transfer.");
        } else {
            const char *path;
            g_autoptr(GVariant) properties = NULL;
            g_variant_get(reply, "(&o@a{sv})", &path, &properties);
            self->transfer = g_strdup(path);
            read_transfer_properties(self, properties);
            changed(self);
            if (self->status && (g_str_equal(self->status, "complete") ||
                                 g_str_equal(self->status, "error")))
                transfer_state(self, properties);
        }
    }
    request_free(request);
}

static void
send_next(ObexClient *self)
{
    if (!self->files || self->file_index >= self->files->len || !self->session)
        return;
    const char *path = g_ptr_array_index(self->files, self->file_index);
    g_free(self->name);
    self->name = g_path_get_basename(path);
    g_free(self->status);
    self->status = g_strdup("queued");
    changed(self);
    Request *request = g_new0(Request, 1);
    request->client = g_object_ref(self);
    request->generation = self->generation;
    request->operation = self->operation;
    g_dbus_connection_call(self->bus, self->owner, self->session, OBEX_PUSH,
        "SendFile", g_variant_new("(s)", path), G_VARIANT_TYPE("(oa{sv})"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 30000, self->cancel, send_file_done, request);
}

static void
session_created(GObject *source, GAsyncResult *result, gpointer data)
{
    Request *request = data;
    ObexClient *self = request->client;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!self->stopped && request->generation == self->generation && reply &&
        request->operation != self->operation) {
        const char *path;
        g_variant_get(reply, "(&o)", &path);
        g_dbus_connection_call(self->bus, self->owner, OBEX_ROOT, OBEX_CLIENT_INTERFACE,
            "RemoveSession", g_variant_new("(o)", path), NULL,
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, NULL, NULL);
    } else if (!self->stopped && request->generation == self->generation &&
               request->operation == self->operation) {
        if (!reply) {
            if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                fail_transfer(self, "Could not connect to the device for file transfer.");
        } else {
            const char *path;
            g_variant_get(reply, "(&o)", &path);
            self->session = g_strdup(path);
            send_next(self);
        }
    }
    request_free(request);
}

gboolean
obex_client_send(ObexClient *self, const char *address, GPtrArray *paths, GError **error)
{
    g_return_val_if_fail(OBEX_IS_CLIENT(self), FALSE);
    if (!self->owner) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                            "The Bluetooth file-transfer service is unavailable");
        return FALSE;
    }
    if (obex_client_busy(self) || self->invocation) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                            "Another Bluetooth file transfer is already active");
        return FALSE;
    }
    if (!valid_address(address) || !paths || paths->len == 0 || paths->len > 64) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Choose between one and 64 files to send");
        return FALSE;
    }
    GPtrArray *copy = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < paths->len; i++) {
        const char *path = g_ptr_array_index(paths, i);
        if (!path || !g_path_is_absolute(path) || !g_file_test(path, G_FILE_TEST_IS_REGULAR) ||
            g_access(path, R_OK) != 0) {
            g_ptr_array_unref(copy);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "Every selected item must be a readable regular file");
            return FALSE;
        }
        g_ptr_array_add(copy, g_strdup(path));
    }
    self->files = copy;
    self->operation++;
    self->address = g_strdup(address);
    self->file_index = 0;
    self->incoming = FALSE;
    self->status = g_strdup("connecting");
    changed(self);

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "Target", g_variant_new_string("opp"));
    Request *request = g_new0(Request, 1);
    request->client = g_object_ref(self);
    request->generation = self->generation;
    request->operation = self->operation;
    g_dbus_connection_call(self->bus, self->owner, OBEX_ROOT, OBEX_CLIENT_INTERFACE,
        "CreateSession", g_variant_new("(s@a{sv})", address, g_variant_builder_end(&options)),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 30000,
        self->cancel, session_created, request);
    return TRUE;
}

void
obex_client_cancel(ObexClient *self)
{
    if (!obex_client_busy(self))
        return;
    if (self->owner && self->transfer)
        g_dbus_connection_call(self->bus, self->owner, self->transfer, OBEX_TRANSFER,
            "Cancel", NULL, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, NULL, NULL);
    remove_session(self);
    self->operation++;
    reset_transfer(self);
}

static void
incoming_properties(GObject *source, GAsyncResult *result, gpointer data)
{
    Request *request = data;
    ObexClient *self = request->client;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!self->stopped && request->generation == self->generation &&
        request->operation == self->operation && self->invocation &&
        g_strcmp0(request->path, self->transfer) == 0) {
        if (!reply || self->locked) {
            gboolean failed = !reply && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
            clear_prompt(self, "org.bluez.obex.Error.Rejected");
            reset_transfer(self);
            if (failed)
                report_error(self, "Could not read the incoming Bluetooth file request.");
        } else {
            g_autoptr(GVariant) properties = NULL;
            g_variant_get(reply, "(@a{sv})", &properties);
            const char *name = NULL;
            g_variant_lookup(properties, "Name", "&s", &name);
            if (!name)
                g_variant_lookup(properties, "Filename", "&s", &name);
            self->prompt_name = safe_name(name);
            g_variant_lookup(properties, "Size", "t", &self->prompt_size);
            self->prompt_timeout = g_timeout_add_seconds(60, prompt_expired, self);
            g_signal_emit(self, signals[PROMPT_CHANGED], 0);
        }
    }
    request_free(request);
}

static void
agent_method(GDBusConnection *bus, const char *sender, const char *path,
             const char *interface, const char *method, GVariant *parameters,
             GDBusMethodInvocation *invocation, gpointer data)
{
    ObexClient *self = data;
    if (self->stopped || !self->owner || g_strcmp0(sender, self->owner) != 0) {
        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.bluez.obex.Error.Rejected", "Not the Bluetooth file-transfer service");
        return;
    }
    if (g_str_equal(method, "Cancel") || g_str_equal(method, "Release")) {
        clear_prompt(self, "org.bluez.obex.Error.Canceled");
        if (self->incoming)
            reset_transfer(self);
        if (g_str_equal(method, "Release")) {
            self->ready = FALSE;
            changed(self);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (!g_str_equal(method, "AuthorizePush") || self->locked ||
        obex_client_busy(self) || self->invocation) {
        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.bluez.obex.Error.Rejected", "File receiving is unavailable");
        return;
    }
    const char *transfer;
    g_variant_get(parameters, "(&o)", &transfer);
    self->invocation = g_object_ref(invocation);
    self->operation++;
    self->transfer = g_strdup(transfer);
    self->incoming = TRUE;
    self->status = g_strdup("waiting for approval");
    Request *request = g_new0(Request, 1);
    request->client = g_object_ref(self);
    request->generation = self->generation;
    request->operation = self->operation;
    request->path = g_strdup(transfer);
    g_dbus_connection_call(self->bus, self->owner, transfer,
        "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", OBEX_TRANSFER),
        G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000,
        self->cancel, incoming_properties, request);
}

static const GDBusInterfaceVTable agent_vtable = { .method_call = agent_method };

void
obex_client_answer(ObexClient *self, gboolean accept)
{
    if (!self->invocation)
        return;
    if (accept && !self->locked) {
        g_autoptr(GError) error = NULL;
        g_autofree char *path = obex_safe_receive_path(self->receive_directory,
            self->prompt_name, &error);
        if (!path) {
            clear_prompt(self, "org.bluez.obex.Error.Rejected");
            self->operation++;
            reset_transfer(self);
            report_error(self, "Could not prepare a safe location for the incoming file.");
            return;
        }
        g_dbus_method_invocation_return_value(self->invocation, g_variant_new("(s)", path));
        g_clear_object(&self->invocation);
        g_free(self->name);
        self->name = g_strdup(self->prompt_name);
        g_free(self->status);
        self->status = g_strdup("queued");
        self->size = self->prompt_size;
        self->transferred = 0;
        if (self->prompt_timeout) {
            g_source_remove(self->prompt_timeout);
            self->prompt_timeout = 0;
        }
        g_clear_pointer(&self->prompt_name, g_free);
        self->prompt_size = 0;
        g_signal_emit(self, signals[PROMPT_CHANGED], 0);
        changed(self);
    } else {
        clear_prompt(self, "org.bluez.obex.Error.Rejected");
        self->operation++;
        reset_transfer(self);
    }
}

typedef struct { ObexClient *client; guint generation; } Registration;

static void
registered(GObject *source, GAsyncResult *result, gpointer data)
{
    Registration *request = data;
    ObexClient *self = request->client;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!self->stopped && request->generation == self->generation) {
        self->ready = reply != NULL;
        if (!reply && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            report_error(self, "The Bluetooth file receiver could not register.");
        changed(self);
    }
    g_object_unref(self);
    g_free(request);
}

static void
appeared(GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
    ObexClient *self = data;
    self->generation++;
    g_cancellable_cancel(self->cancel);
    g_clear_object(&self->cancel);
    self->cancel = g_cancellable_new();
    g_free(self->owner);
    self->owner = g_strdup(owner);
    self->ready = FALSE;
    Registration *request = g_new0(Registration, 1);
    request->client = g_object_ref(self);
    request->generation = self->generation;
    g_dbus_connection_call(bus, owner, OBEX_ROOT, OBEX_MANAGER, "RegisterAgent",
        g_variant_new("(o)", OBEX_AGENT_PATH), G_VARIANT_TYPE_UNIT,
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000, self->cancel, registered, request);
    changed(self);
}

static void
vanished(GDBusConnection *bus, const char *name, gpointer data)
{
    ObexClient *self = data;
    gboolean active = obex_client_busy(self) || self->invocation;
    self->generation++;
    g_cancellable_cancel(self->cancel);
    g_clear_object(&self->cancel);
    self->cancel = g_cancellable_new();
    clear_prompt(self, "org.bluez.obex.Error.Canceled");
    reset_transfer(self);
    g_clear_pointer(&self->owner, g_free);
    self->ready = FALSE;
    if (active)
        report_error(self, "The Bluetooth file-transfer service stopped during a transfer.");
    changed(self);
}

ObexClient *
obex_client_new(GDBusConnection *session_bus)
{
    ObexClient *self = g_object_new(OBEX_TYPE_CLIENT, NULL);
    self->bus = g_object_ref(session_bus);
    self->info = g_dbus_node_info_new_for_xml(agent_xml, NULL);
    g_autoptr(GError) error = NULL;
    self->registration = g_dbus_connection_register_object(session_bus, OBEX_AGENT_PATH,
        self->info->interfaces[0], &agent_vtable, self, NULL, &error);
    if (!self->registration)
        report_error(self, "Could not export the Bluetooth file receiver.");
    self->signal_subscription = g_dbus_connection_signal_subscribe(session_bus, NULL,
        "org.freedesktop.DBus.Properties", "PropertiesChanged", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, properties_changed, self, NULL);
    self->watch = g_bus_watch_name_on_connection(session_bus, OBEX_SERVICE,
        G_BUS_NAME_WATCHER_FLAGS_AUTO_START, appeared, vanished, self, NULL);
    return self;
}

void
obex_client_set_locked(ObexClient *self, gboolean locked)
{
    self->locked = locked;
    if (locked && self->invocation)
        obex_client_answer(self, FALSE);
}

gboolean obex_client_available(ObexClient *self) { return self->owner != NULL; }
gboolean obex_client_ready(ObexClient *self) { return self->ready; }
gboolean obex_client_busy(ObexClient *self) { return self->transfer != NULL || self->files != NULL; }
gboolean obex_client_incoming(ObexClient *self) { return self->incoming; }
const char *obex_client_name(ObexClient *self) { return self->name; }
const char *obex_client_status(ObexClient *self) { return self->status; }
guint64 obex_client_transferred(ObexClient *self) { return self->transferred; }
guint64 obex_client_size(ObexClient *self) { return self->size; }
guint obex_client_file_number(ObexClient *self) { return self->files ? self->file_index + 1 : 1; }
guint obex_client_file_count(ObexClient *self) { return self->files ? self->files->len : 1; }
gboolean obex_client_has_prompt(ObexClient *self) { return self->invocation != NULL && self->prompt_name != NULL; }
const char *obex_client_prompt_name(ObexClient *self) { return self->prompt_name; }
guint64 obex_client_prompt_size(ObexClient *self) { return self->prompt_size; }
const char *obex_client_receive_directory(ObexClient *self) { return self->receive_directory; }
void
obex_client_set_receive_directory(ObexClient *self, const char *directory)
{
    g_return_if_fail(OBEX_IS_CLIENT(self));
    g_return_if_fail(g_path_is_absolute(directory));
    if (obex_client_busy(self) || self->invocation)
        return;
    g_free(self->receive_directory);
    self->receive_directory = g_strdup(directory);
}

void
obex_client_stop(ObexClient *self)
{
    if (self->stopped)
        return;
    self->stopped = TRUE;
    if (self->owner && self->ready)
        g_dbus_connection_call(self->bus, self->owner, OBEX_ROOT, OBEX_MANAGER,
            "UnregisterAgent", g_variant_new("(o)", OBEX_AGENT_PATH), NULL,
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL, NULL, NULL);
    obex_client_cancel(self);
    clear_prompt(self, "org.bluez.obex.Error.Canceled");
    g_cancellable_cancel(self->cancel);
    if (self->watch) { g_bus_unwatch_name(self->watch); self->watch = 0; }
    if (self->signal_subscription) {
        g_dbus_connection_signal_unsubscribe(self->bus, self->signal_subscription);
        self->signal_subscription = 0;
    }
    if (self->registration) {
        g_dbus_connection_unregister_object(self->bus, self->registration);
        self->registration = 0;
    }
}

static void
obex_client_finalize(GObject *object)
{
    ObexClient *self = OBEX_CLIENT(object);
    obex_client_stop(self);
    reset_transfer(self);
    g_clear_object(&self->invocation);
    g_clear_object(&self->cancel);
    g_clear_object(&self->bus);
    g_clear_pointer(&self->info, g_dbus_node_info_unref);
    g_free(self->owner);
    g_free(self->prompt_name);
    g_free(self->receive_directory);
    G_OBJECT_CLASS(obex_client_parent_class)->finalize(object);
}

static void
obex_client_class_init(ObexClientClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = obex_client_finalize;
    signals[CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[PROMPT_CHANGED] = g_signal_new("prompt-changed", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[ERROR] = g_signal_new("error", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
    signals[COMPLETED] = g_signal_new("completed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_BOOLEAN, G_TYPE_STRING);
}

static void
obex_client_init(ObexClient *self)
{
    self->cancel = g_cancellable_new();
    self->locked = TRUE;
    const char *downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    g_autofree char *fallback = downloads ? NULL : g_build_filename(g_get_home_dir(), "Downloads", NULL);
    self->receive_directory = g_build_filename(downloads ? downloads : fallback, "Bluetooth", NULL);
}
