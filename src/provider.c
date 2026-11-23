#include "provider.h"
#include "translate.h"
#include "vertere.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

static constexpr unsigned int CONNECT_TIMEOUT_SECS = 10;
static constexpr unsigned int READ_TIMEOUT_SECS = 20;

struct _VertereProvider {
    gatomicrefcount ref_count;
    SoupSession *session;
    char *api_key;
    char *base_url;
    char *model;
    char *image_system_prompt;
    char *text_system_prompt;
    char *extra_json;
};

#define VERTERE_PROVIDER_ERROR (vertere_provider_error_quark())
static GQuark vertere_provider_error_quark(void) {
    return g_quark_from_static_string("vertere-provider-error");
}

static char *strip_trailing_slashes(const char *url) {
    size_t len = strlen(url);
    while (len > 0 && url[len - 1] == '/') {
        len--;
    }
    return g_strndup(url, len);
}

VertereProvider *vertere_provider_new(
    const VertereSettings *settings,
    const char *api_key,
    GError **error
) {
    g_autofree char *trimmed_key = vertere_trim_dup(api_key);
    if (*trimmed_key == '\0') {
        g_set_error(error, VERTERE_PROVIDER_ERROR, 1, "API key is empty");
        return nullptr;
    }

    g_autofree char *trimmed_extra = vertere_trim_dup(settings->extra_json);
    if (!vertere_provider_validate_extra_json(trimmed_extra, error)) {
        g_free(trimmed_key);
        return nullptr;
    }

    g_autofree char *raw_url = vertere_settings_dup_base_url(settings);
    g_autofree char *target = vertere_settings_dup_target(settings);
    g_autofree char *fallback = vertere_settings_dup_fallback(settings);

    VertereProvider *p = g_new0(VertereProvider, 1);
    g_atomic_ref_count_init(&p->ref_count);
    p->session = soup_session_new();
    soup_session_set_timeout(p->session, READ_TIMEOUT_SECS + 5);
    p->api_key = g_steal_pointer(&trimmed_key);
    p->base_url = strip_trailing_slashes(raw_url);
    p->model = g_strdup(settings->model);
    p->image_system_prompt = vertere_image_system_prompt(target, fallback);
    p->text_system_prompt = vertere_text_system_prompt(target, fallback);
    p->extra_json = g_steal_pointer(&trimmed_extra);
    return p;
}

VertereProvider *vertere_provider_ref(VertereProvider *provider) {
    g_atomic_ref_count_inc(&provider->ref_count);
    return provider;
}

void vertere_provider_unref(VertereProvider *provider) {
    if (provider == nullptr) {
        return;
    }
    if (!g_atomic_ref_count_dec(&provider->ref_count)) {
        return;
    }
    g_clear_object(&provider->session);
    g_free(provider->api_key);
    g_free(provider->base_url);
    g_free(provider->model);
    g_free(provider->image_system_prompt);
    g_free(provider->text_system_prompt);
    g_free(provider->extra_json);
    g_free(provider);
}

