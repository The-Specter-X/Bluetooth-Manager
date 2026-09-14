/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"

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

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/input/pin", test_pin);
    g_test_add_func("/input/passkey", test_passkey);
    g_test_add_func("/errors/redaction", test_errors);
    return g_test_run();
}
