/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

typedef struct { gboolean notifications; int width; int height; } Settings;
void settings_load(Settings *settings);
gboolean settings_save(const Settings *settings, GError **error);
gboolean settings_autostart(void);
gboolean settings_set_autostart(gboolean enabled, GError **error);
