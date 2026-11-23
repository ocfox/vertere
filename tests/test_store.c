#include "store.h"
#include "vertere.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

static VertereRecord make_rec(const char *source, const char *translated) {
    return (VertereRecord){
        .kind = VERTERE_KIND_CLIP,
        .model = "vendor/model",
        .target = "zh-Hans",
        .source = source,
        .translated = translated,
    };
}

static void test_store_round_trip(void) {
    g_autoptr(GError) err = nullptr;
    g_autoptr(VertereStore) store = vertere_store_open_in_memory(&err);
    g_assert_no_error(err);

    VertereRecord rec = make_rec("Hello", "你好");
    int64_t id = 0;
    g_assert_true(vertere_store_add(store, &rec, &id, &err));
    g_assert_no_error(err);

    bool found = false;
    g_autoptr(VertereEntry) entry = vertere_store_get(store, id, &found, &err);
    g_assert_no_error(err);
    g_assert_true(found);
    g_assert_nonnull(entry);
    g_assert_cmpint(entry->id, ==, id);
    g_assert_cmpint(entry->kind, ==, VERTERE_KIND_CLIP);
    g_assert_cmpstr(entry->source, ==, "Hello");
    g_assert_cmpstr(entry->translated, ==, "你好");
    g_assert_cmpint(entry->created_at, >, 0);

    bool missing_found = true;
    g_autoptr(VertereEntry) missing = vertere_store_get(store, 404, &missing_found, &err);
    g_assert_no_error(err);
    g_assert_false(missing_found);
    g_assert_null(missing);
}

static void test_recent_order_and_limit(void) {
    g_autoptr(GError) err = nullptr;
    g_autoptr(VertereStore) store = vertere_store_open_in_memory(&err);
    g_assert_no_error(err);

    int64_t id1 = 0;
    int64_t id2 = 0;
    int64_t id3 = 0;
    VertereRecord r1 = make_rec("one", "一");
    VertereRecord r2 = make_rec("two", "二");
    VertereRecord r3 = make_rec("three", "三");
    g_assert_true(vertere_store_add(store, &r1, &id1, &err));
    g_assert_true(vertere_store_add(store, &r2, &id2, &err));
    g_assert_true(vertere_store_add(store, &r3, &id3, &err));

    g_autoptr(GPtrArray) recent = vertere_store_recent(store, 2, &err);
    g_assert_no_error(err);
    g_assert_cmpuint(recent->len, ==, 2);
    const VertereEntry *e0 = g_ptr_array_index(recent, 0);
    const VertereEntry *e1 = g_ptr_array_index(recent, 1);
    g_assert_cmpint(e0->id, ==, id3);
    g_assert_cmpint(e1->id, ==, id2);
}

static void test_search_and_wildcards(void) {
    g_autoptr(GError) err = nullptr;
    g_autoptr(VertereStore) store = vertere_store_open_in_memory(&err);
    g_assert_no_error(err);

    VertereRecord r1 = make_rec("The Quick Fox", "敏捷的狐狸");
    VertereRecord r2 = make_rec("100% sure", "百分百");
    VertereRecord r3 = make_rec("a slow turtle", "缓慢的乌龟");
    int64_t id1 = 0;
    g_assert_true(vertere_store_add(store, &r1, &id1, &err));
    g_assert_true(vertere_store_add(store, &r2, nullptr, &err));
    g_assert_true(vertere_store_add(store, &r3, nullptr, &err));

    g_autoptr(GPtrArray) f1 = vertere_store_search(store, "quick", 10, &err);
    g_assert_cmpuint(f1->len, ==, 1);

    g_autoptr(GPtrArray) f2 = vertere_store_search(store, "的狐", 10, &err);
    g_assert_cmpuint(f2->len, ==, 1);

    g_autoptr(GPtrArray) f3 = vertere_store_search(store, "%", 10, &err);
    g_assert_cmpuint(f3->len, ==, 1);
    g_assert_cmpstr(((VertereEntry *)g_ptr_array_index(f3, 0))->source, ==, "100% sure");

    g_autoptr(GPtrArray) f4 = vertere_store_search(store, "_", 10, &err);
    g_assert_cmpuint(f4->len, ==, 0);

    g_autoptr(GPtrArray) f5 = vertere_store_search(store, "   ", 10, &err);
    g_assert_cmpuint(f5->len, ==, 3);

    g_assert_true(vertere_store_delete(store, id1, &err));
    g_autoptr(GPtrArray) f6 = vertere_store_search(store, "fox", 10, &err);
    g_assert_cmpuint(f6->len, ==, 0);
}

