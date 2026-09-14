/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "bluetooth.h"

#define BT_TYPE_AGENT (bt_agent_get_type())
G_DECLARE_FINAL_TYPE(BtAgent, bt_agent, BT, AGENT, GObject)

typedef enum { BT_PROMPT_NONE, BT_PROMPT_PIN, BT_PROMPT_PASSKEY,
               BT_PROMPT_CONFIRM, BT_PROMPT_AUTHORIZE, BT_PROMPT_DISPLAY } BtPrompt;
/* Signals: prompt-changed(); changed(). No GTK dependency. */
BtAgent *bt_agent_new(BtClient *client);
void bt_agent_stop(BtAgent *self);
void bt_agent_retry(BtAgent *self);
gboolean bt_agent_ready(BtAgent *self);
gboolean bt_agent_is_default(BtAgent *self);
void bt_agent_set_locked(BtAgent *self, gboolean locked);
gboolean bt_agent_locked(BtAgent *self);
BtPrompt bt_agent_prompt(BtAgent *self);
const char *bt_agent_device(BtAgent *self);
const char *bt_agent_text(BtAgent *self);
/* Invalid input leaves the prompt open. Returned FALSE means validation failed. */
gboolean bt_agent_answer(BtAgent *self, gboolean accept, const char *text);
