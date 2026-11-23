#pragma once

#include <glib.h>

typedef struct _VertereReply VertereReply;

/**
 * Builds the system prompt for translating a screenshot (includes transcription).
 */
[[nodiscard]] [[gnu::nonnull(1, 2)]]
char *vertere_image_system_prompt(const char *target_lang, const char *fallback_lang);

/**
 * Builds the system prompt for translating plain text.
 */
[[nodiscard]] [[gnu::nonnull(1, 2)]]
char *vertere_text_system_prompt(const char *target_lang, const char *fallback_lang);

/**
 * Allocates a reply accumulator for an image translation (expects separator + source).
 */
[[nodiscard]]
VertereReply *vertere_reply_new(void);

/**
 * Allocates a reply accumulator for a text translation with known @source.
 */
[[nodiscard]] [[gnu::nonnull(1)]]
VertereReply *vertere_reply_new_with_source(const char *source);

void vertere_reply_free(VertereReply *reply);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VertereReply, vertere_reply_free)

/**
 * Appends one streamed delta fragment to @reply.
 */
[[gnu::nonnull(1)]]
void vertere_reply_push(VertereReply *reply, const char *delta);

/**
 * Returns the trimmed translation received so far (owned by @reply).
 */
[[nodiscard]] [[gnu::nonnull(1)]]
const char *vertere_reply_translation(const VertereReply *reply);

/**
 * Returns the trimmed source text (owned by @reply).
 */
[[nodiscard]] [[gnu::nonnull(1)]]
const char *vertere_reply_source(const VertereReply *reply);

/**
 * Returns whether source text is ready to display.
 */
[[nodiscard]] [[gnu::nonnull(1)]]
bool vertere_reply_has_source(const VertereReply *reply);

/**
 * Returns whether both translation and source are empty after trimming.
 */
[[nodiscard]] [[gnu::nonnull(1)]]
bool vertere_reply_is_empty(const VertereReply *reply);