static void test_reopening_file(void) {
    g_autoptr(GError) err = nullptr;
    g_autofree char *dir = g_dir_make_tmp("vertere-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_autofree char *path = g_build_filename(dir, "history.db", nullptr);

    {
        g_autoptr(VertereStore) s1 = vertere_store_open(path, &err);
        g_assert_no_error(err);
        VertereRecord r = make_rec("Hello", "你好");
        g_assert_true(vertere_store_add(s1, &r, nullptr, &err));
    }
    {
        g_autoptr(VertereStore) s2 = vertere_store_open(path, &err);
        g_assert_no_error(err);
        g_autoptr(GPtrArray) rows = vertere_store_recent(s2, 10, &err);
        g_assert_cmpuint(rows->len, ==, 1);
    }

    g_autofree char *wal = g_strdup_printf("%s-wal", path);
    g_autofree char *shm = g_strdup_printf("%s-shm", path);
    g_unlink(path);
    g_unlink(wal);
    g_unlink(shm);
    g_rmdir(dir);
}

static void test_settings_round_trip_and_defaults(void) {
    g_autoptr(GError) err = nullptr;
    g_autoptr(VertereStore) store = vertere_store_open_in_memory(&err);
    g_assert_no_error(err);

    g_autoptr(VertereSettings) def = vertere_store_settings(store, &err);
    g_assert_false(vertere_settings_is_usable(def));
    g_assert_cmpstr(def->extra_json, ==, "");

    g_autofree char *t = vertere_settings_dup_target(def);
    g_autofree char *fb = vertere_settings_dup_fallback(def);
    g_autofree char *url = vertere_settings_dup_base_url(def);
    g_assert_cmpstr(t, ==, VERTERE_DEFAULT_TARGET_LANG);
    g_assert_cmpstr(fb, ==, VERTERE_DEFAULT_FALLBACK_LANG);
    g_assert_cmpstr(url, ==, VERTERE_DEFAULT_BASE_URL);

    VertereSettings s = {
        .api_key = "sk-test-123",
        .model = "deepseek/deepseek-flash",
        .target_lang = " Japanese ",
        .fallback_lang = "German",
        .base_url = "https://example.com/v1",
        .extra_json = "{\"thinking\":{\"type\":\"disabled\"}}",
    };
    g_assert_true(vertere_settings_is_usable(&s));
    g_assert_true(vertere_store_save_settings(store, &s, &err));

    g_autoptr(VertereSettings) loaded = vertere_store_settings(store, &err);
    g_assert_true(vertere_settings_equal(loaded, &s));

    g_autofree char *lt = vertere_settings_dup_target(loaded);
    g_autofree char *lfb = vertere_settings_dup_fallback(loaded);
    g_autofree char *lurl = vertere_settings_dup_base_url(loaded);
    g_assert_cmpstr(lt, ==, "Japanese");
    g_assert_cmpstr(lfb, ==, "German");
    g_assert_cmpstr(lurl, ==, "https://example.com/v1");

    VertereSettings ws = {.model = "  ", .target_lang = " \n ", .fallback_lang = "", .base_url = ""};
    g_assert_false(vertere_settings_is_usable(&ws));
    g_autofree char *wt = vertere_settings_dup_target(&ws);
    g_assert_cmpstr(wt, ==, VERTERE_DEFAULT_TARGET_LANG);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/store/round_trip", test_store_round_trip);
    g_test_add_func("/store/recent_order_and_limit", test_recent_order_and_limit);
    g_test_add_func("/store/search_and_wildcards", test_search_and_wildcards);
    g_test_add_func("/store/reopening_file", test_reopening_file);
    g_test_add_func("/store/settings", test_settings_round_trip_and_defaults);
    return g_test_run();
}
