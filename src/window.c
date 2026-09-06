/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"
#include <string.h>

typedef struct {
    View *view;
    char *path;
    char *search;
    GtkWidget *row;
    GtkWidget *name;
    GtkWidget *status;
    GtkWidget *icon;
    GtkWidget *action;
    GtkWidget *details;
    guint order;
    gboolean saved;
    gboolean present;
} DeviceRow;

struct View {
    App *app;
    GtkWidget *window;
    GtkWidget *adapter;
    GtkWidget *power;
    GtkWidget *scan;
    GtkWidget *mode;
    GtkWidget *search;
    GtkWidget *list;
    GtkWidget *summary;
    GtkWidget *empty;
    GtkWidget *error_bar;
    GtkWidget *error_text;
    GtkWidget *agent_status;
    GHashTable *rows;
    guint next_order;
    gboolean updating;
    gboolean add_mode;
    char *filter;
    GtkWidget *pair_dialog;
    GtkWidget *pair_text;
    GtkWidget *pair_entry;
    GtkWidget *pair_error;
    BtPrompt pair_kind;
    GtkWidget *details;
    char *detail_path;
    GtkWidget *detail_info;
    GtkWidget *detail_name;
    GtkWidget *detail_trust;
    GtkWidget *detail_block;
    GtkWidget *detail_save;
    GtkWidget *detail_forget;
};

static GtkWidget *
label(const char *text, const char *style)
{
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    if (style)
        gtk_style_context_add_class(gtk_widget_get_style_context(widget), style);
    return widget;
}

static GtkWidget *
button(const char *text, GCallback callback, gpointer data)
{
    GtkWidget *widget = gtk_button_new_with_label(text);
    g_signal_connect(widget, "clicked", callback, data);
    return widget;
}

static void
save_preferences(View *view)
{
    g_autoptr(GError) error = NULL;
    if (!settings_save(&view->app->settings, &error))
        app_error(view->app, "Could not save Mytooth preferences. Check your configuration directory permissions.");
}

static gboolean
window_closed(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    View *view = data;
    gtk_window_get_size(GTK_WINDOW(widget), &view->app->settings.width, &view->app->settings.height);
    save_preferences(view);
    if (view->app->client)
        bt_client_scan(view->app->client, NULL);
    if (tray_supported(view->app->tray))
        gtk_widget_hide(widget);
    else
        app_quit(view->app);
    return TRUE;
}

static void
adapter_selected(GtkComboBox *combo, gpointer data)
{
    View *view = data;
    if (!view->updating)
        app_choose_adapter(view->app, gtk_combo_box_get_active_id(combo));
}

static gboolean
power_changed(GtkSwitch *widget, gboolean state, gpointer data)
{
    View *view = data;
    if (!view->updating)
        app_power(view->app);
    return TRUE; /* Daemon state updates the switch after completion. */
}

static void
scan_clicked(GtkButton *widget, gpointer data)
{
    View *view = data;
    if (!view->app->client)
        return;
    bt_client_scan(view->app->client,
        bt_client_scan_path(view->app->client) ? NULL : view->app->adapter);
}

static gboolean
row_visible(GtkListBoxRow *widget, gpointer data)
{
    View *view = data;
    DeviceRow *row = g_object_get_data(G_OBJECT(widget), "device-row");
    return row && (view->add_mode ? !row->saved : row->saved) &&
        (!view->filter || !*view->filter || strstr(row->search, view->filter));
}

static void
empty_state(View *view)
{
    guint count = 0;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, view->rows);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        count += row_visible(GTK_LIST_BOX_ROW(((DeviceRow *)value)->row), view);
    gtk_label_set_text(GTK_LABEL(view->empty), view->add_mode ?
        "No nearby devices found.\nPut your device in pairing mode, then scan." :
        "No saved devices match.\nChoose Add Device to pair your first device.");
    gtk_widget_set_visible(view->empty, count == 0);
}

static void
search_changed(GtkSearchEntry *entry, gpointer data)
{
    View *view = data;
    g_free(view->filter);
    view->filter = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(entry)), -1);
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(view->list));
    empty_state(view);
}

