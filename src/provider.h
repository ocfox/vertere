#pragma once

#include "store.h"

#include <gio/gio.h>
#include <glib.h>

typedef struct _VertereProvider VertereProvider;

typedef void (*VertereDeltaCallback)(const char *delta, gpointer user_data);
typedef void (*VertereDoneCallback)(const GError *error, gpointer user_data);
typedef void (*VertereCheckCallback)(const GError *error, gpointer user_data);

[[nodiscard]] [[gnu::nonnull(1, 2)]]
VertereProvider *vertere_provider_new(
    const VertereSettings *settings,
    const char *api_key,
    GError **error
);

VertereProvider *vertere_provider_ref(VertereProvider *provider);
void vertere_provider_unref(VertereProvider *provider);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VertereProvider, vertere_provider_unref)

/**
 * Verifies that the configured model exists on the endpoint and accepts images.
 */
[[gnu::nonnull(1, 3)]]
void vertere_provider_check_model(
    VertereProvider *provider,
    GCancellable *cancellable,
    VertereCheckCallback on_done,
    gpointer user_data,
    GDestroyNotify destroy_data
);

/**
 * Streams translation (and transcribed source) of a PNG screenshot.
 */
[[gnu::nonnull(1, 2, 4, 5)]]
void vertere_provider_translate_image(
    VertereProvider *provider,
    GBytes *png,
    GCancellable *cancellable,
    VertereDeltaCallback on_delta,
    VertereDoneCallback on_done,
    gpointer user_data,
    GDestroyNotify destroy_data
);

/**
 * Streams translation of plain text.
 */
[[gnu::nonnull(1, 2, 4, 5)]]
void vertere_provider_translate_text(
    VertereProvider *provider,
    const char *text,
    GCancellable *cancellable,
    VertereDeltaCallback on_delta,
    VertereDoneCallback on_done,
    gpointer user_data,
    GDestroyNotify destroy_data
);

/**
 * Pure helper: validates that @extra_json is either empty/whitespace or a JSON object.
 */
[[nodiscard]]
bool vertere_provider_validate_extra_json(const char *extra_json, GError **error);

/**
 * Pure helper (exposed for unit tests): builds a streaming chat-completions JSON body.
 */
[[nodiscard]] [[gnu::nonnull(1, 2, 3)]]
char *vertere_provider_build_text_payload(
    const char *model,
    const char *system_prompt,
    const char *user_text,
    const char *extra_json,
    size_t *out_len
);

/**
 * Pure helper (exposed for unit tests): validates a /models JSON payload
 * against @model_id and checks image input modality metadata.
 */
[[nodiscard]] [[gnu::nonnull(1, 3)]]
bool vertere_provider_validate_models_json(
    const char *json,
    size_t len,
    const char *model_id,
    GError **error
);

/**
 * Pure helper (exposed for unit tests): parses one SSE line.
 * Sets *out_done = true on `data: [DONE]`. Sets *out_delta if a non-empty
 * content delta is present. Returns false only on malformed/error payloads.
 */
[[nodiscard]] [[gnu::nonnull(2, 3)]]
bool vertere_provider_parse_sse_line(
    const char *line,
    char **out_delta,
    bool *out_done,
    GError **error
);
