/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"
#include "obex.h"
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static void
test_pin(void)
{
    g_assert_true(bt_valid_pin("0000"));
    g_assert_true(bt_valid_pin("aBc123"));
    g_assert_true(bt_valid_pin("1234567890123456"));
    g_assert_false(bt_valid_pin("12345678901234567"));
    g_assert_false(bt_valid_pin(""));
    g_assert_false(bt_valid_pin(NULL));
    g_assert_false(bt_valid_pin("a\nb"));
    g_assert_false(bt_valid_pin("\xff"));
}

static void
test_passkey(void)
{
    guint32 key = 42;
    g_assert_true(bt_parse_passkey("000000", &key));
    g_assert_cmpuint(key, ==, 0);
    g_assert_true(bt_parse_passkey("999999", &key));
    g_assert_cmpuint(key, ==, 999999);
    const char *bad[] = { "", "1000000", "-1", "+1", " 123", "1e3", "12a", "１２３" };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++)
        g_assert_false(bt_parse_passkey(bad[i], &key));
    g_assert_false(bt_parse_passkey(NULL, &key));
}

static void
test_errors(void)
{
    g_autoptr(GError) error = g_dbus_error_new_for_dbus_error("org.bluez.Error.AuthenticationRejected", "private remote details");
    g_autofree char *message = bt_error_message(error);
    g_assert_nonnull(strstr(message, "rejected"));
    g_assert_null(strstr(message, "private"));
    g_clear_error(&error);
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "AA:BB:CC:DD:EE:FF");
    g_free(message);
    message = bt_error_message(error);
    g_assert_null(strstr(message, "AA:BB"));
}

static void
test_receive_path(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *root = g_dir_make_tmp("mytooth-path-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *directory = g_build_filename(root, "Bluetooth", NULL);
    g_autofree char *first = obex_safe_receive_path(directory, "../../photo.jpg", &error);
    g_assert_no_error(error);
    g_autofree char *first_directory = g_path_get_dirname(first);
    g_autofree char *first_name = g_path_get_basename(first);
    g_assert_cmpstr(first_directory, ==, directory);
    g_assert_cmpstr(first_name, ==, "photo.jpg");
    g_assert_true(g_file_set_contents(first, "x", 1, &error));
    g_assert_no_error(error);
    g_autofree char *second = obex_safe_receive_path(directory, "photo.jpg", &error);
    g_assert_no_error(error);
    g_autofree char *second_name = g_path_get_basename(second);
    g_assert_cmpstr(second_name, ==, "photo (1).jpg");
    g_autofree char *odd = obex_safe_receive_path(directory, "..\\secret\n.txt", &error);
    g_assert_no_error(error);
    g_autofree char *odd_name = g_path_get_basename(odd);
    g_assert_null(strchr(odd_name, '/'));
    g_assert_null(strchr(odd_name, '\\'));
    GStatBuf st;
    g_assert_cmpint(g_stat(directory, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
    g_assert_cmpint(g_remove(first), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_assert_cmpint(g_rmdir(root), ==, 0);
}

static void
test_receive_symlink(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *root = g_dir_make_tmp("mytooth-link-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *target = g_build_filename(root, "target", NULL);
    g_autofree char *link = g_build_filename(root, "Bluetooth", NULL);
    g_assert_cmpint(g_mkdir(target, 0700), ==, 0);
    g_assert_cmpint(symlink(target, link), ==, 0);
    g_autofree char *path = obex_safe_receive_path(link, "file.txt", &error);
    g_assert_null(path);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY);
    g_assert_cmpint(g_unlink(link), ==, 0);
    g_assert_cmpint(g_rmdir(target), ==, 0);
    g_assert_cmpint(g_rmdir(root), ==, 0);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/input/pin", test_pin);
    g_test_add_func("/input/passkey", test_passkey);
    g_test_add_func("/errors/redaction", test_errors);
    g_test_add_func("/obex/safe-receive-path", test_receive_path);
    g_test_add_func("/obex/reject-symlink-directory", test_receive_symlink);
    return g_test_run();
}