static void
mode_changed(GtkToggleButton *widget, gpointer data)
{
    View *view = data;
    view->add_mode = gtk_toggle_button_get_active(widget);
    gtk_button_set_label(GTK_BUTTON(widget), view->add_mode ? "My Devices" : "Add Device");
    gtk_entry_set_text(GTK_ENTRY(view->search), "");
    if (view->app->client) {
        g_autoptr(GDBusProxy) adapter = bt_client_proxy(view->app->client, view->app->adapter, BT_ADAPTER);
        bt_client_scan(view->app->client,
            view->add_mode && bt_boolean(adapter, "Powered") ? view->app->adapter : NULL);
    }
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(view->list));
    view_refresh(view);
}

static void row_action(GtkButton *widget, gpointer data) { DeviceRow *row = data; app_device_action(row->view->app, row->path); }

static void
detail_closed(GtkWidget *widget, gpointer data)
{
    View *view = data;
    view->details = NULL;
    g_clear_pointer(&view->detail_path, g_free);
}

static void
dialog_close(GtkDialog *dialog, gint response, gpointer data)
{
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
detail_rename(GtkButton *button_widget, gpointer data)
{
    View *view = data;
    const char *name = gtk_entry_get_text(GTK_ENTRY(view->detail_name));
    bt_client_set(view->app->client, view->detail_path, BT_DEVICE, "Alias", g_variant_new_string(name));
}

static void
detail_toggle(GtkToggleButton *widget, gpointer data)
{
    View *view = data;
    if (view->updating || !view->detail_path)
        return;
    const char *property = GTK_WIDGET(widget) == view->detail_trust ? "Trusted" : "Blocked";
    bt_client_set(view->app->client, view->detail_path, BT_DEVICE, property,
        g_variant_new_boolean(gtk_toggle_button_get_active(widget)));
    app_refresh(view->app);
}

typedef struct { App *app; char *device; char *adapter; } Forget;

static void
forget_free(gpointer data)
{
    Forget *request = data;
    g_free(request->device);
    g_free(request->adapter);
    g_free(request);
}

static void
forget_response(GtkDialog *dialog, gint response, gpointer data)
{
    Forget *request = data;
    if (response == GTK_RESPONSE_ACCEPT && !request->app->closing)
        bt_client_method(request->app->client, request->adapter, BT_ADAPTER,
            "RemoveDevice", g_variant_new("(o)", request->device));
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
detail_forget(GtkButton *widget, gpointer data)
{
    View *view = data;
    g_autoptr(GDBusProxy) device = bt_client_proxy(view->app->client, view->detail_path, BT_DEVICE);
    if (!device)
        return;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(view->details),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_CANCEL, "%s", "Forget this device?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
        "Pairing information will be removed. You will need to pair again to reconnect.");
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Forget Device", GTK_RESPONSE_ACCEPT);
    Forget *request = g_new0(Forget, 1);
    request->app = view->app;
    request->device = g_strdup(view->detail_path);
    request->adapter = bt_string(device, "Adapter", "");
    g_object_set_data_full(G_OBJECT(dialog), "request", request, forget_free);
    g_signal_connect(dialog, "response", G_CALLBACK(forget_response), request);
    gtk_widget_show(dialog);
}

static void
refresh_details(View *view)
{
    if (!view->details)
        return;
    g_autoptr(GDBusProxy) device = bt_client_proxy(view->app->client, view->detail_path, BT_DEVICE);
    if (!device) {
        gtk_widget_destroy(view->details);
        return;
    }
    g_autofree char *address = bt_string(device, "Address", "Unknown");
    g_autofree char *type = bt_string(device, "AddressType", "Unknown");
    g_autoptr(GDBusProxy) battery = bt_client_proxy(view->app->client, view->detail_path, BT_BATTERY);
    int level = bt_battery(battery);
    g_autofree char *battery_text = level >= 0 ? g_strdup_printf("%d%%", level) : g_strdup("Not reported");
    g_autoptr(GString) text = g_string_new(NULL);
    g_string_append_printf(text, "Address: %s (%s)\nPaired: %s\nConnected: %s\nBattery: %s\nServices: %s",
        address, type, bt_boolean(device, "Paired") ? "Yes" : "No",
        bt_boolean(device, "Connected") ? "Yes" : "No", battery_text,
        bt_boolean(device, "ServicesResolved") ? "Resolved" : "Not resolved");
    g_autoptr(GVariant) uuids = g_dbus_proxy_get_cached_property(device, "UUIDs");
    if (uuids && g_variant_is_of_type(uuids, G_VARIANT_TYPE_STRING_ARRAY)) {
        GVariantIter iter;
        const char *uuid;
        guint count = 0;
        g_variant_iter_init(&iter, uuids);
        while (g_variant_iter_next(&iter, "&s", &uuid) && count++ < 32)
            g_string_append_printf(text, "\n%.128s", uuid);
    }
    gtk_label_set_text(GTK_LABEL(view->detail_info), text->str);
    gboolean busy = bt_client_busy(view->app->client, view->detail_path);
    view->updating = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->detail_trust), bt_boolean(device, "Trusted"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->detail_block), bt_boolean(device, "Blocked"));
    view->updating = FALSE;
    gtk_widget_set_sensitive(view->detail_trust, !busy);
    gtk_widget_set_sensitive(view->detail_block, !busy);
    gtk_widget_set_sensitive(view->detail_save, !busy);
    gtk_widget_set_sensitive(view->detail_forget, !busy);
}

