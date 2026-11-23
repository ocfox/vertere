#include "store.h"
#include "vertere.h"

#include <errno.h>
#include <sqlite3.h>
#include <string.h>

struct _VertereStore {
    sqlite3 *db;
};

static inline void stmt_cleanup(sqlite3_stmt *stmt) {
    if (stmt != nullptr) {
        sqlite3_finalize(stmt);
    }
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(sqlite3_stmt, stmt_cleanup)

#define VERTERE_STORE_ERROR (vertere_store_error_quark())
static GQuark vertere_store_error_quark(void) {
    return g_quark_from_static_string("vertere-store-error");
}

static bool set_sqlite_error(sqlite3 *db, int rc, const char *context, GError **error) {
    g_set_error(error, VERTERE_STORE_ERROR, rc, "%s: %s",
                context, db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return false;
}

const char *vertere_kind_to_str(VertereKind kind) {
    switch (kind) {
    case VERTERE_KIND_SHOT:
        return "shot";
    case VERTERE_KIND_CLIP:
        return "clip";
    case VERTERE_KIND_SELECT:
        return "select";
    }
    g_return_val_if_reached("shot");
}

bool vertere_kind_from_str(const char *str, VertereKind *out_kind, GError **error) {
    if (g_strcmp0(str, "shot") == 0) {
        *out_kind = VERTERE_KIND_SHOT;
        return true;
    }
    if (g_strcmp0(str, "clip") == 0) {
        *out_kind = VERTERE_KIND_CLIP;
        return true;
    }
    if (g_strcmp0(str, "select") == 0) {
        *out_kind = VERTERE_KIND_SELECT;
        return true;
    }
    g_set_error(error, VERTERE_STORE_ERROR, 1, "unknown kind in history: %s",
                str != nullptr ? str : "(null)");
    return false;
}

VertereSettings *vertere_settings_new(void) {
    VertereSettings *s = g_new0(VertereSettings, 1);
    s->api_key = g_strdup("");
    s->model = g_strdup("");
    s->target_lang = g_strdup("");
    s->fallback_lang = g_strdup("");
    s->base_url = g_strdup("");
    s->extra_json = g_strdup("");
    return s;
}

VertereSettings *vertere_settings_dup(const VertereSettings *settings) {
    VertereSettings *s = g_new0(VertereSettings, 1);
    s->api_key = g_strdup(settings->api_key != nullptr ? settings->api_key : "");
    s->model = g_strdup(settings->model != nullptr ? settings->model : "");
    s->target_lang = g_strdup(settings->target_lang != nullptr ? settings->target_lang : "");
    s->fallback_lang = g_strdup(settings->fallback_lang != nullptr ? settings->fallback_lang : "");
    s->base_url = g_strdup(settings->base_url != nullptr ? settings->base_url : "");
    s->extra_json = g_strdup(settings->extra_json != nullptr ? settings->extra_json : "");
    return s;
}

void vertere_settings_free(VertereSettings *settings) {
    if (settings == nullptr) {
        return;
    }
    g_free(settings->api_key);
    g_free(settings->model);
    g_free(settings->target_lang);
    g_free(settings->fallback_lang);
    g_free(settings->base_url);
    g_free(settings->extra_json);
    g_free(settings);
}

bool vertere_settings_equal(const VertereSettings *a, const VertereSettings *b) {
    return g_strcmp0(a->api_key, b->api_key) == 0 &&
           g_strcmp0(a->model, b->model) == 0 &&
           g_strcmp0(a->target_lang, b->target_lang) == 0 &&
           g_strcmp0(a->fallback_lang, b->fallback_lang) == 0 &&
           g_strcmp0(a->base_url, b->base_url) == 0 &&
           g_strcmp0(a->extra_json, b->extra_json) == 0;
}

bool vertere_settings_is_usable(const VertereSettings *settings) {
    g_autofree char *trimmed = vertere_trim_dup(settings->model);
    return *trimmed != '\0';
}

[[nodiscard]]
static char *trimmed_or_default(const char *value, const char *fallback) {
    g_autofree char *trimmed = vertere_trim_dup(value);
    if (*trimmed != '\0') {
        return g_steal_pointer(&trimmed);
    }
    return g_strdup(fallback);
}

char *vertere_settings_dup_target(const VertereSettings *settings) {
    return trimmed_or_default(settings->target_lang, VERTERE_DEFAULT_TARGET_LANG);
}

char *vertere_settings_dup_fallback(const VertereSettings *settings) {
    return trimmed_or_default(settings->fallback_lang, VERTERE_DEFAULT_FALLBACK_LANG);
}

char *vertere_settings_dup_base_url(const VertereSettings *settings) {
    return trimmed_or_default(settings->base_url, VERTERE_DEFAULT_BASE_URL);
}

void vertere_entry_free(VertereEntry *entry) {
    if (entry == nullptr) {
        return;
    }
    g_free(entry->model);
    g_free(entry->target);
    g_free(entry->source);
    g_free(entry->translated);
    g_free(entry);
}

static bool exec_sql(sqlite3 *db, const char *sql, const char *ctx, GError **error) {
    char *errmsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        g_set_error(error, VERTERE_STORE_ERROR, rc, "%s: %s",
                    ctx, errmsg != nullptr ? errmsg : sqlite3_errstr(rc));
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

static bool migrate(VertereStore *store, GError **error) {
    g_autoptr(sqlite3_stmt) stmt = nullptr;
    int rc = sqlite3_prepare_v2(store->db, "PRAGMA user_version", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return set_sqlite_error(store->db, rc, "cannot read the schema version", error);
    }
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        return set_sqlite_error(store->db, sqlite3_errcode(store->db),
                                "cannot read the schema version", error);
    }
    int64_t version = sqlite3_column_int64(stmt, 0);
    g_clear_pointer(&stmt, stmt_cleanup);

    if (version < 1) {
        static constexpr char SQL_V1[] =
            "BEGIN;"
            "CREATE TABLE setting ("
            "  key   TEXT PRIMARY KEY,"
            "  value TEXT NOT NULL"
            ");"
            "CREATE TABLE entry ("
            "  id         INTEGER PRIMARY KEY,"
            "  created_at INTEGER NOT NULL,"
            "  kind       TEXT NOT NULL,"
            "  model      TEXT NOT NULL,"
            "  target     TEXT NOT NULL,"
            "  source     TEXT NOT NULL,"
            "  translated TEXT NOT NULL,"
            "  image_path TEXT"
            ");"
            "CREATE INDEX entry_created_at ON entry (created_at DESC);"
            "PRAGMA user_version = 1;"
            "COMMIT;";
        if (!exec_sql(store->db, SQL_V1, "cannot initialize database v1", error)) {
            return false;
        }
    }

    if (version < 2) {
        static constexpr char SQL_V2[] =
            "ALTER TABLE entry DROP COLUMN image_path;"
            "PRAGMA user_version = 2;";
        if (!exec_sql(store->db, SQL_V2, "cannot migrate database to v2", error)) {
            return false;
        }
    }

    return true;
}

[[nodiscard]]
static VertereStore *store_from_db(sqlite3 *db, GError **error) {
    sqlite3_busy_timeout(db, 5000);
    if (!exec_sql(db, "PRAGMA journal_mode = WAL; PRAGMA foreign_keys = ON;",
                  "cannot configure pragmas", error)) {
        sqlite3_close(db);
        return nullptr;
    }

    VertereStore *store = g_new0(VertereStore, 1);
    store->db = db;
    if (!migrate(store, error)) {
        vertere_store_close(store);
        return nullptr;
    }
    return store;
}

VertereStore *vertere_store_open(const char *path, GError **error) {
    g_autofree char *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0o755) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "cannot create %s", dir);
        return nullptr;
    }

    sqlite3 *db = nullptr;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        set_sqlite_error(db, rc, "cannot open history database", error);
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return nullptr;
    }
    return store_from_db(db, error);
}

