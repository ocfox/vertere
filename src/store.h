#pragma once

#include <glib.h>
#include <stdint.h>

typedef enum : uint8_t {
    VERTERE_KIND_SHOT = 0,
    VERTERE_KIND_CLIP = 1,
    VERTERE_KIND_SELECT = 2,
} VertereKind;

typedef struct {
    char *api_key;
    char *model;
    char *target_lang;
    char *fallback_lang;
    char *base_url;
    char *extra_json;
} VertereSettings;

typedef struct {
    VertereKind kind;
    const char *model;
    const char *target;
    const char *source;
    const char *translated;
} VertereRecord;

typedef struct {
    int64_t id;
    int64_t created_at;
    VertereKind kind;
    char *model;
    char *target;
    char *source;
    char *translated;
} VertereEntry;

typedef struct _VertereStore VertereStore;

[[nodiscard]]
const char *vertere_kind_to_str(VertereKind kind);

[[nodiscard]] [[gnu::nonnull(2)]]
bool vertere_kind_from_str(const char *str, VertereKind *out_kind, GError **error);

[[nodiscard]]
VertereSettings *vertere_settings_new(void);

[[nodiscard]] [[gnu::nonnull(1)]]
VertereSettings *vertere_settings_dup(const VertereSettings *settings);

void vertere_settings_free(VertereSettings *settings);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VertereSettings, vertere_settings_free)

[[nodiscard]] [[gnu::nonnull(1, 2)]]
bool vertere_settings_equal(const VertereSettings *a, const VertereSettings *b);

[[nodiscard]] [[gnu::nonnull(1)]]
bool vertere_settings_is_usable(const VertereSettings *settings);

[[nodiscard]] [[gnu::nonnull(1)]]
char *vertere_settings_dup_target(const VertereSettings *settings);

[[nodiscard]] [[gnu::nonnull(1)]]
char *vertere_settings_dup_fallback(const VertereSettings *settings);

[[nodiscard]] [[gnu::nonnull(1)]]
char *vertere_settings_dup_base_url(const VertereSettings *settings);

void vertere_entry_free(VertereEntry *entry);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VertereEntry, vertere_entry_free)

[[nodiscard]] [[gnu::nonnull(1)]]
VertereStore *vertere_store_open(const char *path, GError **error);

[[nodiscard]]
VertereStore *vertere_store_open_in_memory(GError **error);

void vertere_store_close(VertereStore *store);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VertereStore, vertere_store_close)

[[nodiscard]] [[gnu::nonnull(1)]]
VertereSettings *vertere_store_settings(VertereStore *store, GError **error);

[[nodiscard]] [[gnu::nonnull(1, 2)]]
bool vertere_store_save_settings(
    VertereStore *store,
    const VertereSettings *settings,
    GError **error
);

[[nodiscard]] [[gnu::nonnull(1, 2)]]
bool vertere_store_add(
    VertereStore *store,
    const VertereRecord *record,
    int64_t *out_id,
    GError **error
);

[[nodiscard]] [[gnu::nonnull(1)]]
GPtrArray *vertere_store_recent(VertereStore *store, size_t limit, GError **error);

[[nodiscard]] [[gnu::nonnull(1, 2)]]
GPtrArray *vertere_store_search(
    VertereStore *store,
    const char *query,
    size_t limit,
    GError **error
);

[[nodiscard]] [[gnu::nonnull(1)]]
VertereEntry *vertere_store_get(
    VertereStore *store,
    int64_t id,
    bool *out_found,
    GError **error
);

[[nodiscard]] [[gnu::nonnull(1)]]
bool vertere_store_delete(VertereStore *store, int64_t id, GError **error);