static void
show_details(GtkButton *widget, gpointer data)
{
    DeviceRow *row = data;
    View *view = row->view;
    if (view->details)
        gtk_widget_destroy(view->details);
    g_autoptr(GDBusProxy) device = bt_client_proxy(view->app->client, row->path, BT_DEVICE);
    if (!device)
        return;
    view->detail_path = g_strdup(row->path);
    view->details = gtk_dialog_new_with_buttons("Device Details", GTK_WINDOW(view->window),
        GTK_DIALOG_DESTROY_WITH_PARENT, "Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(view->details), 480, 440);
    g_signal_connect(view->details, "response", G_CALLBACK(dialog_close), NULL);
    g_signal_connect(view->details, "destroy", G_CALLBACK(detail_closed), view);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(view->details));
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    gtk_box_set_spacing(GTK_BOX(box), 12);
    gtk_box_pack_start(GTK_BOX(box), label("Device name", "heading"), FALSE, FALSE, 0);
    GtkWidget *rename = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view->detail_name = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(view->detail_name), 128);
    g_autofree char *name = bt_string(device, "Alias", "");
    gtk_entry_set_text(GTK_ENTRY(view->detail_name), name);
    gtk_box_pack_start(GTK_BOX(rename), view->detail_name, TRUE, TRUE, 0);
    view->detail_save = button("Save Name", G_CALLBACK(detail_rename), view);
    gtk_box_pack_start(GTK_BOX(rename), view->detail_save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), rename, FALSE, FALSE, 0);
    view->detail_trust = gtk_check_button_new_with_label("Trust this device for future connections");
    view->detail_block = gtk_check_button_new_with_label("Block this device");
    g_signal_connect(view->detail_trust, "toggled", G_CALLBACK(detail_toggle), view);
    g_signal_connect(view->detail_block, "toggled", G_CALLBACK(detail_toggle), view);
    gtk_box_pack_start(GTK_BOX(box), view->detail_trust, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), view->detail_block, FALSE, FALSE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    view->detail_info = label("", "dim-label");
    gtk_label_set_selectable(GTK_LABEL(view->detail_info), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(view->detail_info), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(view->detail_info), PANGO_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scroll), view->detail_info);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    view->detail_forget = button("Forget Device…", G_CALLBACK(detail_forget), view);
    gtk_style_context_add_class(gtk_widget_get_style_context(view->detail_forget), "destructive-action");
    gtk_box_pack_start(GTK_BOX(box), view->detail_forget, FALSE, FALSE, 0);
    refresh_details(view);
    gtk_widget_show_all(view->details);
}

static void
row_free(gpointer data)
{
    DeviceRow *row = data;
    gtk_widget_destroy(row->row);
    g_free(row->path);
    g_free(row->search);
    g_free(row);
}

