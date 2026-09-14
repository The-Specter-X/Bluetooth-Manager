/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

typedef struct Rfkill Rfkill;
typedef enum { RADIO_UNKNOWN, RADIO_CLEAR, RADIO_SOFT_BLOCKED, RADIO_HARD_BLOCKED } RadioState;
Rfkill *rfkill_new(GSourceFunc changed, gpointer data);
void rfkill_free(Rfkill *radio);
RadioState rfkill_state(Rfkill *radio, const char *adapter_path);
gboolean rfkill_unblock(Rfkill *radio, const char *adapter_path, GError **error);
