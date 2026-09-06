/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "agent.h"
typedef struct Session Session;
Session *session_new(GDBusConnection *bus, BtClient *client, BtAgent *agent);
void session_free(Session *session);