static DeviceRow *
row_new(View *view, GDBusProxy *device)
{
    DeviceRow *row = g_new0(DeviceRow, 1);
    row->view = view;
    row->path = g_strdup(g_dbus_proxy_get_object_path(device));
    row->search = g_strdup("");
    row->order = view->next_order++;
    row->row = gtk_list_box_row_new();
    g_object_set_data(G_OBJECT(row->row), "device-row", row);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    row->icon = gtk_image_new_from_icon_name("bluetooth-symbolic", GTK_ICON_SIZE_DND);
    gtk_box_pack_start(GTK_BOX(box), row->icon, FALSE, FALSE, 0);
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    row->name = label("", "heading");
    gtk_label_set_ellipsize(GTK_LABEL(row->name), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(row->name), 36);
    row->status = label("", "dim-label");
    gtk_box_pack_start(GTK_BOX(labels), row->name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(labels), row->status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), labels, TRUE, TRUE, 0);
    row->action = button("Connect", G_CALLBACK(row_action), row);
    gtk_widget_set_valign(row->action, GTK_ALIGN_CENTER);
    row->details = button("Details", G_CALLBACK(show_details), row);
    gtk_widget_set_valign(row->details, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), row->action, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row->details, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row->row), box);
    gtk_container_add(GTK_CONTAINER(view->list), row->row);
    gtk_widget_show_all(row->row);
    return row;
}

static void
update_row(DeviceRow *row, GDBusProxy *device, gboolean powered)
{
    App *app = row->view->app;
    g_autofree char *address = bt_string(device, "Address", "Unknown device");
    g_autofree char *name = bt_string(device, "Alias", address);
    g_autofree char *icon = bt_string(device, "Icon", "bluetooth-symbolic");
    /* Icon names come from BlueZ; never interpret them as filenames. */
    if (!gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), icon)) {
        g_free(icon);
        icon = g_strdup("bluetooth-symbolic");
    }
    gtk_image_set_from_icon_name(GTK_IMAGE(row->icon), icon, GTK_ICON_SIZE_DND);
    gtk_label_set_text(GTK_LABEL(row->name), name);
    g_free(row->search);
    g_autofree char *search = g_strconcat(name, " ", address, NULL);
    row->search = g_utf8_casefold(search, -1);
    gboolean connected = bt_boolean(device, "Connected");
    gboolean paired = bt_boolean(device, "Paired");
    gboolean blocked = bt_boolean(device, "Blocked");
    row->saved = paired || connected || blocked || bt_boolean(device, "Trusted");
    row->present = TRUE;
    g_autoptr(GDBusProxy) battery = bt_client_proxy(app->client, row->path, BT_BATTERY);
    int level = bt_battery(battery);
    const char *pending = bt_client_pending(app->client, row->path);
    const char *state = blocked ? "Blocked" : connected ? "Connected" : paired ? "Paired · Disconnected" : "Available to pair";
    if (pending) {
        state = g_str_equal(pending, "Pair") ? "Pairing…" :
            g_str_equal(pending, "Connect") ? "Connecting…" :
            g_str_equal(pending, "Disconnect") ? "Disconnecting…" : "Updating…";
    }
    g_autofree char *status = level >= 0 ? g_strdup_printf("%s · %d%% battery", state, level) : g_strdup(state);
    gtk_label_set_text(GTK_LABEL(row->status), status);
    gboolean pairing = g_strcmp0(pending, "Pair") == 0;
    gtk_button_set_label(GTK_BUTTON(row->action), pairing ? "Cancel Pairing" : connected ? "Disconnect" : paired ? "Connect" : "Pair");
    gtk_widget_set_sensitive(row->action, pairing || (!pending && powered && !blocked &&
        (paired || connected || (app->agent && bt_agent_ready(app->agent) && !bt_agent_locked(app->agent)))));
}

static gint
initial_device_order(gconstpointer left, gconstpointer right)
{
    GDBusProxy *a = *(GDBusProxy *const *)left;
    GDBusProxy *b = *(GDBusProxy *const *)right;
    int connected = (int)bt_boolean(b, "Connected") - (int)bt_boolean(a, "Connected");
    if (connected)
        return connected;
    return g_strcmp0(g_dbus_proxy_get_object_path(a), g_dbus_proxy_get_object_path(b));
}

static void
notifications_toggled(GtkCheckMenuItem *item, gpointer data)
{
    View *view = data;
    view->app->settings.notifications = gtk_check_menu_item_get_active(item);
    save_preferences(view);
}

static void
autostart_toggled(GtkCheckMenuItem *item, gpointer data)
{
    View *view = data;
    if (view->updating)
        return;
    g_autoptr(GError) error = NULL;
    if (!settings_set_autostart(gtk_check_menu_item_get_active(item), &error)) {
        view->updating = TRUE;
        gtk_check_menu_item_set_active(item, settings_autostart());
        view->updating = FALSE;
        app_error(view->app, "Could not save the login startup setting.");
    }
}