VertereStore *vertere_store_open_in_memory(GError **error) {
    sqlite3 *db = nullptr;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) {
        set_sqlite_error(db, rc, "cannot open in-memory database", error);
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return nullptr;
    }
    return store_from_db(db, error);
}

void vertere_store_close(VertereStore *store) {
    if (store == nullptr) {
        return;
    }
    if (store->db != nullptr) {
        sqlite3_close(store->db);
    }
    g_free(store);
}

VertereSettings *vertere_store_settings(VertereStore *store, GError **error) {
    g_autoptr(sqlite3_stmt) stmt = nullptr;
    int rc = sqlite3_prepare_v2(store->db, "SELECT key, value FROM setting", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        set_sqlite_error(store->db, rc, "cannot prepare settings query", error);
        return nullptr;
    }

    g_autoptr(VertereSettings) settings = vertere_settings_new();
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *key = (const char *)sqlite3_column_text(stmt, 0);
        const char *value = (const char *)sqlite3_column_text(stmt, 1);
        if (key == nullptr || value == nullptr) {
            continue;
        }
        if (g_strcmp0(key, "api_key") == 0) {
            g_free(settings->api_key);
            settings->api_key = g_strdup(value);
        } else if (g_strcmp0(key, "model") == 0) {
            g_free(settings->model);
            settings->model = g_strdup(value);
        } else if (g_strcmp0(key, "target_lang") == 0) {
            g_free(settings->target_lang);
            settings->target_lang = g_strdup(value);
        } else if (g_strcmp0(key, "fallback_lang") == 0) {
            g_free(settings->fallback_lang);
            settings->fallback_lang = g_strdup(value);
        } else if (g_strcmp0(key, "base_url") == 0) {
            g_free(settings->base_url);
            settings->base_url = g_strdup(value);
        } else if (g_strcmp0(key, "extra_json") == 0) {
            g_free(settings->extra_json);
            settings->extra_json = g_strdup(value);
        }
    }
    if (rc != SQLITE_DONE) {
        set_sqlite_error(store->db, rc, "cannot read settings", error);
        return nullptr;
    }

    return g_steal_pointer(&settings);
}

