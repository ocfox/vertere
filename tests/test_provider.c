#include "provider.h"

#include <glib.h>
#include <string.h>

static void test_validate_models_json(void) {
    static constexpr char JSON[] =
        "{\"data\":["
        "{\"id\":\"text-only\",\"architecture\":{\"input_modalities\":[\"text\"]}},"
        "{\"id\":\"vision-new\",\"architecture\":{\"input_modalities\":[\"text\",\"IMAGE\"]}},"
        "{\"id\":\"vision-old\",\"architecture\":{\"modality\":\"text+image->text\"}},"
        "{\"id\":\"text-old\",\"architecture\":{\"modality\":\"text->text\"}},"
        "{\"id\":\"proxy-unknown\",\"architecture\":{}}"
        "]}";
    size_t len = strlen(JSON);

    g_autoptr(GError) err = nullptr;
    g_assert_true(vertere_provider_validate_models_json(JSON, len, "vision-new", &err));
    g_assert_no_error(err);

    g_assert_true(vertere_provider_validate_models_json(JSON, len, "vision-old", &err));
    g_assert_no_error(err);

    g_assert_true(vertere_provider_validate_models_json(JSON, len, "proxy-unknown", &err));
    g_assert_no_error(err);

    g_assert_false(vertere_provider_validate_models_json(JSON, len, "text-only", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

    g_assert_false(vertere_provider_validate_models_json(JSON, len, "text-old", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

    g_assert_false(vertere_provider_validate_models_json(JSON, len, "missing", &err));
    g_assert_nonnull(err);
}

static void test_parse_sse_line(void) {
    g_autoptr(GError) err = nullptr;
    g_autofree char *delta = nullptr;
    bool done = false;

    g_assert_true(vertere_provider_parse_sse_line(": OPENROUTER PROCESSING\r\n", &delta, &done, &err));
    g_assert_null(delta);
    g_assert_false(done);

    g_assert_true(vertere_provider_parse_sse_line(
        "data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}\r\n",
        &delta, &done, &err));
    g_assert_no_error(err);
    g_assert_cmpstr(delta, ==, "你好");
    g_assert_false(done);
    g_clear_pointer(&delta, g_free);

    g_assert_true(vertere_provider_parse_sse_line("data: [DONE]\n", &delta, &done, &err));
    g_assert_null(delta);
    g_assert_true(done);

    g_assert_false(vertere_provider_parse_sse_line(
        "data: {\"error\":{\"message\":\"rate limited\"}}\n",
        &delta, &done, &err));
    g_assert_nonnull(err);
}

static void test_extra_json_validation_and_payload(void) {
    g_autoptr(GError) err = nullptr;
    g_assert_true(vertere_provider_validate_extra_json("", &err));
    g_assert_true(vertere_provider_validate_extra_json("   ", &err));
    g_assert_true(vertere_provider_validate_extra_json("{\"thinking\":{\"type\":\"disabled\"}}", &err));
    g_assert_no_error(err);

    g_assert_false(vertere_provider_validate_extra_json("[1,2,3]", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

    g_assert_false(vertere_provider_validate_extra_json("{bad json", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

    size_t len = 0;
    g_autofree char *with_extra = vertere_provider_build_text_payload(
        "deepseek/deepseek-flash", "sys", "hello",
        "{\"thinking\":{\"type\":\"disabled\"},\"reasoning_effort\":\"none\",\"model\":\"hijack\"}",
        &len);
    g_assert_nonnull(with_extra);
    g_assert_cmpuint(len, >, 0);
    g_assert_nonnull(strstr(with_extra, "\"thinking\":{\"type\":\"disabled\"}"));
    g_assert_nonnull(strstr(with_extra, "\"reasoning_effort\":\"none\""));
    g_assert_nonnull(strstr(with_extra, "\"model\":\"deepseek/deepseek-flash\""));
    g_assert_null(strstr(with_extra, "hijack"));

    g_autofree char *empty_extra = vertere_provider_build_text_payload(
        "deepseek/deepseek-flash", "sys", "hello", "  ", &len);
    g_assert_nonnull(empty_extra);
    g_assert_null(strstr(empty_extra, "\"thinking\""));
    g_assert_null(strstr(empty_extra, "\"reasoning_effort\""));
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/provider/validate_models", test_validate_models_json);
    g_test_add_func("/provider/parse_sse", test_parse_sse_line);
    g_test_add_func("/provider/extra_json", test_extra_json_validation_and_payload);
    return g_test_run();
}