static void retry_agent(GtkMenuItem *item, gpointer data) { App *app = data; if (app->agent) bt_agent_retry(app->agent); }
static void quit_clicked(GtkMenuItem *item, gpointer data) { app_quit(data); }

typedef struct { View *view; char *path; GtkWidget *name; GtkWidget *visible; } AdapterDialog;

static void
adapter_dialog_free(gpointer data)
{
    AdapterDialog *dialog = data;
    g_free(dialog->path);
    g_free(dialog);
}

static void
adapter_rename(GtkButton *widget, gpointer data)
{
    AdapterDialog *dialog = data;
    bt_client_set(dialog->view->app->client, dialog->path, BT_ADAPTER, "Alias",
        g_variant_new_string(gtk_entry_get_text(GTK_ENTRY(dialog->name))));
}

static void
adapter_visible(GtkButton *widget, gpointer data)
{
    AdapterDialog *dialog = data;
    g_autoptr(GDBusProxy) adapter = bt_client_proxy(dialog->view->app->client, dialog->path, BT_ADAPTER);
    if (adapter)
        bt_client_discoverable(dialog->view->app->client, dialog->path, !bt_boolean(adapter, "Discoverable"));
}

static void
adapter_settings(GtkMenuItem *item, gpointer data)
{
    View *view = data;
    if (!view->app->client || !view->app->adapter)
        return;
    g_autoptr(GDBusProxy) adapter = bt_client_proxy(view->app->client, view->app->adapter, BT_ADAPTER);
    if (!adapter)
        return;
    GtkWidget *widget = gtk_dialog_new_with_buttons("Adapter Settings", GTK_WINDOW(view->window),
        GTK_DIALOG_DESTROY_WITH_PARENT, "Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(widget), 440, 200);
    AdapterDialog *dialog = g_new0(AdapterDialog, 1);
    dialog->view = view;
    dialog->path = g_strdup(view->app->adapter);
    g_object_set_data_full(G_OBJECT(widget), "adapter-dialog", dialog, adapter_dialog_free);
    g_signal_connect(widget, "response", G_CALLBACK(dialog_close), NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(widget));
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    gtk_box_set_spacing(GTK_BOX(box), 12);
    gtk_box_pack_start(GTK_BOX(box), label("Computer name shown to nearby devices", NULL), FALSE, FALSE, 0);
    dialog->name = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(dialog->name), 128);
    g_autofree char *name = bt_string(adapter, "Alias", "");
    gtk_entry_set_text(GTK_ENTRY(dialog->name), name);
    gtk_box_pack_start(GTK_BOX(box), dialog->name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), button("Save Name", G_CALLBACK(adapter_rename), dialog), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), button("Toggle Visibility (2 minutes when enabled)", G_CALLBACK(adapter_visible), dialog), FALSE, FALSE, 0);
    gtk_widget_show_all(widget);
}

static GtkWidget *
make_menu(View *view)
{
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *adapter = gtk_menu_item_new_with_label("Adapter Settings…");
    GtkWidget *notifications = gtk_check_menu_item_new_with_label("Show notifications");
    GtkWidget *autostart = gtk_check_menu_item_new_with_label("Start at login");
    GtkWidget *retry = gtk_menu_item_new_with_label("Retry pairing agent");
    GtkWidget *quit = gtk_menu_item_new_with_label("Quit Mytooth");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(notifications), view->app->settings.notifications);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(autostart), settings_autostart());
    g_signal_connect(adapter, "activate", G_CALLBACK(adapter_settings), view);
    g_signal_connect(notifications, "toggled", G_CALLBACK(notifications_toggled), view);
    g_signal_connect(autostart, "toggled", G_CALLBACK(autostart_toggled), view);
    g_signal_connect(retry, "activate", G_CALLBACK(retry_agent), view->app);
    g_signal_connect(quit, "activate", G_CALLBACK(quit_clicked), view->app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), adapter);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), notifications);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), autostart);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), retry);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
    gtk_widget_show_all(menu);
    return menu;
}

static void dismiss_error(GtkInfoBar *bar, gint response, gpointer data) { gtk_widget_hide(GTK_WIDGET(bar)); }