bool vertere_store_save_settings(
    VertereStore *store,
    const VertereSettings *settings,
    GError **error
) {
    if (!exec_sql(store->db, "BEGIN", "cannot start settings transaction", error)) {
        return false;
    }

    g_autoptr(sqlite3_stmt) stmt = nullptr;
    int rc = sqlite3_prepare_v2(
        store->db,
        "INSERT INTO setting (key, value) VALUES (?1, ?2) "
        "ON CONFLICT (key) DO UPDATE SET value = excluded.value",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        set_sqlite_error(store->db, rc, "cannot prepare settings save", error);
        sqlite3_exec(store->db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    const struct {
        const char *key;
        const char *value;
    } pairs[] = {
        {"api_key", settings->api_key != nullptr ? settings->api_key : ""},
        {"model", settings->model != nullptr ? settings->model : ""},
        {"target_lang", settings->target_lang != nullptr ? settings->target_lang : ""},
        {"fallback_lang", settings->fallback_lang != nullptr ? settings->fallback_lang : ""},
        {"base_url", settings->base_url != nullptr ? settings->base_url : ""},
        {"extra_json", settings->extra_json != nullptr ? settings->extra_json : ""},
    };

    for (size_t i = 0; i < _Countof(pairs); i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, pairs[i].key, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, pairs[i].value, -1, SQLITE_STATIC);
        if ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
            set_sqlite_error(store->db, rc, "cannot save setting", error);
            sqlite3_exec(store->db, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
    }

    if (!exec_sql(store->db, "COMMIT", "cannot commit settings", error)) {
        sqlite3_exec(store->db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

bool vertere_store_add(
    VertereStore *store,
    const VertereRecord *record,
    int64_t *out_id,
    GError **error
) {
    g_autoptr(sqlite3_stmt) stmt = nullptr;
    int rc = sqlite3_prepare_v2(
        store->db,
        "INSERT INTO entry (created_at, kind, model, target, source, translated) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return set_sqlite_error(store->db, rc, "cannot prepare insert entry", error);
    }

    int64_t now = g_get_real_time() / G_USEC_PER_SEC;
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_text(stmt, 2, vertere_kind_to_str(record->kind), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, record->model, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, record->target, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, record->source, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, record->translated, -1, SQLITE_STATIC);

    if ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
        return set_sqlite_error(store->db, rc, "cannot insert history entry", error);
    }
    if (out_id != nullptr) {
        *out_id = sqlite3_last_insert_rowid(store->db);
    }
    return true;
}

[[nodiscard]]
static char *dup_col_text(sqlite3_stmt *stmt, int col) {
    const char *txt = (const char *)sqlite3_column_text(stmt, col);
    return g_strdup(txt != nullptr ? txt : "");
}

[[nodiscard]]
static VertereEntry *read_entry(sqlite3_stmt *stmt, GError **error) {
    const char *kind_str = (const char *)sqlite3_column_text(stmt, 2);
    VertereKind kind = VERTERE_KIND_SHOT;
    if (!vertere_kind_from_str(kind_str, &kind, error)) {
        return nullptr;
    }

    VertereEntry *e = g_new0(VertereEntry, 1);
    e->id = sqlite3_column_int64(stmt, 0);
    e->created_at = sqlite3_column_int64(stmt, 1);
    e->kind = kind;
    e->model = dup_col_text(stmt, 3);
    e->target = dup_col_text(stmt, 4);
    e->source = dup_col_text(stmt, 5);
    e->translated = dup_col_text(stmt, 6);
    return e;
}

[[nodiscard]]
static GPtrArray *collect_entries(VertereStore *store, sqlite3_stmt *stmt, GError **error) {
    g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func((GDestroyNotify)vertere_entry_free);
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        VertereEntry *entry = read_entry(stmt, error);
        if (entry == nullptr) {
            return nullptr;
        }
        g_ptr_array_add(entries, entry);
    }
    if (rc != SQLITE_DONE) {
        set_sqlite_error(store->db, rc, "cannot read history entries", error);
        return nullptr;
    }
    return g_steal_pointer(&entries);
}

GPtrArray *vertere_store_recent(VertereStore *store, size_t limit, GError **error) {
    g_autoptr(sqlite3_stmt) stmt = nullptr;
    int rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, created_at, kind, model, target, source, translated "
        "FROM entry ORDER BY created_at DESC, id DESC LIMIT ?1",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        set_sqlite_error(store->db, rc, "cannot prepare recent query", error);
        return nullptr;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)limit);
    return collect_entries(store, stmt, error);
}

[[nodiscard]]
static char *like_contains(const char *query) {
    GString *pat = g_string_new("%");
    for (const char *p = query; *p != '\0'; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') {
            g_string_append_c(pat, '\\');
        }
        g_string_append_c(pat, *p);
    }
    g_string_append_c(pat, '%');
    return g_string_free(pat, FALSE);
}