static bool json_array_has_ci(JsonArray *arr, const char *needle) {
    guint n = json_array_get_length(arr);
    for (guint i = 0; i < n; i++) {
        JsonNode *elem = json_array_get_element(arr, i);
        if (elem != nullptr && JSON_NODE_HOLDS_VALUE(elem)) {
            const char *s = json_node_get_string(elem);
            if (s != nullptr && g_ascii_strcasecmp(s, needle) == 0) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Returns -1 if metadata explicitly says no image input, 1 if accepted, 0 if unknown.
 */
static int check_model_accepts_images(JsonObject *model_obj) {
    if (!json_object_has_member(model_obj, "architecture")) {
        return 0;
    }
    JsonNode *arch_node = json_object_get_member(model_obj, "architecture");
    if (arch_node == nullptr || !JSON_NODE_HOLDS_OBJECT(arch_node)) {
        return 0;
    }
    JsonObject *arch = json_node_get_object(arch_node);

    if (json_object_has_member(arch, "input_modalities")) {
        JsonNode *mods_node = json_object_get_member(arch, "input_modalities");
        if (mods_node != nullptr && JSON_NODE_HOLDS_ARRAY(mods_node)) {
            return json_array_has_ci(json_node_get_array(mods_node), "image") ? 1 : -1;
        }
    }

    if (json_object_has_member(arch, "modality")) {
        JsonNode *mod_node = json_object_get_member(arch, "modality");
        if (mod_node != nullptr && JSON_NODE_HOLDS_VALUE(mod_node)) {
            const char *mod = json_node_get_string(mod_node);
            if (mod != nullptr) {
                const char *arrow = strstr(mod, "->");
                g_autofree char *inputs = arrow != nullptr
                    ? g_strndup(mod, (gsize)(arrow - mod))
                    : g_strdup(mod);
                return strstr(inputs, "image") != nullptr ? 1 : -1;
            }
        }
    }

    return 0;
}

bool vertere_provider_validate_models_json(
    const char *json,
    size_t len,
    const char *model_id,
    GError **error
) {
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, (gssize)len, error)) {
        return false;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root == nullptr || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error(error, VERTERE_PROVIDER_ERROR, 2, "invalid models response");
        return false;
    }
    JsonObject *root_obj = json_node_get_object(root);
    if (!json_object_has_member(root_obj, "data")) {
        g_set_error(error, VERTERE_PROVIDER_ERROR, 2, "models response missing `data`");
        return false;
    }
    JsonNode *data_node = json_object_get_member(root_obj, "data");
    if (data_node == nullptr || !JSON_NODE_HOLDS_ARRAY(data_node)) {
        g_set_error(error, VERTERE_PROVIDER_ERROR, 2, "models `data` is not an array");
        return false;
    }

    JsonArray *data = json_node_get_array(data_node);
    guint count = json_array_get_length(data);
    for (guint i = 0; i < count; i++) {
        JsonNode *item = json_array_get_element(data, i);
        if (item == nullptr || !JSON_NODE_HOLDS_OBJECT(item)) {
            continue;
        }
        JsonObject *obj = json_node_get_object(item);
        const char *id = json_object_get_string_member_with_default(obj, "id", nullptr);
        if (g_strcmp0(id, model_id) == 0) {
            if (check_model_accepts_images(obj) == -1) {
                g_set_error(error, VERTERE_PROVIDER_ERROR, 3,
                            "model %s does not accept image input", model_id);
                return false;
            }
            return true;
        }
    }

    g_set_error(error, VERTERE_PROVIDER_ERROR, 4, "no such model: %s", model_id);
    return false;
}

typedef struct {
    VertereProvider *provider;
    GCancellable *cancellable;
    GCancellable *timeout_cancel;
    guint timeout_id;
    bool timed_out;
    SoupMessage *msg;
    VertereCheckCallback on_done;
    gpointer user_data;
    GDestroyNotify destroy_data;
} CheckCtx;

static void check_ctx_free(CheckCtx *ctx) {
    if (ctx->timeout_id != 0) {
        g_source_remove(ctx->timeout_id);
    }
    if (ctx->destroy_data != nullptr && ctx->user_data != nullptr) {
        ctx->destroy_data(ctx->user_data);
    }
    g_clear_object(&ctx->msg);
    g_clear_object(&ctx->timeout_cancel);
    g_clear_object(&ctx->cancellable);
    vertere_provider_unref(ctx->provider);
    g_free(ctx);
}

static gboolean on_check_timeout(gpointer user_data) {
    CheckCtx *ctx = user_data;
    ctx->timeout_id = 0;
    ctx->timed_out = true;
    g_cancellable_cancel(ctx->timeout_cancel);
    return G_SOURCE_REMOVE;
}

static void on_check_cancelled(GCancellable *, gpointer user_data) {
    CheckCtx *ctx = user_data;
    g_cancellable_cancel(ctx->timeout_cancel);
}

static void on_models_response(GObject *source, GAsyncResult *res, gpointer user_data) {
    CheckCtx *ctx = user_data;
    if (ctx->timeout_id != 0) {
        g_source_remove(ctx->timeout_id);
        ctx->timeout_id = 0;
    }

    g_autoptr(GError) error = nullptr;
    g_autoptr(GBytes) body = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);
    if (body == nullptr) {
        if (ctx->timed_out) {
            g_clear_error(&error);
            g_set_error(&error, VERTERE_PROVIDER_ERROR, 5, "listing models timed out");
        }
        ctx->on_done(error, ctx->user_data);
        check_ctx_free(ctx);
        return;
    }

    guint status = soup_message_get_status(ctx->msg);
    gsize len = 0;
    const char *data = g_bytes_get_data(body, &len);
    if (status < 200 || status >= 300) {
        g_autofree char *snippet = g_strndup(data != nullptr ? data : "", MIN(len, (gsize)256));
        g_set_error(&error, VERTERE_PROVIDER_ERROR, 6,
                    "cannot list models: HTTP %u %s", status, snippet);
        ctx->on_done(error, ctx->user_data);
        check_ctx_free(ctx);
        return;
    }

    if (vertere_provider_validate_models_json(data, len, ctx->provider->model, &error)) {
        ctx->on_done(nullptr, ctx->user_data);
    } else {
        ctx->on_done(error, ctx->user_data);
    }
    check_ctx_free(ctx);
}

