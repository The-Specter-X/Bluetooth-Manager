/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio.h"
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>
#include <string.h>

typedef struct {
    uint32_t index;
    char *identity;
    GPtrArray *profiles;
} AudioCard;

struct _AudioClient {
    GObject parent_instance;
    pa_glib_mainloop *loop;
    pa_context *context;
    GPtrArray *cards;
    GPtrArray *loading_cards;
    guint retry;
    gboolean ready;
    gboolean loading;
    gboolean refresh_again;
    gboolean setting;
    gboolean stopped;
};

G_DEFINE_TYPE(AudioClient, audio_client, G_TYPE_OBJECT)
enum { CHANGED, ERROR, N_SIGNALS };
static guint signals[N_SIGNALS];

static void audio_connect(AudioClient *self);
static void refresh(AudioClient *self);

void
audio_profile_free(AudioProfile *profile)
{
    if (!profile)
        return;
    g_free(profile->name);
    g_free(profile->description);
    g_free(profile);
}

static void
audio_card_free(gpointer data)
{
    AudioCard *card = data;
    g_free(card->identity);
    g_ptr_array_unref(card->profiles);
    g_free(card);
}

static char *
normalized(const char *text)
{
    GString *result = g_string_new(NULL);
    for (const char *p = text ? text : ""; *p; p++)
        if (g_ascii_isxdigit(*p))
            g_string_append_c(result, g_ascii_toupper(*p));
    return g_string_free(result, FALSE);
}

static gboolean
candidate_matches(const char *candidate, const char *address)
{
    g_autofree char *value = normalized(candidate);
    return address[0] && strstr(value, address) != NULL;
}

static gboolean
bluetooth_card(const pa_card_info *info)
{
    const char *bus = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_BUS);
    const char *api = pa_proplist_gets(info->proplist, "device.api");
    return g_strcmp0(bus, "bluetooth") == 0 || (api && g_str_has_prefix(api, "bluez")) ||
        (info->name && g_str_has_prefix(info->name, "bluez_card."));
}

static char *
card_identity(const pa_card_info *info)
{
    const char *keys[] = { "api.bluez5.address", "device.string", "device.name",
                           "bluez.path", NULL };
    GString *identity = g_string_new(info->name ? info->name : "");
    for (guint i = 0; keys[i]; i++) {
        const char *value = pa_proplist_gets(info->proplist, keys[i]);
        if (value)
            g_string_append_printf(identity, " %s", value);
    }
    return g_string_free(identity, FALSE);
}

static void
emit_error(AudioClient *self, const char *message)
{
    if (!self->stopped)
        g_signal_emit(self, signals[ERROR], 0, message);
}

static void
card_info(pa_context *context, const pa_card_info *info, int eol, void *data)
{
    AudioClient *self = data;
    if (self->stopped || !self->loading)
        return;
    if (eol < 0) {
        self->loading = FALSE;
        g_clear_pointer(&self->loading_cards, g_ptr_array_unref);
        return;
    }
    if (eol > 0) {
        gboolean again = self->refresh_again;
        self->refresh_again = FALSE;
        self->loading = FALSE;
        g_clear_pointer(&self->cards, g_ptr_array_unref);
        self->cards = g_steal_pointer(&self->loading_cards);
        g_signal_emit(self, signals[CHANGED], 0);
        if (again)
            refresh(self);
        return;
    }
    if (!info || !bluetooth_card(info))
        return;
    AudioCard *card = g_new0(AudioCard, 1);
    card->index = info->index;
    card->identity = card_identity(info);
    card->profiles = g_ptr_array_new_with_free_func((GDestroyNotify)audio_profile_free);
    for (uint32_t i = 0; i < info->n_profiles; i++) {
        const pa_card_profile_info2 *profile = info->profiles2 ? info->profiles2[i] : NULL;
        if (!profile || !profile->name || profile->available == 0)
            continue;
        AudioProfile *copy = g_new0(AudioProfile, 1);
        copy->name = g_strdup(profile->name);
        copy->description = g_strdup(profile->description ? profile->description : profile->name);
        copy->active = info->active_profile2 &&
            g_strcmp0(info->active_profile2->name, profile->name) == 0;
        g_ptr_array_add(card->profiles, copy);
    }
    if (card->profiles->len)
        g_ptr_array_add(self->loading_cards, card);
    else
        audio_card_free(card);
}

static void
refresh(AudioClient *self)
{
    if (self->stopped || !self->ready)
        return;
    if (self->loading) {
        self->refresh_again = TRUE;
        return;
    }
    self->loading = TRUE;
    self->loading_cards = g_ptr_array_new_with_free_func(audio_card_free);
    pa_operation *operation = pa_context_get_card_info_list(self->context, card_info, self);
    if (operation)
        pa_operation_unref(operation);
    else {
        self->loading = FALSE;
        g_clear_pointer(&self->loading_cards, g_ptr_array_unref);
    }
}

static void
subscribed(pa_context *context, pa_subscription_event_type_t type,
           uint32_t index, void *data)
{
    if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_CARD)
        refresh(data);
}

static gboolean
retry_connect(gpointer data)
{
    AudioClient *self = data;
    self->retry = 0;
    audio_connect(self);
    return G_SOURCE_REMOVE;
}

