#include "window_state.h"

#include <json-glib/json-glib.h>
#include <stdckdint.h>

[[nodiscard]]
static char *state_file_path(void) {
    return g_build_filename(g_get_user_state_dir(), "vertere", "window.json", nullptr);
}

void vertere_position_free(VerterePosition *pos) {
    if (pos == nullptr) {
        return;
    }
    g_free(pos->output);
    g_free(pos);
}

VerterePosition *vertere_window_state_load(void) {
    g_autofree char *path = state_file_path();
    g_autofree char *text = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path, &text, &len, nullptr)) {
        return nullptr;
    }

    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, text, (gssize)len, nullptr)) {
        return nullptr;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root == nullptr || !JSON_NODE_HOLDS_OBJECT(root)) {
        return nullptr;
    }

    JsonObject *obj = json_node_get_object(root);
    if (!json_object_has_member(obj, "output") ||
        !json_object_has_member(obj, "x") ||
        !json_object_has_member(obj, "y")) {
        return nullptr;
    }

    const char *output = json_object_get_string_member(obj, "output");
    if (output == nullptr) {
        return nullptr;
    }

    gint64 x = json_object_get_int_member(obj, "x");
    gint64 y = json_object_get_int_member(obj, "y");
    if (x < G_MININT || x > G_MAXINT || y < G_MININT || y > G_MAXINT) {
        return nullptr;
    }

    VerterePosition *pos = g_new0(VerterePosition, 1);
    pos->output = g_strdup(output);
    pos->x = (int)x;
    pos->y = (int)y;
    return pos;
}

bool vertere_window_state_save(const VerterePosition *pos, GError **error) {
    g_autofree char *path = state_file_path();
    g_autofree char *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0o755) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "cannot create %s", dir);
        return false;
    }

    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "output");
    json_builder_add_string_value(builder, pos->output != nullptr ? pos->output : "");
    json_builder_set_member_name(builder, "x");
    json_builder_add_int_value(builder, pos->x);
    json_builder_set_member_name(builder, "y");
    json_builder_add_int_value(builder, pos->y);
    json_builder_end_object(builder);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    g_autofree char *json = json_generator_to_data(gen, nullptr);

    return g_file_set_contents(path, json, -1, error);
}

void vertere_window_place(
    const VerterePosition *stored,
    const char *output,
    VertereScreen screen,
    VertereScreen size,
    int *out_x,
    int *out_y
) {
    constexpr int margin = 24;

    int raw_max_x = 0;
    int raw_max_y = 0;
    if (ckd_sub(&raw_max_x, screen.width, size.width) || ckd_sub(&raw_max_x, raw_max_x, margin)) {
        raw_max_x = margin;
    }
    if (ckd_sub(&raw_max_y, screen.height, size.height) || ckd_sub(&raw_max_y, raw_max_y, margin)) {
        raw_max_y = margin;
    }

    int max_x = MAX(raw_max_x, margin);
    int max_y = MAX(raw_max_y, margin);

    if (stored != nullptr && output != nullptr && g_strcmp0(stored->output, output) == 0) {
        *out_x = CLAMP(stored->x, margin, max_x);
        *out_y = CLAMP(stored->y, margin, max_y);
        return;
    }

    *out_x = max_x;
    *out_y = MAX((screen.height - size.height) / 2, margin);
}