void vertere_provider_check_model(
    VertereProvider *provider,
    GCancellable *cancellable,
    VertereCheckCallback on_done,
    gpointer user_data,
    GDestroyNotify destroy_data
) {
    g_autofree char *url = g_strdup_printf("%s/models", provider->base_url);
    SoupMessage *msg = soup_message_new("GET", url);
    if (msg == nullptr) {
        g_autoptr(GError) err = g_error_new(
            VERTERE_PROVIDER_ERROR, 7, "invalid base URL: %s", provider->base_url);
        on_done(err, user_data);
        if (destroy_data != nullptr && user_data != nullptr) {
            destroy_data(user_data);
        }
        return;
    }

    g_autofree char *auth = g_strdup_printf("Bearer %s", provider->api_key);
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "Authorization", auth);

    CheckCtx *ctx = g_new0(CheckCtx, 1);
    ctx->provider = vertere_provider_ref(provider);
    ctx->cancellable = cancellable != nullptr ? g_object_ref(cancellable) : nullptr;
    ctx->timeout_cancel = g_cancellable_new();
    ctx->msg = msg;
    ctx->on_done = on_done;
    ctx->user_data = user_data;
    ctx->destroy_data = destroy_data;

    if (ctx->cancellable != nullptr) {
        g_cancellable_connect(ctx->cancellable, G_CALLBACK(on_check_cancelled), ctx, nullptr);
    }
    ctx->timeout_id = g_timeout_add_seconds(CONNECT_TIMEOUT_SECS, on_check_timeout, ctx);

    soup_session_send_and_read_async(
        provider->session, msg, G_PRIORITY_DEFAULT, ctx->timeout_cancel, on_models_response, ctx);
}

