/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "settings.h"
#include "util.h"
#include <glib/gstdio.h>
#include <errno.h>

static gboolean
save_file(const char *directory, const char *path, const char *text, GError **error)
{
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot create settings directory");
        return FALSE;
    }
    return g_file_set_contents_full(path, text, -1,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
}

void
settings_load(Settings *settings)
{
    *settings = (Settings) { .notifications = TRUE, .width = 840, .height = 620 };
    g_autofree char *path = g_build_filename(g_get_user_config_dir(), "mytooth", "settings.ini", NULL);
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
        return;
    g_autoptr(GError) error = NULL;
    gint width = g_key_file_get_integer(file, "Window", "width", &error);
    if (!error)
        settings->width = CLAMP(width, 600, 2400);
    g_clear_error(&error);
    gint height = g_key_file_get_integer(file, "Window", "height", &error);
    if (!error)
        settings->height = CLAMP(height, 420, 1600);
    g_clear_error(&error);
    gboolean notifications = g_key_file_get_boolean(file, "General", "notifications", &error);
    if (!error)
        settings->notifications = notifications;
}

gboolean
settings_save(const Settings *settings, GError **error)
{
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_key_file_set_integer(file, "Window", "width", settings->width);
    g_key_file_set_integer(file, "Window", "height", settings->height);
    g_key_file_set_boolean(file, "General", "notifications", settings->notifications);
    g_autofree char *text = g_key_file_to_data(file, NULL, NULL);
    g_autofree char *directory = g_build_filename(g_get_user_config_dir(), "mytooth", NULL);
    g_autofree char *path = g_build_filename(directory, "settings.ini", NULL);
    return save_file(directory, path, text, error);
}

gboolean
settings_autostart(void)
{
    g_autofree char *path = g_build_filename(g_get_user_config_dir(), "autostart", MYTOOTH_ID ".desktop", NULL);
    g_autoptr(GKeyFile) file = g_key_file_new();
    return g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL) &&
        !g_key_file_get_boolean(file, "Desktop Entry", "Hidden", NULL);
}

gboolean
settings_set_autostart(gboolean enabled, GError **error)
{
    g_autofree char *directory = g_build_filename(g_get_user_config_dir(), "autostart", NULL);
    g_autofree char *path = g_build_filename(directory, MYTOOTH_ID ".desktop", NULL);
    g_autofree char *text = g_strdup_printf(
        "[Desktop Entry]\nType=Application\nName=Mytooth\nExec=mytooth --background\n"
        "TryExec=mytooth\nIcon=" MYTOOTH_ID "\nTerminal=false\nHidden=%s\n", enabled ? "false" : "true");
    return save_file(directory, path, text, error);
}
