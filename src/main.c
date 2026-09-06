/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"
#include <gdk/gdkwayland.h>
#include <glib-unix.h>
#include <unistd.h>
#include <string.h>

static gboolean debug_logging;

static void
trace_startup(const char *stage)
{
    if (g_getenv("MYTOOTH_TEST_TRACE"))
        g_printerr("startup: %s\n", stage);
}

static GLogWriterOutput
log_writer(GLogLevelFlags level, const GLogField *fields, gsize count, gpointer data)
{
    if ((level & (G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO)) && !debug_logging)
        return G_LOG_WRITER_HANDLED;
    if (!isatty(STDERR_FILENO)) {
        GLogField *journal = g_new(GLogField, count + 1);
        memcpy(journal, fields, count * sizeof *journal);
        journal[count] = (GLogField) { "SYSLOG_IDENTIFIER", "mytooth", -1 };
        GLogWriterOutput result = g_log_writer_journald(level, journal, count + 1, NULL);
        g_free(journal);
        if (result == G_LOG_WRITER_HANDLED)
            return result;
    }
    return g_log_writer_standard_streams(level, fields, count, NULL);
}

void
app_notify(App *app, const char *id, const char *title, const char *body)
{
    if (!app->settings.notifications || !app->notification_service || app->closing ||
        (app->agent && bt_agent_locked(app->agent)))
        return;
    g_autoptr(GNotification) notification = g_notification_new(title);
    g_notification_set_body(notification, body);
    g_autoptr(GIcon) icon = g_themed_icon_new("bluetooth-symbolic");
    g_notification_set_icon(notification, icon);
    g_notification_set_default_action(notification, "app.show");
    g_application_send_notification(G_APPLICATION(app->application), id, notification);
}

void
app_error(App *app, const char *message)
{
    if (app->closing)
        return;
    view_error(app->view, message);
    app_notify(app, "error", "Bluetooth needs attention", message);
}

void
app_show(App *app)
{
    app->background = FALSE;
    if (app->view)
        view_show(app->view);
}

static gboolean
refresh_now(gpointer data)
{
    App *app = data;
    app->refresh_idle = 0;
    if (!app->closing) {
        if (app->client) {
            g_autoptr(GDBusProxy) selected = bt_client_proxy(app->client, app->adapter, BT_ADAPTER);
            if (!selected) {
                g_autoptr(GPtrArray) adapters = bt_client_list(app->client, BT_ADAPTER);
                g_clear_pointer(&app->adapter, g_free);
                if (adapters->len)
                    app->adapter = g_strdup(g_dbus_proxy_get_object_path(g_ptr_array_index(adapters, 0)));
            }
        }
        view_refresh(app->view);
        tray_refresh(app->tray);
    }
    return G_SOURCE_REMOVE;
}

gboolean
app_refresh(gpointer data)
{
    App *app = data;
    if (!app->closing && !app->refresh_idle)
        app->refresh_idle = g_idle_add(refresh_now, app);
    return G_SOURCE_REMOVE;
}

static void model_changed(GObject *object, gpointer data) { app_refresh(data); }
static void model_error(BtClient *client, const char *message, gpointer data) { app_error(data, message); }

static void
operation_done(BtClient *client, const char *path, const char *method, gboolean success, gpointer data)
{
    App *app = data;
    if (!success)
        return;
    if (g_str_equal(method, "Pair"))
        app_notify(app, "operation", "Device paired", "You can now connect to the device. Trust remains your choice.");
    else if (g_str_equal(method, "Connect"))
        app_notify(app, "operation", "Device connected", "The Bluetooth connection is ready.");
    else if (g_str_equal(method, "Disconnect"))
        app_notify(app, "operation", "Device disconnected", "The Bluetooth connection has ended.");
}

static void
prompt_changed(BtAgent *agent, gpointer data)
{
    App *app = data;
    view_pairing(app->view);
    if (bt_agent_prompt(agent) != BT_PROMPT_NONE)
        app_notify(app, "pairing", "Bluetooth pairing request", "Open Mytooth to review the request.");
    else
        g_application_withdraw_notification(G_APPLICATION(app->application), "pairing");
}

void
app_choose_adapter(App *app, const char *path)
{
    if (g_strcmp0(app->adapter, path) == 0)
        return;
    if (app->client)
        bt_client_scan(app->client, NULL);
    g_free(app->adapter);
    app->adapter = g_strdup(path);
    app_refresh(app);
}