static void
context_state(pa_context *context, void *data)
{
    AudioClient *self = data;
    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY: {
        self->ready = TRUE;
        pa_context_set_subscribe_callback(context, subscribed, self);
        pa_operation *operation = pa_context_subscribe(context, PA_SUBSCRIPTION_MASK_CARD,
                                                        NULL, NULL);
        if (operation)
            pa_operation_unref(operation);
        refresh(self);
        g_signal_emit(self, signals[CHANGED], 0);
        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        self->ready = FALSE;
        self->loading = FALSE;
        self->refresh_again = FALSE;
        self->setting = FALSE;
        g_clear_pointer(&self->loading_cards, g_ptr_array_unref);
        g_clear_pointer(&self->cards, g_ptr_array_unref);
        g_signal_emit(self, signals[CHANGED], 0);
        if (!self->stopped && !self->retry)
            self->retry = g_timeout_add_seconds(5, retry_connect, self);
        break;
    default:
        break;
    }
}

static void
audio_connect(AudioClient *self)
{
    if (self->stopped)
        return;
    if (self->context) {
        pa_context_set_state_callback(self->context, NULL, NULL);
        pa_context_disconnect(self->context);
        pa_context_unref(self->context);
    }
    self->context = pa_context_new(pa_glib_mainloop_get_api(self->loop), "Mytooth");
    if (!self->context)
        return;
    pa_context_set_state_callback(self->context, context_state, self);
    if (pa_context_connect(self->context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0 && !self->retry)
        self->retry = g_timeout_add_seconds(5, retry_connect, self);
}

AudioClient *
audio_client_new(void)
{
    AudioClient *self = g_object_new(AUDIO_TYPE_CLIENT, NULL);
    self->loop = pa_glib_mainloop_new(g_main_context_default());
    if (self->loop)
        audio_connect(self);
    return self;
}

static AudioCard *
find_card(AudioClient *self, const char *bluetooth_address)
{
    g_autofree char *address = normalized(bluetooth_address);
    for (guint i = 0; self->cards && i < self->cards->len; i++) {
        AudioCard *card = g_ptr_array_index(self->cards, i);
        if (candidate_matches(card->identity, address))
            return card;
    }
    return NULL;
}

GPtrArray *
audio_client_profiles(AudioClient *self, const char *bluetooth_address)
{
    GPtrArray *profiles = g_ptr_array_new_with_free_func((GDestroyNotify)audio_profile_free);
    AudioCard *card = find_card(self, bluetooth_address);
    if (!card)
        return profiles;
    for (guint i = 0; i < card->profiles->len; i++) {
        AudioProfile *source = g_ptr_array_index(card->profiles, i);
        AudioProfile *copy = g_new0(AudioProfile, 1);
        copy->name = g_strdup(source->name);
        copy->description = g_strdup(source->description);
        copy->active = source->active;
        g_ptr_array_add(profiles, copy);
    }
    return profiles;
}

static void
profile_set(pa_context *context, int success, void *data)
{
    AudioClient *self = data;
    self->setting = FALSE;
    if (success)
        refresh(self);
    else
        emit_error(self, "The audio service could not change this Bluetooth profile.");
    g_signal_emit(self, signals[CHANGED], 0);
}

gboolean
audio_client_set_profile(AudioClient *self, const char *bluetooth_address,
                         const char *profile_name)
{
    if (!self->ready || self->setting || !profile_name)
        return FALSE;
    AudioCard *card = find_card(self, bluetooth_address);
    if (!card)
        return FALSE;
    gboolean found = FALSE;
    for (guint i = 0; i < card->profiles->len; i++) {
        AudioProfile *profile = g_ptr_array_index(card->profiles, i);
        found |= g_strcmp0(profile->name, profile_name) == 0;
    }
    if (!found)
        return FALSE;
    pa_operation *operation = pa_context_set_card_profile_by_index(self->context,
        card->index, profile_name, profile_set, self);
    if (!operation)
        return FALSE;
    self->setting = TRUE;
    pa_operation_unref(operation);
    g_signal_emit(self, signals[CHANGED], 0);
    return TRUE;
}

gboolean audio_client_ready(AudioClient *self) { return self->ready; }
gboolean audio_client_busy(AudioClient *self) { return self->setting; }

void
audio_client_stop(AudioClient *self)
{
    if (self->stopped)
        return;
    self->stopped = TRUE;
    if (self->retry) { g_source_remove(self->retry); self->retry = 0; }
    if (self->context) {
        pa_context_set_state_callback(self->context, NULL, NULL);
        pa_context_set_subscribe_callback(self->context, NULL, NULL);
        pa_context_disconnect(self->context);
        pa_context_unref(self->context);
        self->context = NULL;
    }
    self->ready = FALSE;
}

static void
audio_client_finalize(GObject *object)
{
    AudioClient *self = AUDIO_CLIENT(object);
    audio_client_stop(self);
    g_clear_pointer(&self->cards, g_ptr_array_unref);
    g_clear_pointer(&self->loading_cards, g_ptr_array_unref);
    if (self->loop)
        pa_glib_mainloop_free(self->loop);
    G_OBJECT_CLASS(audio_client_parent_class)->finalize(object);
}

static void
audio_client_class_init(AudioClientClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = audio_client_finalize;
    signals[CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[ERROR] = g_signal_new("error", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void audio_client_init(AudioClient *self) { (void)self; }
