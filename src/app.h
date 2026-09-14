/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>
#include "agent.h"
#include "settings.h"
#include "rfkill.h"
#include "session.h"

typedef struct View View;
typedef struct Tray Tray;
typedef struct {
    GtkApplication *application;
    GDBusConnection *system_bus;
    BtClient *client;
    BtAgent *agent;
    GCancellable *cancel;
    Session *session;
    Rfkill *radio;
    View *view;
    Tray *tray;
    Settings settings;
    gboolean closing;
    gboolean started;
    gboolean background;
    gboolean notification_service;
    guint notification_watch;
    guint refresh_idle;
    guint tray_check;
    char *adapter;
} App;

void app_show(App *app);
void app_error(App *app, const char *message);
void app_notify(App *app, const char *id, const char *title, const char *body);
void app_quit(App *app);
gboolean app_refresh(gpointer data);
void app_device_action(App *app, const char *path);
void app_power(App *app);
void app_choose_adapter(App *app, const char *path);

View *view_new(App *app);
void view_free(View *view);
void view_show(View *view);
void view_refresh(View *view);
void view_error(View *view, const char *message);
GtkWindow *view_window(View *view);
void view_pairing(View *view);

Tray *tray_new(App *app);
void tray_free(Tray *tray);
void tray_refresh(Tray *tray);
gboolean tray_supported(Tray *tray);
