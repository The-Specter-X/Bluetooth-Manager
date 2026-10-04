/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "settings.h"
#include "util.h"
#include <glib/gstdio.h>

static char *root;

static void
test_preferences(void)
{
    Settings settings;
    settings_load(&settings);
    g_assert_cmpint(settings.width, ==, 840);
    g_assert_cmpint(settings.height, ==, 620);
    g_assert_true(settings.notifications);
    settings = (Settings) { .width = 1000, .height = 750, .notifications = FALSE };
    g_autoptr(GError) error = NULL;
    g_assert_true(settings_save(&settings, &error));
    g_assert_no_error(error);
    Settings loaded;
    settings_load(&loaded);
    g_assert_cmpint(loaded.width, ==, 1000);
    g_assert_cmpint(loaded.height, ==, 750);
    g_assert_false(loaded.notifications);
    g_autofree char *directory = g_build_filename(root, "mytooth", NULL);
    g_autofree char *path = g_build_filename(directory, "settings.ini", NULL);
    GStatBuf st;
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 0777, ==, 0600);
    g_assert_true(g_file_set_contents(path,
        "[Window]\nwidth=invalid\nheight=9999\n[General]\nnotifications=invalid\n", -1, &error));
    g_assert_no_error(error);
    settings_load(&loaded);
    g_assert_cmpint(loaded.width, ==, 840);
    g_assert_cmpint(loaded.height, ==, 1600);
    g_assert_true(loaded.notifications);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void
test_autostart(void)
{
    g_assert_false(settings_autostart());
    g_assert_true(settings_set_autostart(TRUE, NULL));
    g_assert_true(settings_autostart());
    g_assert_true(settings_set_autostart(FALSE, NULL));
    g_assert_false(settings_autostart());
    g_autofree char *directory = g_build_filename(root, "autostart", NULL);
    g_autofree char *path = g_build_filename(directory, MYTOOTH_ID ".desktop", NULL);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    root = g_dir_make_tmp("mytooth-settings-XXXXXX", NULL);
    g_assert_nonnull(root);
    g_setenv("XDG_CONFIG_HOME", root, TRUE);
    g_test_add_func("/settings/preferences-and-malformed-values", test_preferences);
    g_test_add_func("/settings/autostart-toggle", test_autostart);
    int result = g_test_run();
    g_assert_cmpint(g_rmdir(root), ==, 0);
    g_free(root);
    return result;
}
