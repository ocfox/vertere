#include "translate.h"
#include "vertere.h"

#include <string.h>

struct _VertereReply {
    GString *translation_raw;
    GString *source_raw;
    char *translation_trimmed;
    char *source_trimmed;
    bool past_separator;
    bool expects_source;
};

[[nodiscard]]
static char *build_directive(const char *target_lang, const char *fallback_lang) {
    g_autofree char *fallback = vertere_trim_dup(fallback_lang);
    g_autofree char *exception = (*fallback == '\0')
        ? g_strdup("repeat it unchanged")
        : g_strdup_printf("translate it into %s instead", fallback);

    return g_strdup_printf(
        "You translate text into %s, unless the source is already "
        "written in %s — judged by its grammar, not merely by sharing "
        "characters with it — in which case you %s.",
        target_lang, target_lang, exception);
}

char *vertere_image_system_prompt(const char *target_lang, const char *fallback_lang) {
    g_autofree char *directive = build_directive(target_lang, fallback_lang);
    return g_strdup_printf(
        "%s The text reaches you as an image of a screen region.\n\n"
        "Reply with exactly two sections, separated by a line containing only %s:\n\n"
        "<the translation>\n"
        "%s\n"
        "<the source text, transcribed exactly as it appears>\n\n"
        "Rules:\n"
        "- Output nothing else. No preamble, no commentary, no markdown code fences.\n"
        "- Preserve line breaks and paragraph structure in both sections.\n"
        "- Transcribe the source in its original language, keeping its spelling and "
        "punctuation. Do not correct it.\n"
        "- Give only the translation; do not mention which language it ended up in or why.\n"
        "- If there is no readable text at all, write a single hyphen as the translation "
        "and leave the source section empty.",
        directive, VERTERE_SEPARATOR, VERTERE_SEPARATOR);
}

char *vertere_text_system_prompt(const char *target_lang, const char *fallback_lang) {
    g_autofree char *directive = build_directive(target_lang, fallback_lang);
    return g_strdup_printf(
        "%s\n\n"
        "Rules:\n"
        "- Output nothing else. No preamble, no commentary, no markdown code fences.\n"
        "- Preserve line breaks and paragraph structure.\n"
        "- Give only the translation; do not mention which language it ended up in or why.\n"
        "- If there is no translatable text at all, reply with a single hyphen.",
        directive);
}

static void refresh_trimmed(VertereReply *reply) {
    g_free(reply->translation_trimmed);
    reply->translation_trimmed = vertere_trim_dup(reply->translation_raw->str);
    g_free(reply->source_trimmed);
    reply->source_trimmed = vertere_trim_dup(reply->source_raw->str);
}

VertereReply *vertere_reply_new(void) {
    VertereReply *reply = g_new0(VertereReply, 1);
    reply->translation_raw = g_string_new("");
    reply->source_raw = g_string_new("");
    reply->expects_source = true;
    refresh_trimmed(reply);
    return reply;
}

VertereReply *vertere_reply_new_with_source(const char *source) {
    VertereReply *reply = g_new0(VertereReply, 1);
    reply->translation_raw = g_string_new("");
    reply->source_raw = g_string_new(source);
    reply->expects_source = false;
    refresh_trimmed(reply);
    return reply;
}

void vertere_reply_free(VertereReply *reply) {
    if (reply == nullptr) {
        return;
    }
    g_string_free(reply->translation_raw, TRUE);
    g_string_free(reply->source_raw, TRUE);
    g_free(reply->translation_trimmed);
    g_free(reply->source_trimmed);
    g_free(reply);
}

void vertere_reply_push(VertereReply *reply, const char *delta) {
    if (delta == nullptr || *delta == '\0') {
        return;
    }
    if (!reply->expects_source) {
        g_string_append(reply->translation_raw, delta);
        refresh_trimmed(reply);
        return;
    }

    const char *rest = delta;
    if (!reply->past_separator) {
        const char *sep = strstr(rest, VERTERE_SEPARATOR);
        if (sep != nullptr) {
            g_string_append_len(reply->translation_raw, rest, (gssize)(sep - rest));
            reply->past_separator = true;
            rest = sep + strlen(VERTERE_SEPARATOR);
        } else {
            g_string_append(reply->translation_raw, rest);
            refresh_trimmed(reply);
            return;
        }
    }
    g_string_append(reply->source_raw, rest);
    refresh_trimmed(reply);
}

const char *vertere_reply_translation(const VertereReply *reply) {
    return reply->translation_trimmed;
}

const char *vertere_reply_source(const VertereReply *reply) {
    return reply->source_trimmed;
}

bool vertere_reply_has_source(const VertereReply *reply) {
    return !reply->expects_source || reply->past_separator;
}

bool vertere_reply_is_empty(const VertereReply *reply) {
    return *reply->translation_trimmed == '\0' && *reply->source_trimmed == '\0';
}