bool vertere_provider_parse_sse_line(
    const char *line,
    char **out_delta,
    bool *out_done,
    GError **error
) {
    *out_delta = nullptr;
    *out_done = false;
    if (line == nullptr) {
        return true;
    }

    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    if (len == 0 || line[0] == ':') {
        return true;
    }
    if (!g_str_has_prefix(line, "data:")) {
        return true;
    }

    const char *payload = line + 5;
    size_t payload_len = len - 5;
    while (payload_len > 0 && *payload == ' ') {
        payload++;
        payload_len--;
    }
    if (payload_len == 0) {
        return true;
    }
    if (payload_len == 6 && strncmp(payload, "[DONE]", 6) == 0) {
        *out_done = true;
        return true;
    }

    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, payload, (gssize)payload_len, error)) {
        return false;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root == nullptr || !JSON_NODE_HOLDS_OBJECT(root)) {
        return true;
    }
    JsonObject *obj = json_node_get_object(root);

    if (json_object_has_member(obj, "error")) {
        JsonNode *err_node = json_object_get_member(obj, "error");
        const char *msg = "stream error";
        if (err_node != nullptr && JSON_NODE_HOLDS_OBJECT(err_node)) {
            JsonObject *err_obj = json_node_get_object(err_node);
            msg = json_object_get_string_member_with_default(err_obj, "message", msg);
        }
        g_set_error(error, VERTERE_PROVIDER_ERROR, 8, "translation stream failed: %s", msg);
        return false;
    }

    if (!json_object_has_member(obj, "choices")) {
        return true;
    }
    JsonNode *choices_node = json_object_get_member(obj, "choices");
    if (choices_node == nullptr || !JSON_NODE_HOLDS_ARRAY(choices_node)) {
        return true;
    }
    JsonArray *choices = json_node_get_array(choices_node);
    if (json_array_get_length(choices) == 0) {
        return true;
    }

    JsonNode *first = json_array_get_element(choices, 0);
    if (first == nullptr || !JSON_NODE_HOLDS_OBJECT(first)) {
        return true;
    }
    JsonObject *choice = json_node_get_object(first);
    if (!json_object_has_member(choice, "delta")) {
        return true;
    }
    JsonNode *delta_node = json_object_get_member(choice, "delta");
    if (delta_node == nullptr || !JSON_NODE_HOLDS_OBJECT(delta_node)) {
        return true;
    }
    JsonObject *delta = json_node_get_object(delta_node);
    if (!json_object_has_member(delta, "content")) {
        return true;
    }
    JsonNode *content_node = json_object_get_member(delta, "content");
    if (content_node != nullptr && JSON_NODE_HOLDS_VALUE(content_node)) {
        const char *content = json_node_get_string(content_node);
        if (content != nullptr && *content != '\0') {
            *out_delta = g_strdup(content);
        }
    }
    return true;
}

typedef struct {
    VertereProvider *provider;
    GCancellable *cancellable;
    gulong cancel_handler_id;
    GCancellable *io_cancel;
    guint timeout_id;
    const char *timeout_reason;
    bool timed_out;
    SoupMessage *msg;
    GDataInputStream *stream;
    VertereDeltaCallback on_delta;
    VertereDoneCallback on_done;
    gpointer user_data;
    GDestroyNotify destroy_data;
} StreamCtx;

static void stream_ctx_finish(StreamCtx *ctx, const GError *error) {
    if (ctx->timeout_id != 0) {
        g_source_remove(ctx->timeout_id);
        ctx->timeout_id = 0;
    }
    if (ctx->cancel_handler_id != 0 && ctx->cancellable != nullptr) {
        g_cancellable_disconnect(ctx->cancellable, ctx->cancel_handler_id);
        ctx->cancel_handler_id = 0;
    }
    if (ctx->on_done != nullptr) {
        ctx->on_done(error, ctx->user_data);
    }
    if (ctx->destroy_data != nullptr && ctx->user_data != nullptr) {
        ctx->destroy_data(ctx->user_data);
    }
    g_clear_object(&ctx->stream);
    g_clear_object(&ctx->msg);
    g_clear_object(&ctx->io_cancel);
    g_clear_object(&ctx->cancellable);
    vertere_provider_unref(ctx->provider);
    g_free(ctx);
}

static gboolean on_stream_timeout(gpointer user_data) {
    StreamCtx *ctx = user_data;
    ctx->timeout_id = 0;
    ctx->timed_out = true;
    g_cancellable_cancel(ctx->io_cancel);
    return G_SOURCE_REMOVE;
}

static void reset_stream_timeout(StreamCtx *ctx, unsigned int secs, const char *reason) {
    if (ctx->timeout_id != 0) {
        g_source_remove(ctx->timeout_id);
    }
    ctx->timeout_reason = reason;
    ctx->timeout_id = g_timeout_add_seconds(secs, on_stream_timeout, ctx);
}

static void on_stream_cancelled(GCancellable *, gpointer user_data) {
    StreamCtx *ctx = user_data;
    g_cancellable_cancel(ctx->io_cancel);
}

