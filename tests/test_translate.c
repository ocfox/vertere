#include "translate.h"
#include "vertere.h"

#include <glib.h>
#include <string.h>

[[nodiscard]]
static VertereReply *reply_of(const char *const *deltas, size_t n) {
    VertereReply *r = vertere_reply_new();
    for (size_t i = 0; i < n; i++) {
        vertere_reply_push(r, deltas[i]);
    }
    return r;
}

static void test_collects_translation_before_separator(void) {
    const char *d[] = {"\xe4\xbd\xa0\xe5\xa5\xbd", "\xef\xbc\x8c", "\xe4\xb8\x96\xe7\x95\x8c"};
    g_autoptr(VertereReply) r = reply_of(d, _Countof(d));
    g_assert_cmpstr(vertere_reply_translation(r), ==, "你好，世界");
    g_assert_cmpstr(vertere_reply_source(r), ==, "");
    g_assert_false(vertere_reply_has_source(r));
}

static void test_splits_translation_from_source(void) {
    const char *d[] = {"你好\n\xe2\x81\x82\nHello"};
    g_autoptr(VertereReply) r = reply_of(d, _Countof(d));
    g_assert_cmpstr(vertere_reply_translation(r), ==, "你好");
    g_assert_cmpstr(vertere_reply_source(r), ==, "Hello");
    g_assert_true(vertere_reply_has_source(r));
}

static void test_splits_when_separator_arrives_alone(void) {
    const char *d[] = {"你好\n", "\xe2\x81\x82", "\nHello"};
    g_autoptr(VertereReply) r = reply_of(d, _Countof(d));
    g_assert_cmpstr(vertere_reply_translation(r), ==, "你好");
    g_assert_cmpstr(vertere_reply_source(r), ==, "Hello");
}

static void test_splits_when_separator_glued(void) {
    const char *d[] = {"你好\xe2\x81\x82Hel", "lo"};
    g_autoptr(VertereReply) r = reply_of(d, _Countof(d));
    g_assert_cmpstr(vertere_reply_translation(r), ==, "你好");
    g_assert_cmpstr(vertere_reply_source(r), ==, "Hello");
}

static void test_keeps_second_separator_in_source(void) {
    const char *d[] = {"你好\n\xe2\x81\x82\nHello \xe2\x81\x82 world"};
    g_autoptr(VertereReply) r = reply_of(d, _Countof(d));
    g_assert_cmpstr(vertere_reply_translation(r), ==, "你好");
    g_assert_cmpstr(vertere_reply_source(r), ==, "Hello ⁂ world");
}

static void test_keeps_newlines_inside_each_part(void) {
    const char *d[] = {"第一行\n第二行\n\xe2\x81\x82\nline one\nline two"};
    g_autoptr(VertereReply) r = reply_of(d, _Countof(d));
    g_assert_cmpstr(vertere_reply_translation(r), ==, "第一行\n第二行");
    g_assert_cmpstr(vertere_reply_source(r), ==, "line one\nline two");
}

static void test_tolerates_missing_or_empty_source(void) {
    const char *d1[] = {"你好"};
    g_autoptr(VertereReply) r1 = reply_of(d1, _Countof(d1));
    g_assert_cmpstr(vertere_reply_translation(r1), ==, "你好");
    g_assert_false(vertere_reply_has_source(r1));
    g_assert_false(vertere_reply_is_empty(r1));

    const char *d2[] = {"你好\n\xe2\x81\x82\n"};
    g_autoptr(VertereReply) r2 = reply_of(d2, _Countof(d2));
    g_assert_cmpstr(vertere_reply_translation(r2), ==, "你好");
    g_assert_cmpstr(vertere_reply_source(r2), ==, "");
    g_assert_true(vertere_reply_has_source(r2));
}

static void test_ignores_empty_deltas_and_whitespace(void) {
    const char *d1[] = {"", "你好", "", "\xe2\x81\x82", "", "Hello", ""};
    g_autoptr(VertereReply) r1 = reply_of(d1, _Countof(d1));
    g_assert_cmpstr(vertere_reply_translation(r1), ==, "你好");
    g_assert_cmpstr(vertere_reply_source(r1), ==, "Hello");

    const char *d2[] = {"  \n ", "\t"};
    g_autoptr(VertereReply) r2 = reply_of(d2, _Countof(d2));
    g_assert_true(vertere_reply_is_empty(r2));

    g_autoptr(VertereReply) r3 = vertere_reply_new();
    g_assert_true(vertere_reply_is_empty(r3));
}

static void test_prompts(void) {
    g_autofree char *img = vertere_image_system_prompt("Simplified Chinese", "");
    g_assert_nonnull(strstr(img, "Simplified Chinese"));
    g_assert_nonnull(strstr(img, VERTERE_SEPARATOR));
    g_assert_nonnull(strstr(img, "repeat it unchanged"));
    g_assert_null(strstr(img, "instead"));

    g_autofree char *txt = vertere_text_system_prompt("Simplified Chinese", "");
    g_assert_nonnull(strstr(txt, "Simplified Chinese"));
    g_assert_null(strstr(txt, VERTERE_SEPARATOR));

    g_autofree char *img_fb = vertere_image_system_prompt("Simplified Chinese", "English");
    g_autofree char *txt_fb = vertere_text_system_prompt("Simplified Chinese", "English");
    g_assert_nonnull(strstr(img_fb, "translate it into English instead"));
    g_assert_null(strstr(img_fb, "repeat it unchanged"));
    g_assert_nonnull(strstr(txt_fb, "translate it into English instead"));
    g_assert_null(strstr(txt_fb, "repeat it unchanged"));

    g_autofree char *img_blank = vertere_image_system_prompt("English", "   ");
    g_autofree char *img_empty = vertere_image_system_prompt("English", "");
    g_assert_cmpstr(img_blank, ==, img_empty);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/translate/collects_before_sep", test_collects_translation_before_separator);
    g_test_add_func("/translate/splits_from_source", test_splits_translation_from_source);
    g_test_add_func("/translate/sep_alone", test_splits_when_separator_arrives_alone);
    g_test_add_func("/translate/sep_glued", test_splits_when_separator_glued);
    g_test_add_func("/translate/keeps_second_sep", test_keeps_second_separator_in_source);
    g_test_add_func("/translate/keeps_newlines", test_keeps_newlines_inside_each_part);
    g_test_add_func("/translate/missing_or_empty_source", test_tolerates_missing_or_empty_source);
    g_test_add_func("/translate/empty_deltas_and_ws", test_ignores_empty_deltas_and_whitespace);
    g_test_add_func("/translate/prompts", test_prompts);
    return g_test_run();
}
