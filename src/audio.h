/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib-object.h>

#define AUDIO_TYPE_CLIENT (audio_client_get_type())
G_DECLARE_FINAL_TYPE(AudioClient, audio_client, AUDIO, CLIENT, GObject)

typedef struct {
    char *name;
    char *description;
    gboolean active;
} AudioProfile;

/* Uses the PulseAudio client API, which is also provided by PipeWire-Pulse.
 * Signals: changed(); error(message). Returned arrays and profiles are owned. */
AudioClient *audio_client_new(void);
void audio_client_stop(AudioClient *self);
gboolean audio_client_ready(AudioClient *self);
gboolean audio_client_busy(AudioClient *self);
GPtrArray *audio_client_profiles(AudioClient *self, const char *bluetooth_address);
gboolean audio_client_set_profile(AudioClient *self, const char *bluetooth_address,
                                  const char *profile_name);
void audio_profile_free(AudioProfile *profile);
