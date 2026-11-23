#pragma once

#include <glib.h>

/**
 * D-Bus application ID, desktop entry basename, and icon name.
 */
static constexpr char VERTERE_APP_ID[] = "me.ocfox.Vertere";

/**
 * Layer-shell namespace matched by compositor window rules.
 */
static constexpr char VERTERE_LAYER_NAMESPACE[] = "vertere";

/**
 * Environment variable holding the API key for the configured endpoint.
 */
static constexpr char VERTERE_API_KEY_ENV[] = "API_KEY";

/**
 * Single-character separator emitted between translation and source transcription
 * for image translation requests.
 */
static constexpr char VERTERE_SEPARATOR[] = "\xe2\x81\x82"; /* U+2042 ASTERISM ⁂ */

static constexpr char VERTERE_DEFAULT_TARGET_LANG[] = "Simplified Chinese";
static constexpr char VERTERE_DEFAULT_FALLBACK_LANG[] = "English";
static constexpr char VERTERE_DEFAULT_BASE_URL[] = "https://tokenhub.tencentmaas.com/v1";

/**
 * Returns a newly allocated UTF-8 whitespace-trimmed copy of @str,
 * or empty string if @str is nullptr or all whitespace.
 */
[[nodiscard]]
static inline char *vertere_trim_dup(const char *str) {
    if (str == nullptr) {
        return g_strdup("");
    }
    const char *start = str;
    while (*start != '\0') {
        gunichar ch = g_utf8_get_char(start);
        if (!g_unichar_isspace(ch)) {
            break;
        }
        start = g_utf8_next_char(start);
    }
    if (*start == '\0') {
        return g_strdup("");
    }
    const char *end = start + strlen(start);
    while (end > start) {
        const char *prev = g_utf8_prev_char(end);
        gunichar ch = g_utf8_get_char(prev);
        if (!g_unichar_isspace(ch)) {
            break;
        }
        end = prev;
    }
    return g_strndup(start, (gsize)(end - start));
}
