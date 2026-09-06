/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"
#include <libxapp/xapp-status-icon.h>

struct Tray { App *app; XAppStatusIcon *icon; GtkWidget *menu; gboolean had_host; };

static void show_manager(GtkMenuItem *item, gpointer data) { app_show(data); }
static void toggle_power(GtkMenuItem *item, gpointer data) { app_power(data); }
static void quit_app(GtkMenuItem *item, gpointer data) { app_quit(data); }

static void
device_action(GtkMenuItem *item, gpointer data)
{
    app_device_action(data, g_object_get_data(G_OBJECT(item), "device-path"));
}

static void
clicked(XAppStatusIcon *icon, gint x, gint y, guint button, guint timestamp,
        gint panel_position, gpointer data)
{
    if (button == 1)
        app_show(data);
}

static void
state_changed(XAppStatusIcon *icon, XAppStatusIconState state, gpointer data)
{
    Tray *tray = data;
    if (state == XAPP_STATUS_ICON_STATE_NATIVE)
        tray->had_host = TRUE;
    else if (tray->had_host && !tray->app->closing) {
        app_show(tray->app);
        app_error(tray->app, "The XApp tray host disappeared. Mytooth has reopened its window.");
    }
}

static void menu_hidden(GtkWidget *menu, gpointer data) { app_refresh(data); }

static GtkWidget *
item_add(Tray *tray, const char *text, GCallback action)
{
    GtkWidget *item = gtk_menu_item_new_with_label(text);
    if (action)
        g_signal_connect(item, "activate", action, tray->app);
    else
        gtk_widget_set_sensitive(item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), item);
    return item;
}

Tray *
tray_new(App *app)
{
    Tray *tray = g_new0(Tray, 1);
    tray->app = app;
    tray->icon = xapp_status_icon_new_with_name(MYTOOTH_ID);
    tray->menu = g_object_ref_sink(gtk_menu_new());
    xapp_status_icon_set_icon_name(tray->icon, "bluetooth-symbolic");
    xapp_status_icon_set_tooltip_text(tray->icon, "Mytooth — Bluetooth");
    xapp_status_icon_set_secondary_menu(tray->icon, GTK_MENU(tray->menu));
    xapp_status_icon_set_visible(tray->icon, TRUE);
    g_signal_connect(tray->icon, "button-release-event", G_CALLBACK(clicked), app);
    g_signal_connect(tray->icon, "state-changed", G_CALLBACK(state_changed), tray);
    g_signal_connect(tray->menu, "hide", G_CALLBACK(menu_hidden), app);
    tray_refresh(tray);
    return tray;
}

gboolean
tray_supported(Tray *tray)
{
    return tray && xapp_status_icon_get_state(tray->icon) == XAPP_STATUS_ICON_STATE_NATIVE;
}

void
tray_refresh(Tray *tray)
{
    if (!tray)
        return;
    App *app = tray->app;
    g_autoptr(GDBusProxy) adapter = app->client ? bt_client_proxy(app->client, app->adapter, BT_ADAPTER) : NULL;
    gboolean powered = bt_boolean(adapter, "Powered");
    g_autoptr(GPtrArray) devices = app->client ? bt_client_list(app->client, BT_DEVICE) : g_ptr_array_new();
    guint connected = 0;
    for (guint i = 0; i < devices->len; i++)
        connected += bt_boolean(g_ptr_array_index(devices, i), "Connected");
    g_autofree char *tooltip = connected ? g_strdup_printf("Mytooth · %u connected device%s", connected, connected == 1 ? "" : "s") :
        g_strdup(powered ? "Mytooth · Bluetooth on" : "Mytooth · Bluetooth off");
    xapp_status_icon_set_tooltip_text(tray->icon, tooltip);
    xapp_status_icon_set_icon_name(tray->icon, powered ? "bluetooth-active-symbolic" : "bluetooth-disabled-symbolic");
    /* Never replace a menu item under the user's pointer. Rebuild after it closes. */
    if (gtk_widget_get_mapped(tray->menu))
        return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(tray->menu));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(children);
    item_add(tray, "Open Mytooth", G_CALLBACK(show_manager));
    GtkWidget *power = item_add(tray, powered ? "Turn Bluetooth Off" : "Turn Bluetooth On", G_CALLBACK(toggle_power));
    gtk_widget_set_sensitive(power, adapter && !bt_client_busy(app->client, app->adapter));
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), gtk_separator_menu_item_new());
    guint count = 0;
    for (guint i = 0; i < devices->len; i++) {
        GDBusProxy *device = g_ptr_array_index(devices, i);
        g_autofree char *parent = bt_string(device, "Adapter", "");
        if (g_strcmp0(parent, app->adapter) != 0 ||
            (!bt_boolean(device, "Paired") && !bt_boolean(device, "Connected")))
            continue;
        if (count++ >= 10)
            continue;
        const char *path = g_dbus_proxy_get_object_path(device);
        g_autofree char *name = bt_string(device, "Alias", "Bluetooth device");
        g_autofree char *short_name = g_utf8_substring(name, 0, MIN(g_utf8_strlen(name, -1), 80));
        g_autofree char *text = g_strdup_printf("%s · %s",
            bt_boolean(device, "Connected") ? "Disconnect" : "Connect", short_name);
        GtkWidget *item = item_add(tray, text, G_CALLBACK(device_action));
        g_object_set_data_full(G_OBJECT(item), "device-path", g_strdup(path), g_free);
        gtk_widget_set_sensitive(item, powered && !bt_boolean(device, "Blocked") && !bt_client_busy(app->client, path));
    }
    if (!count)
        item_add(tray, "No saved devices", NULL);
    if (count > 10)
        item_add(tray, "More devices in Mytooth…", G_CALLBACK(show_manager));
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), gtk_separator_menu_item_new());
    item_add(tray, "Quit Mytooth", G_CALLBACK(quit_app));
    gtk_widget_show_all(tray->menu);
}

void
tray_free(Tray *tray)
{
    if (!tray)
        return;
    g_signal_handlers_disconnect_by_data(tray->icon, tray);
    g_signal_handlers_disconnect_by_data(tray->icon, tray->app);
    g_signal_handlers_disconnect_by_data(tray->menu, tray->app);
    xapp_status_icon_set_visible(tray->icon, FALSE);
    xapp_status_icon_set_secondary_menu(tray->icon, NULL);
    g_object_unref(tray->icon);
    gtk_widget_destroy(tray->menu);
    g_object_unref(tray->menu);
    g_free(tray);
}