void
app_power(App *app)
{
    if (!app->client || !app->adapter)
        return;
    g_autoptr(GDBusProxy) adapter = bt_client_proxy(app->client, app->adapter, BT_ADAPTER);
    if (!adapter)
        return;
    RadioState radio = rfkill_state(app->radio, app->adapter);
    if (radio == RADIO_HARD_BLOCKED) {
        app_error(app, "Bluetooth is blocked by a hardware switch. Turn the switch on first.");
        return;
    }
    if (radio == RADIO_SOFT_BLOCKED) {
        g_autoptr(GError) error = NULL;
        if (!rfkill_unblock(app->radio, app->adapter, &error))
            app_error(app, error->message);
        else
            app_error(app, "Bluetooth has been unblocked. Turn the adapter on when the block clears.");
        return;
    }
    bt_client_set(app->client, app->adapter, BT_ADAPTER, "Powered",
        g_variant_new_boolean(!bt_boolean(adapter, "Powered")));
}

void
app_device_action(App *app, const char *path)
{
    if (!app->client)
        return;
    g_autoptr(GDBusProxy) device = bt_client_proxy(app->client, path, BT_DEVICE);
    if (!device)
        return;
    const char *pending = bt_client_pending(app->client, path);
    if (g_strcmp0(pending, "Pair") == 0) {
        bt_client_method(app->client, path, BT_DEVICE, "CancelPairing", NULL);
        return;
    }
    if (bt_boolean(device, "Blocked")) {
        app_error(app, "Unblock this device in its details before connecting.");
        return;
    }
    const char *method = bt_boolean(device, "Connected") ? "Disconnect" :
        (bt_boolean(device, "Paired") ? "Connect" : "Pair");
    if (g_str_equal(method, "Pair") && (!bt_agent_ready(app->agent) || bt_agent_locked(app->agent))) {
        app_error(app, "Pairing requires a registered agent and an unlocked Cinnamon session.");
        return;
    }
    bt_client_method(app->client, path, BT_DEVICE, method, NULL);
}

void
app_quit(App *app)
{
    if (app->closing)
        return;
    app->closing = TRUE;
    if (app->agent)
        bt_agent_stop(app->agent);
    if (app->client)
        bt_client_stop(app->client);
    g_cancellable_cancel(app->cancel);
    g_application_quit(G_APPLICATION(app->application));
}

static void action_show(GSimpleAction *action, GVariant *parameter, gpointer data) { app_show(data); }
static void action_quit(GSimpleAction *action, GVariant *parameter, gpointer data) { app_quit(data); }
static gboolean unix_quit(gpointer data) { app_quit(data); return G_SOURCE_CONTINUE; }

static void
bus_closed(GDBusConnection *bus, gboolean remote, GError *error, gpointer data)
{
    App *app = data;
    if (app->closing)
        return;
    bt_agent_stop(app->agent);
    bt_client_stop(app->client);
    app_error(app, "The system bus connection closed. Quit and reopen Mytooth to reconnect.");
    app_show(app);
}

static void
bus_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    App *app = data;
    g_autoptr(GError) error = NULL;
    app->system_bus = g_dbus_connection_new_for_address_finish(result, &error);
    if (app->closing)
        return;
    if (!app->system_bus) {
        app_error(app, "Could not connect to the system bus. Quit and reopen Mytooth to retry.");
        return;
    }
    g_dbus_connection_set_exit_on_close(app->system_bus, FALSE);
    app->client = bt_client_new(app->system_bus);
    g_signal_connect(app->client, "changed", G_CALLBACK(model_changed), app);
    g_signal_connect(app->client, "error", G_CALLBACK(model_error), app);
    g_signal_connect(app->client, "operation", G_CALLBACK(operation_done), app);
    app->agent = bt_agent_new(app->client);
    g_signal_connect(app->agent, "changed", G_CALLBACK(model_changed), app);
    g_signal_connect(app->agent, "prompt-changed", G_CALLBACK(prompt_changed), app);
    g_signal_connect(app->system_bus, "closed", G_CALLBACK(bus_closed), app);
    app->session = session_new(g_application_get_dbus_connection(G_APPLICATION(app->application)), app->client, app->agent);
    app_refresh(app);
}

static gboolean
check_tray(gpointer data)
{
    App *app = data;
    app->tray_check = 0;
    if (!tray_supported(app->tray)) {
        app_show(app);
        app_error(app, "No native XApp tray host was found. The window will stay accessible; check the Cinnamon XApp status applet.");
    }
    return G_SOURCE_REMOVE;
}

static void
notifications_appeared(GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
    ((App *)data)->notification_service = TRUE;
}
static void
notifications_vanished(GDBusConnection *bus, const char *name, gpointer data)
{
    ((App *)data)->notification_service = FALSE;
}