View *
view_new(App *app)
{
    View *view = g_new0(View, 1);
    view->app = app;
    view->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, row_free);
    view->window = gtk_application_window_new(app->application);
    gtk_window_set_title(GTK_WINDOW(view->window), "Mytooth — Bluetooth");
    gtk_window_set_default_size(GTK_WINDOW(view->window), app->settings.width, app->settings.height);
    gtk_widget_set_size_request(view->window, 600, 420);
    g_signal_connect(view->window, "delete-event", G_CALLBACK(window_closed), view);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(view->window), root);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 18);
    GtkWidget *title = label("Bluetooth", "title");
    gtk_box_pack_start(GTK_BOX(toolbar), title, FALSE, FALSE, 0);
    view->adapter = gtk_combo_box_text_new();
    g_signal_connect(view->adapter, "changed", G_CALLBACK(adapter_selected), view);
    gtk_box_pack_start(GTK_BOX(toolbar), view->adapter, TRUE, TRUE, 0);
    view->power = gtk_switch_new();
    gtk_widget_set_valign(view->power, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(view->power, "Bluetooth power");
    g_signal_connect(view->power, "state-set", G_CALLBACK(power_changed), view);
    gtk_box_pack_start(GTK_BOX(toolbar), view->power, FALSE, FALSE, 0);
    GtkWidget *menu = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(menu), gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(menu), make_menu(view));
    gtk_box_pack_start(GTK_BOX(toolbar), menu, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    view->error_bar = gtk_info_bar_new();
    gtk_info_bar_set_message_type(GTK_INFO_BAR(view->error_bar), GTK_MESSAGE_WARNING);
    gtk_info_bar_set_show_close_button(GTK_INFO_BAR(view->error_bar), TRUE);
    view->error_text = label("", NULL);
    gtk_label_set_line_wrap(GTK_LABEL(view->error_text), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(view->error_text), 65);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(view->error_bar))), view->error_text);
    g_signal_connect(view->error_bar, "response", G_CALLBACK(dismiss_error), NULL);
    gtk_box_pack_start(GTK_BOX(root), view->error_bar, FALSE, FALSE, 0);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(content), 24);
    gtk_box_pack_start(GTK_BOX(root), content, TRUE, TRUE, 0);
    view->summary = label("Connecting to the Bluetooth service…", "heading");
    gtk_label_set_line_wrap(GTK_LABEL(view->summary), TRUE);
    gtk_box_pack_start(GTK_BOX(content), view->summary, FALSE, FALSE, 0);
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    view->search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(view->search), "Search devices");
    g_signal_connect(view->search, "search-changed", G_CALLBACK(search_changed), view);
    gtk_box_pack_start(GTK_BOX(controls), view->search, TRUE, TRUE, 0);
    view->scan = button("Scan", G_CALLBACK(scan_clicked), view);
    gtk_box_pack_start(GTK_BOX(controls), view->scan, FALSE, FALSE, 0);
    view->mode = gtk_toggle_button_new_with_label("Add Device");
    gtk_style_context_add_class(gtk_widget_get_style_context(view->mode), "suggested-action");
    g_signal_connect(view->mode, "toggled", G_CALLBACK(mode_changed), view);
    gtk_box_pack_start(GTK_BOX(controls), view->mode, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), controls, FALSE, FALSE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    view->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(view->list), GTK_SELECTION_NONE);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(view->list), row_visible, view, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), view->list);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);
    view->empty = label("", "dim-label");
    gtk_label_set_justify(GTK_LABEL(view->empty), GTK_JUSTIFY_CENTER);
    gtk_label_set_xalign(GTK_LABEL(view->empty), 0.5);
    gtk_box_pack_start(GTK_BOX(content), view->empty, FALSE, FALSE, 8);
    view->agent_status = label("", "dim-label");
    gtk_label_set_line_wrap(GTK_LABEL(view->agent_status), TRUE);
    gtk_box_pack_start(GTK_BOX(content), view->agent_status, FALSE, FALSE, 0);
    g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        ".title { font-size: 18px; font-weight: bold; } .heading { font-weight: bold; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(view->window),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_widget_show_all(root);
    gtk_widget_hide(view->error_bar);
    gtk_widget_hide(view->scan);
    return view;
}