GPtrArray *vertere_store_search(
    VertereStore *store,
    const char *query,
    size_t limit,
    GError **error
) {
    g_autofree char *trimmed = vertere_trim_dup(query);
    if (*trimmed == '\0') {
        return vertere_store_recent(store, limit, error);
    }

    g_autoptr(sqlite3_stmt) stmt = nullptr;
    int rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, created_at, kind, model, target, source, translated "
        "FROM entry "
        "WHERE source LIKE ?1 ESCAPE '\\' OR translated LIKE ?1 ESCAPE '\\' "
        "ORDER BY created_at DESC, id DESC LIMIT ?2",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        set_sqlite_error(store->db, rc, "cannot prepare search query", error);
        return nullptr;
    }

    g_autofree char *pattern = like_contains(trimmed);
    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)limit);
    return collect_entries(store, stmt, error);
}

VertereEntry *vertere_store_get(
    VertereStore *store,
    int64_t id,
    bool *out_found,
    GError **error
) {
    if (out_found != nullptr) {
        *out_found = false;
    }
    g_autoptr(sqlite3_stmt) stmt = nullptr;
    int rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, created_at, kind, model, target, source, translated "
        "FROM entry WHERE id = ?1",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        set_sqlite_error(store->db, rc, "cannot prepare entry lookup", error);
        return nullptr;
    }
    sqlite3_bind_int64(stmt, 1, id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        return nullptr;
    }
    if (rc != SQLITE_ROW) {
        set_sqlite_error(store->db, rc, "cannot fetch entry", error);
        return nullptr;
    }
    if (out_found != nullptr) {
        *out_found = true;
    }
    return read_entry(stmt, error);
}

bool vertere_store_delete(VertereStore *store, int64_t id, GError **error) {
    g_autoptr(sqlite3_stmt) stmt = nullptr;
    int rc = sqlite3_prepare_v2(store->db, "DELETE FROM entry WHERE id = ?1", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return set_sqlite_error(store->db, rc, "cannot prepare delete", error);
    }
    sqlite3_bind_int64(stmt, 1, id);
    if ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
        return set_sqlite_error(store->db, rc, "cannot delete entry", error);
    }
    return true;
}