static void read_next_line(StreamCtx *ctx);

static void on_line_read(GObject *source, GAsyncResult *res, gpointer user_data) {
    StreamCtx *ctx = user_data;
    if (ctx->timeout_id != 0) {
        g_source_remove(ctx->timeout_id);
        ctx->timeout_id = 0;
    }

    g_autoptr(GError) error = nullptr;
    gsize len = 0;
    g_autofree char *line = g_data_input_stream_read_line_finish_utf8(
        G_DATA_INPUT_STREAM(source), res, &len, &error);

    if (error != nullptr) {
        if (ctx->timed_out) {
            g_clear_error(&error);
            g_set_error(&error, VERTERE_PROVIDER_ERROR, 9, "%s", ctx->timeout_reason);
        }
        stream_ctx_finish(ctx, error);
        return;
    }

    if (line == nullptr) {
        stream_ctx_finish(ctx, nullptr);
        return;
    }

    g_autofree char *delta = nullptr;
    bool done = false;
    if (!vertere_provider_parse_sse_line(line, &delta, &done, &error)) {
        stream_ctx_finish(ctx, error);
        return;
    }

    if (delta != nullptr && ctx->on_delta != nullptr) {
        ctx->on_delta(delta, ctx->user_data);
    }
    if (done) {
        stream_ctx_finish(ctx, nullptr);
        return;
    }
    if (g_cancellable_is_cancelled(ctx->io_cancel)) {
        g_autoptr(GError) cancel_err = g_error_new_literal(
            G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
        stream_ctx_finish(ctx, cancel_err);
        return;
    }

    read_next_line(ctx);
}

static void read_next_line(StreamCtx *ctx) {
    reset_stream_timeout(ctx, READ_TIMEOUT_SECS, "the connection went silent");
    g_data_input_stream_read_line_async(
        ctx->stream, G_PRIORITY_DEFAULT, ctx->io_cancel, on_line_read, ctx);
}

static void on_error_body_read(GObject *source, GAsyncResult *res, gpointer user_data) {
    StreamCtx *ctx = user_data;
    g_autoptr(GError) read_err = nullptr;
    g_autoptr(GBytes) body = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), res, &read_err);
    guint status = soup_message_get_status(ctx->msg);

    g_autofree char *snippet = nullptr;
    if (body != nullptr) {
        gsize sz = 0;
        const char *raw = g_bytes_get_data(body, &sz);
        snippet = g_strndup(raw != nullptr ? raw : "", MIN(sz, (gsize)256));
    } else {
        snippet = g_strdup(soup_message_get_reason_phrase(ctx->msg));
    }

    g_autoptr(GError) err = g_error_new(
        VERTERE_PROVIDER_ERROR, 10,
        "cannot reach the endpoint: HTTP %u %s",
        status, snippet != nullptr ? snippet : "");
    stream_ctx_finish(ctx, err);
}

static void on_chat_response(GObject *source, GAsyncResult *res, gpointer user_data) {
    StreamCtx *ctx = user_data;
    if (ctx->timeout_id != 0) {
        g_source_remove(ctx->timeout_id);
        ctx->timeout_id = 0;
    }

    g_autoptr(GError) error = nullptr;
    g_autoptr(GInputStream) body_stream = soup_session_send_finish(SOUP_SESSION(source), res, &error);
    if (body_stream == nullptr) {
        if (ctx->timed_out) {
            g_clear_error(&error);
            g_set_error(&error, VERTERE_PROVIDER_ERROR, 11, "%s", ctx->timeout_reason);
        }
        stream_ctx_finish(ctx, error);
        return;
    }

    guint status = soup_message_get_status(ctx->msg);
    if (status < 200 || status >= 300) {
        g_input_stream_read_bytes_async(
            body_stream, 512, G_PRIORITY_DEFAULT, ctx->io_cancel, on_error_body_read, ctx);
        return;
    }

    ctx->stream = g_data_input_stream_new(body_stream);
    g_data_input_stream_set_newline_type(ctx->stream, G_DATA_STREAM_NEWLINE_TYPE_ANY);
    read_next_line(ctx);
}