void
view_refresh(View *view)
{
    if (!view)
        return;
    App *app = view->app;
    view->updating = TRUE;
    g_autoptr(GPtrArray) adapters = app->client ? bt_client_list(app->client, BT_ADAPTER) : g_ptr_array_new();
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(view->adapter));
    for (guint i = 0; i < adapters->len; i++) {
        GDBusProxy *adapter = g_ptr_array_index(adapters, i);
        g_autofree char *name = bt_string(adapter, "Alias", "Bluetooth adapter");
        g_autofree char *address = bt_string(adapter, "Address", "");
        g_autofree char *title = g_strdup_printf("%s · %s", name, address);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->adapter), g_dbus_proxy_get_object_path(adapter), title);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->adapter), app->adapter);
    gtk_widget_set_sensitive(view->adapter, adapters->len > 1);
    g_autoptr(GDBusProxy) adapter = app->client ? bt_client_proxy(app->client, app->adapter, BT_ADAPTER) : NULL;
    gboolean powered = bt_boolean(adapter, "Powered");
    RadioState radio = rfkill_state(app->radio, app->adapter);
    gtk_switch_set_state(GTK_SWITCH(view->power), powered);
    gtk_switch_set_active(GTK_SWITCH(view->power), powered);
    gtk_widget_set_sensitive(view->power, adapter && !bt_client_busy(app->client, app->adapter));
    const char *summary = !app->client || !bt_client_owner(app->client) ? "The Bluetooth service is unavailable." :
        !adapter ? "No Bluetooth adapter found. Connect an adapter to get started." :
        radio == RADIO_HARD_BLOCKED ? "Bluetooth is blocked by a hardware switch." :
        radio == RADIO_SOFT_BLOCKED ? "Bluetooth is blocked by software. Use the power switch to unblock it." :
        !powered ? "Bluetooth is off." : bt_boolean(adapter, "Discoverable") ?
        "Bluetooth is on · Visible to nearby devices" :
        view->add_mode ? "Nearby devices · Put your device in pairing mode" : "My Devices";
    gtk_label_set_text(GTK_LABEL(view->summary), summary);
    gboolean scanning = app->client && bt_client_scan_path(app->client);
    gtk_button_set_label(GTK_BUTTON(view->scan), scanning ? "Stop Scan" : "Scan");
    gtk_widget_set_visible(view->scan, view->add_mode);
    gtk_widget_set_sensitive(view->scan, powered);
    gtk_widget_set_sensitive(view->mode, adapter != NULL);
    const char *agent_text = !app->agent ? "" : !bt_agent_ready(app->agent) ? "Pairing agent is not ready." :
        bt_agent_locked(app->agent) ? "Pairing is paused while the session is locked or its lock status is unavailable." :
        !bt_agent_is_default(app->agent) ? "Incoming pairing requests may be handled by another Bluetooth manager." :
        scanning ? "Scanning stops automatically after 30 seconds." :
        "Closing this window keeps Mytooth in the tray when a native tray host is available.";
    gtk_label_set_text(GTK_LABEL(view->agent_status), agent_text);
    view->updating = FALSE;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, view->rows);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        ((DeviceRow *)value)->present = FALSE;
    g_autoptr(GPtrArray) devices = app->client ? bt_client_list(app->client, BT_DEVICE) : g_ptr_array_new();
    g_ptr_array_sort(devices, initial_device_order);
    for (guint i = 0; i < devices->len; i++) {
        GDBusProxy *device = g_ptr_array_index(devices, i);
        g_autofree char *device_adapter = bt_string(device, "Adapter", "");
        if (g_strcmp0(device_adapter, app->adapter) != 0)
            continue;
        const char *path = g_dbus_proxy_get_object_path(device);
        DeviceRow *row = g_hash_table_lookup(view->rows, path);
        if (!row) {
            row = row_new(view, device);
            g_hash_table_insert(view->rows, g_strdup(path), row);
        }
        update_row(row, device, powered);
    }
    g_hash_table_iter_init(&iter, view->rows);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        if (!((DeviceRow *)value)->present)
            g_hash_table_iter_remove(&iter);
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(view->list));
    empty_state(view);
    refresh_details(view);
}