static void
start(App *app)
{
    trace_startup("start entered");
    if (app->started)
        return;
    app->started = TRUE;
    g_application_hold(G_APPLICATION(app->application));
    settings_load(&app->settings);
    trace_startup("settings loaded");
    gtk_window_set_default_icon_name(MYTOOTH_ID);
    app->view = view_new(app);
    trace_startup("view created");
    app->tray = tray_new(app);
    trace_startup("tray created");
    app->radio = rfkill_new(app_refresh, app);
    trace_startup("rfkill opened");
    app->notification_watch = g_bus_watch_name_on_connection(
        g_application_get_dbus_connection(G_APPLICATION(app->application)),
        "org.freedesktop.Notifications", G_BUS_NAME_WATCHER_FLAGS_NONE,
        notifications_appeared, notifications_vanished, app, NULL);
    app->tray_check = g_timeout_add_seconds(3, check_tray, app);
    g_autoptr(GError) error = NULL;
    g_autofree char *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (address)
        g_dbus_connection_new_for_address(address,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL, app->cancel, bus_ready, app);
    else
        app_error(app, "The system bus is unavailable.");
    app_refresh(app);
    trace_startup("start complete");
    g_debug("Mytooth started using native Wayland");
}

static int
command_line(GApplication *application, GApplicationCommandLine *line, gpointer data)
{
    App *app = data;
    trace_startup("command line received");
    GVariantDict *options = g_application_command_line_get_options_dict(line);
    if (g_variant_dict_contains(options, "quit")) {
        app_quit(app);
        return 0;
    }
    if (g_variant_dict_contains(options, "debug"))
        debug_logging = TRUE;
    app->background = g_variant_dict_contains(options, "background");
    start(app);
    if (!app->background)
        app_show(app);
    trace_startup("command line complete");
    return 0;
}

static void activate(GApplication *application, gpointer data) { start(data); app_show(data); }

int
main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (g_str_equal(argv[i], "--version")) { g_print("Mytooth 0.1.0\n"); return 0; }
    }
    g_log_set_writer_func(log_writer, NULL, NULL);
    /* A process-level backend selection prevents GTK and XApp's legacy tray fallback using X11. */
    gdk_set_allowed_backends("wayland");
    g_set_prgname("mytooth");
    g_set_application_name("Mytooth");
    trace_startup("creating application");
    App app = {0};
    app.cancel = g_cancellable_new();
    app.application = gtk_application_new(MYTOOTH_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_application_add_main_option(G_APPLICATION(app.application), "background", 0, G_OPTION_FLAG_NONE,
        G_OPTION_ARG_NONE, "Start with the window hidden (requires a native XApp tray host)", NULL);
    g_application_add_main_option(G_APPLICATION(app.application), "quit", 0, G_OPTION_FLAG_NONE,
        G_OPTION_ARG_NONE, "Quit the running Mytooth instance", NULL);
    g_application_add_main_option(G_APPLICATION(app.application), "debug", 0, G_OPTION_FLAG_NONE,
        G_OPTION_ARG_NONE, "Enable diagnostic logs for this run", NULL);
    const GActionEntry actions[] = {
        { .name = "show", .activate = action_show },
        { .name = "quit", .activate = action_quit },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app.application), actions, G_N_ELEMENTS(actions), &app);
    g_signal_connect(app.application, "command-line", G_CALLBACK(command_line), &app);
    g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
    guint sigterm = g_unix_signal_add(SIGTERM, unix_quit, &app);
    guint sigint = g_unix_signal_add(SIGINT, unix_quit, &app);
    trace_startup("entering application run");
    int result = g_application_run(G_APPLICATION(app.application), argc, argv);
    app_quit(&app);
    if (app.refresh_idle) g_source_remove(app.refresh_idle);
    if (app.tray_check) g_source_remove(app.tray_check);
    if (app.notification_watch) g_bus_unwatch_name(app.notification_watch);
    g_source_remove(sigterm);
    g_source_remove(sigint);
    session_free(app.session);
    rfkill_free(app.radio);
    if (app.client) g_signal_handlers_disconnect_by_data(app.client, &app);
    if (app.agent) g_signal_handlers_disconnect_by_data(app.agent, &app);
    if (app.system_bus) g_signal_handlers_disconnect_by_data(app.system_bus, &app);
    tray_free(app.tray);
    view_free(app.view);
    g_clear_object(&app.agent);
    g_clear_object(&app.client);
    if (app.system_bus && !g_dbus_connection_is_closed(app.system_bus))
        g_dbus_connection_close_sync(app.system_bus, NULL, NULL);
    g_clear_object(&app.system_bus);
    g_clear_object(&app.cancel);
    g_clear_object(&app.application);
    g_free(app.adapter);
    return result;
}