bool vertere_provider_validate_extra_json(const char *extra_json, GError **error) {
    g_autofree char *trimmed = vertere_trim_dup(extra_json);
    if (*trimmed == '\0') {
        return true;
    }

    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, trimmed, -1, error)) {
        return false;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root == nullptr || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error(error, VERTERE_PROVIDER_ERROR, 13,
                    "Extra JSON must be a JSON object (e.g. {\"thinking\":{\"type\":\"disabled\"}})");
        return false;
    }
    return true;
}

static void merge_extra_json(JsonBuilder *b, const char *extra_json) {
    g_autofree char *trimmed = vertere_trim_dup(extra_json);
    if (*trimmed == '\0') {
        return;
    }
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, trimmed, -1, nullptr)) {
        return;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (root == nullptr || !JSON_NODE_HOLDS_OBJECT(root)) {
        return;
    }
    JsonObject *obj = json_node_get_object(root);
    JsonObjectIter iter;
    const char *member_name = nullptr;
    JsonNode *member_node = nullptr;
    json_object_iter_init(&iter, obj);
    while (json_object_iter_next(&iter, &member_name, &member_node)) {
        if (g_strcmp0(member_name, "model") == 0 ||
            g_strcmp0(member_name, "stream") == 0 ||
            g_strcmp0(member_name, "messages") == 0) {
            continue;
        }
        json_builder_set_member_name(b, member_name);
        json_builder_add_value(b, json_node_copy(member_node));
    }
}

static char *serialize_chat_payload(
    const char *model,
    const char *system_prompt,
    JsonBuilder *user_parts_builder,
    const char *extra_json,
    size_t *out_len
) {
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "model");
    json_builder_add_string_value(b, model);
    json_builder_set_member_name(b, "stream");
    json_builder_add_boolean_value(b, TRUE);

    merge_extra_json(b, extra_json);

    json_builder_set_member_name(b, "messages");
    json_builder_begin_array(b);

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "role");
    json_builder_add_string_value(b, "system");
    json_builder_set_member_name(b, "content");
    json_builder_add_string_value(b, system_prompt);
    json_builder_end_object(b);

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "role");
    json_builder_add_string_value(b, "user");
    json_builder_set_member_name(b, "content");
    g_autoptr(JsonNode) parts_node = json_builder_get_root(user_parts_builder);
    json_builder_add_value(b, g_steal_pointer(&parts_node));
    json_builder_end_object(b);

    json_builder_end_array(b);
    json_builder_end_object(b);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    gsize payload_len = 0;
    char *payload = json_generator_to_data(gen, &payload_len);
    if (out_len != nullptr) {
        *out_len = payload_len;
    }
    return payload;
}

char *vertere_provider_build_text_payload(
    const char *model,
    const char *system_prompt,
    const char *user_text,
    const char *extra_json,
    size_t *out_len
) {
    g_autoptr(JsonBuilder) parts = json_builder_new();
    json_builder_begin_array(parts);
    json_builder_begin_object(parts);
    json_builder_set_member_name(parts, "type");
    json_builder_add_string_value(parts, "text");
    json_builder_set_member_name(parts, "text");
    json_builder_add_string_value(parts, user_text);
    json_builder_end_object(parts);
    json_builder_end_array(parts);

    return serialize_chat_payload(model, system_prompt, parts, extra_json, out_len);
}