void view_show(View *view) { gtk_window_present(GTK_WINDOW(view->window)); }
GtkWindow *view_window(View *view) { return GTK_WINDOW(view->window); }
void view_error(View *view, const char *message) { if (view) { gtk_label_set_text(GTK_LABEL(view->error_text), message); gtk_widget_show(view->error_bar); } }

static void
pair_response(GtkDialog *dialog, gint response, gpointer data)
{
    View *view = data;
    const char *text = view->pair_entry ? gtk_entry_get_text(GTK_ENTRY(view->pair_entry)) : NULL;
    if (!bt_agent_answer(view->app->agent, response == GTK_RESPONSE_ACCEPT, text)) {
        gtk_label_set_text(GTK_LABEL(view->pair_error), "Enter a valid PIN or a passkey of one to six digits.");
        gtk_widget_show(view->pair_error);
    }
}

void
view_pairing(View *view)
{
    BtAgent *agent = view->app->agent;
    BtPrompt kind = bt_agent_prompt(agent);
    if (kind == BT_PROMPT_NONE) {
        if (view->pair_dialog)
            gtk_widget_destroy(view->pair_dialog);
        view->pair_dialog = NULL;
        view->pair_kind = BT_PROMPT_NONE;
        return;
    }
    if (!view->pair_dialog) {
        view_show(view);
        view->pair_dialog = gtk_dialog_new_with_buttons("Bluetooth Pairing", GTK_WINDOW(view->window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Reject", GTK_RESPONSE_REJECT, NULL);
        if (kind != BT_PROMPT_DISPLAY)
            gtk_dialog_add_button(GTK_DIALOG(view->pair_dialog), "Allow", GTK_RESPONSE_ACCEPT);
        gtk_dialog_set_default_response(GTK_DIALOG(view->pair_dialog), GTK_RESPONSE_REJECT);
        gtk_window_set_default_size(GTK_WINDOW(view->pair_dialog), 440, 240);
        g_signal_connect(view->pair_dialog, "response", G_CALLBACK(pair_response), view);
        GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(view->pair_dialog));
        gtk_container_set_border_width(GTK_CONTAINER(box), 24);
        gtk_box_set_spacing(GTK_BOX(box), 16);
        view->pair_text = label("", NULL);
        gtk_label_set_line_wrap(GTK_LABEL(view->pair_text), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(view->pair_text), 48);
        gtk_label_set_line_wrap_mode(GTK_LABEL(view->pair_text), PANGO_WRAP_WORD_CHAR);
        gtk_box_pack_start(GTK_BOX(box), view->pair_text, FALSE, FALSE, 0);
        view->pair_entry = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(view->pair_entry), kind == BT_PROMPT_PASSKEY ? 6 : 16);
        gtk_entry_set_input_purpose(GTK_ENTRY(view->pair_entry), kind == BT_PROMPT_PASSKEY ? GTK_INPUT_PURPOSE_DIGITS : GTK_INPUT_PURPOSE_FREE_FORM);
        gtk_box_pack_start(GTK_BOX(box), view->pair_entry, FALSE, FALSE, 0);
        view->pair_error = label("", "error");
        gtk_label_set_line_wrap(GTK_LABEL(view->pair_error), TRUE);
        gtk_box_pack_start(GTK_BOX(box), view->pair_error, FALSE, FALSE, 0);
        view->pair_kind = kind;
        gtk_widget_show_all(view->pair_dialog);
        gtk_widget_set_visible(view->pair_entry, kind == BT_PROMPT_PIN || kind == BT_PROMPT_PASSKEY);
        gtk_widget_hide(view->pair_error);
    }
    g_autoptr(GDBusProxy) device = bt_client_proxy(view->app->client, bt_agent_device(agent), BT_DEVICE);
    g_autofree char *name = bt_string(device, "Alias", "Bluetooth device");
    g_autofree char *address = bt_string(device, "Address", "Unknown address");
    g_autofree char *short_name = g_utf8_substring(name, 0, MIN(g_utf8_strlen(name, -1), 200));
    g_autofree char *text = g_strdup_printf("%s\n%s\n\n%s", short_name, address, bt_agent_text(agent));
    gtk_label_set_text(GTK_LABEL(view->pair_text), text);
}

void
view_free(View *view)
{
    if (!view)
        return;
    g_hash_table_unref(view->rows);
    gtk_widget_destroy(view->window);
    g_free(view->filter);
    g_free(view->detail_path);
    g_free(view);
}