static void start_chat_stream(
    VertereProvider *provider,
    const char *system_prompt,
    JsonBuilder *user_parts_builder,
    GCancellable *cancellable,
    VertereDeltaCallback on_delta,
    VertereDoneCallback on_done,
    gpointer user_data,
    GDestroyNotify destroy_data
) {
    g_autofree char *url = g_strdup_printf("%s/chat/completions", provider->base_url);
    SoupMessage *msg = soup_message_new("POST", url);
    if (msg == nullptr) {
        g_autoptr(GError) err = g_error_new(
            VERTERE_PROVIDER_ERROR, 12, "invalid base URL: %s", provider->base_url);
        on_done(err, user_data);
        if (destroy_data != nullptr && user_data != nullptr) {
            destroy_data(user_data);
        }
        return;
    }

    size_t payload_len = 0;
    char *payload = serialize_chat_payload(
        provider->model, system_prompt, user_parts_builder,
        provider->extra_json, &payload_len);
    g_autoptr(GBytes) body_bytes = g_bytes_new_take(payload, payload_len);

    g_autofree char *auth = g_strdup_printf("Bearer %s", provider->api_key);
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "Authorization", auth);
    soup_message_set_request_body_from_bytes(msg, "application/json", body_bytes);

    StreamCtx *ctx = g_new0(StreamCtx, 1);
    ctx->provider = vertere_provider_ref(provider);
    ctx->cancellable = cancellable != nullptr ? g_object_ref(cancellable) : nullptr;
    ctx->io_cancel = g_cancellable_new();
    ctx->msg = msg;
    ctx->on_delta = on_delta;
    ctx->on_done = on_done;
    ctx->user_data = user_data;
    ctx->destroy_data = destroy_data;

    if (ctx->cancellable != nullptr) {
        ctx->cancel_handler_id = g_cancellable_connect(
            ctx->cancellable, G_CALLBACK(on_stream_cancelled), ctx, nullptr);
    }
    reset_stream_timeout(ctx, CONNECT_TIMEOUT_SECS, "connecting to the endpoint timed out");

    soup_session_send_async(
        provider->session, msg, G_PRIORITY_DEFAULT, ctx->io_cancel, on_chat_response, ctx);
}

void vertere_provider_translate_image(
    VertereProvider *provider,
    GBytes *png,
    GCancellable *cancellable,
    VertereDeltaCallback on_delta,
    VertereDoneCallback on_done,
    gpointer user_data,
    GDestroyNotify destroy_data
) {
    gsize png_len = 0;
    const guchar *png_data = g_bytes_get_data(png, &png_len);
    g_autofree char *b64 = g_base64_encode(png_data, png_len);
    g_autofree char *data_url = g_strdup_printf("data:image/png;base64,%s", b64);

    g_autoptr(JsonBuilder) parts = json_builder_new();
    json_builder_begin_array(parts);

    json_builder_begin_object(parts);
    json_builder_set_member_name(parts, "type");
    json_builder_add_string_value(parts, "image_url");
    json_builder_set_member_name(parts, "image_url");
    json_builder_begin_object(parts);
    json_builder_set_member_name(parts, "url");
    json_builder_add_string_value(parts, data_url);
    json_builder_end_object(parts);
    json_builder_end_object(parts);

    json_builder_begin_object(parts);
    json_builder_set_member_name(parts, "type");
    json_builder_add_string_value(parts, "text");
    json_builder_set_member_name(parts, "text");
    json_builder_add_string_value(parts, "Translate the text in this image.");
    json_builder_end_object(parts);

    json_builder_end_array(parts);

    start_chat_stream(
        provider, provider->image_system_prompt, parts,
        cancellable, on_delta, on_done, user_data, destroy_data);
}

void vertere_provider_translate_text(
    VertereProvider *provider,
    const char *text,
    GCancellable *cancellable,
    VertereDeltaCallback on_delta,
    VertereDoneCallback on_done,
    gpointer user_data,
    GDestroyNotify destroy_data
) {
    g_autofree char *prompt = g_strdup_printf("Translate the following text.\n\n%s", text);

    g_autoptr(JsonBuilder) parts = json_builder_new();
    json_builder_begin_array(parts);

    json_builder_begin_object(parts);
    json_builder_set_member_name(parts, "type");
    json_builder_add_string_value(parts, "text");
    json_builder_set_member_name(parts, "text");
    json_builder_add_string_value(parts, prompt);
    json_builder_end_object(parts);

    json_builder_end_array(parts);

    start_chat_stream(
        provider, provider->text_system_prompt, parts,
        cancellable, on_delta, on_done, user_data, destroy_data);
}
