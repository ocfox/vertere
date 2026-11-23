#include "app.h"
#include "capture.h"
#include "provider.h"
#include "store.h"
#include "tray.h"
#include "ui.h"
#include "vertere.h"

struct _VertereApp {
    GtkApplication *gtk_app;
    VertereStore *store;
    VertereSettings *cached_settings;
    VertereProvider *cached_provider;
    VertereTray *tray;
    bool holding;
};

#define VERTERE_APP_ERROR (vertere_app_error_quark())
static GQuark vertere_app_error_quark(void) {
    return g_quark_from_static_string("vertere-app-error");
}

static char *resolve_api_key(const char *configured, GError **error) {
    g_autofree char *key = vertere_trim_dup(configured);
    if (*key != '\0') {
        return g_steal_pointer(&key);
    }

    const char *raw = g_getenv(VERTERE_API_KEY_ENV);
    g_autofree char *env_key = vertere_trim_dup(raw);
    if (*env_key != '\0') {
        return g_steal_pointer(&env_key);
    }

    g_set_error(error, VERTERE_APP_ERROR, 1,
                "API key is not set (configure in Settings or set $%s)",
                VERTERE_API_KEY_ENV);
    return nullptr;
}

static VertereStore *get_store(VertereApp *app, GError **error) {
    if (app->store == nullptr) {
        g_autofree char *db_path = g_build_filename(
            g_get_user_data_dir(), "vertere", "vertere.db", nullptr);
        app->store = vertere_store_open(db_path, error);
    }
    return app->store;
}

static VertereSettings *load_settings(VertereApp *app, GError **error) {
    VertereStore *store = get_store(app, error);
    if (store == nullptr) {
        return nullptr;
    }
    return vertere_store_settings(store, error);
}

static VertereProvider *get_session_provider(
    VertereApp *app,
    const VertereSettings *settings,
    GError **error
) {
    if (app->cached_provider != nullptr &&
        app->cached_settings != nullptr &&
        vertere_settings_equal(app->cached_settings, settings)) {
        return vertere_provider_ref(app->cached_provider);
    }

    g_autofree char *key = resolve_api_key(settings->api_key, error);
    if (key == nullptr) {
        return nullptr;
    }

    VertereProvider *provider = vertere_provider_new(settings, key, error);
    if (provider == nullptr) {
        return nullptr;
    }

    g_clear_pointer(&app->cached_settings, vertere_settings_free);
    g_clear_pointer(&app->cached_provider, vertere_provider_unref);
    app->cached_settings = vertere_settings_dup(settings);
    app->cached_provider = vertere_provider_ref(provider);
    return provider;
}

VertereApp *vertere_app_new(GtkApplication *gtk_app) {
    VertereApp *app = g_new0(VertereApp, 1);
    app->gtk_app = gtk_app;
    return app;
}

void vertere_app_free(VertereApp *app) {
    if (app == nullptr) {
        return;
    }
    if (app->holding) {
        g_application_release(G_APPLICATION(app->gtk_app));
        app->holding = false;
    }
    g_clear_pointer(&app->tray, vertere_tray_stop);
    g_clear_pointer(&app->cached_provider, vertere_provider_unref);
    g_clear_pointer(&app->cached_settings, vertere_settings_free);
    g_clear_pointer(&app->store, vertere_store_close);
    g_free(app);
}

static void on_startup_check_done(const GError *error, gpointer) {
    if (error != nullptr) {
        g_printerr("vertere: model check failed: %s\n", error->message);
    }
}

static void on_tray_action(VertereTrayAction action, gpointer user_data) {
    VertereApp *app = user_data;
    switch (action) {
    case VERTERE_TRAY_SHOT:
        vertere_app_shot(app);
        break;
    case VERTERE_TRAY_CLIP:
        vertere_app_clip(app);
        break;
    case VERTERE_TRAY_SELECTION:
        vertere_app_select(app);
        break;
    case VERTERE_TRAY_HISTORY:
        vertere_app_history_window(app);
        break;
    case VERTERE_TRAY_SETTINGS:
        vertere_app_settings_window(app);
        break;
    case VERTERE_TRAY_QUIT:
        g_application_quit(G_APPLICATION(app->gtk_app));
        break;
    }
}

bool vertere_app_start_daemon(VertereApp *app, GError **) {
    if (!app->holding) {
        g_application_hold(G_APPLICATION(app->gtk_app));
        app->holding = true;
    }
    if (app->tray == nullptr) {
        app->tray = vertere_tray_start(on_tray_action, app, nullptr);
    }

    g_autoptr(GError) err = nullptr;
    g_autoptr(VertereSettings) settings = load_settings(app, &err);
    if (settings != nullptr && vertere_settings_is_usable(settings)) {
        g_autoptr(VertereProvider) provider = get_session_provider(app, settings, &err);
        if (provider != nullptr) {
            vertere_provider_check_model(provider, nullptr, on_startup_check_done, nullptr, nullptr);
        }
    }
    return true;
}

typedef struct {
    VertereApp *app;
    VertereBubbleHandle *bubble;
    VertereKind kind;
    VertereSettings *settings;
} TranslateRunCtx;

static void translate_run_ctx_free(gpointer data) {
    TranslateRunCtx *ctx = data;
    vertere_bubble_handle_unref(ctx->bubble);
    vertere_settings_free(ctx->settings);
    g_free(ctx);
}

static void on_stream_delta(const char *delta, gpointer user_data) {
    TranslateRunCtx *ctx = user_data;
    vertere_bubble_push_delta(ctx->bubble, delta);
}

static void on_stream_done(const GError *error, gpointer user_data) {
    TranslateRunCtx *ctx = user_data;
    vertere_bubble_finish(ctx->bubble, error);
}

static void on_bubble_recorded(const VertereReply *reply, gpointer user_data) {
    TranslateRunCtx *ctx = user_data;
    if (vertere_reply_is_empty(reply)) {
        return;
    }
    g_autoptr(GError) err = nullptr;
    VertereStore *store = get_store(ctx->app, &err);
    if (store == nullptr) {
        g_printerr("vertere: %s\n", err->message);
        return;
    }
    g_autofree char *target = vertere_settings_dup_target(ctx->settings);
    VertereRecord rec = {
        .kind = ctx->kind,
        .model = ctx->settings->model,
        .target = target,
        .source = vertere_reply_source(reply),
        .translated = vertere_reply_translation(reply),
    };
    if (!vertere_store_add(store, &rec, nullptr, &err)) {
        g_printerr("vertere: %s\n", err->message);
    }
}

static void run_translation(
    VertereApp *app,
    VertereKind kind,
    GBytes *png,
    const char *text
) {
    g_autoptr(GError) err = nullptr;
    g_autoptr(VertereSettings) settings = load_settings(app, &err);
    if (settings == nullptr) {
        vertere_ui_show_error(app->gtk_app, err->message);
        return;
    }
    if (!vertere_settings_is_usable(settings)) {
        vertere_app_settings_window(app);
        return;
    }

    g_autoptr(VertereProvider) provider = get_session_provider(app, settings, &err);
    if (provider == nullptr) {
        vertere_ui_show_error(app->gtk_app, err->message);
        return;
    }

    TranslateRunCtx *ctx = g_new0(TranslateRunCtx, 1);
    ctx->app = app;
    ctx->kind = kind;
    ctx->settings = g_steal_pointer(&settings);

    g_autoptr(GCancellable) cancel = nullptr;
    ctx->bubble = vertere_ui_open_bubble(
        app->gtk_app,
        text,
        &cancel,
        on_bubble_recorded,
        ctx,
        nullptr);

    if (png != nullptr) {
        vertere_provider_translate_image(
            provider, png, cancel,
            on_stream_delta, on_stream_done, ctx, translate_run_ctx_free);
    } else {
        vertere_provider_translate_text(
            provider, text, cancel,
            on_stream_delta, on_stream_done, ctx, translate_run_ctx_free);
    }
}

void vertere_app_shot(VertereApp *app) {
    bool cancelled = false;
    g_autoptr(GError) err = nullptr;
    g_autoptr(GBytes) png = vertere_capture_screenshot(&cancelled, &err);
    if (cancelled) {
        return;
    }
    if (png == nullptr) {
        vertere_ui_show_error(app->gtk_app, err->message);
        return;
    }
    run_translation(app, VERTERE_KIND_SHOT, png, nullptr);
}

static void run_text_capture(
    VertereApp *app,
    VertereKind kind,
    char *(*capture_fn)(GError **),
    const char *when_empty
) {
    g_autoptr(GError) err = nullptr;
    g_autofree char *text = capture_fn(&err);
    if (text == nullptr) {
        vertere_ui_show_error(app->gtk_app, err->message);
        return;
    }
    g_autofree char *trimmed = vertere_trim_dup(text);
    if (*trimmed == '\0') {
        vertere_ui_show_error(app->gtk_app, when_empty);
        return;
    }
    run_translation(app, kind, nullptr, text);
}

void vertere_app_clip(VertereApp *app) {
    run_text_capture(app, VERTERE_KIND_CLIP, vertere_capture_clipboard, "the clipboard is empty");
}

void vertere_app_select(VertereApp *app) {
    run_text_capture(app, VERTERE_KIND_SELECT, vertere_capture_selection, "nothing is selected");
}

static bool save_settings_cb(const VertereSettings *settings, GError **error, gpointer user_data) {
    VertereApp *app = user_data;
    VertereStore *store = get_store(app, error);
    if (store == nullptr) {
        return false;
    }
    if (!vertere_store_save_settings(store, settings, error)) {
        return false;
    }
    g_clear_pointer(&app->cached_settings, vertere_settings_free);
    g_clear_pointer(&app->cached_provider, vertere_provider_unref);
    return true;
}

static void test_connection_cb(
    const char *api_key,
    const char *model,
    const char *base_url,
    GCancellable *cancellable,
    VertereTestResultCallback on_done,
    gpointer cb_data,
    GDestroyNotify cb_destroy,
    gpointer
) {
    g_autoptr(GError) err = nullptr;
    g_autofree char *key = resolve_api_key(api_key, &err);
    if (key == nullptr) {
        on_done(err, cb_data);
        if (cb_destroy != nullptr && cb_data != nullptr) {
            cb_destroy(cb_data);
        }
        return;
    }

    VertereSettings tmp = {
        .api_key = key,
        .model = (char *)model,
        .target_lang = (char *)"",
        .fallback_lang = (char *)"",
        .base_url = (char *)base_url,
        .extra_json = (char *)"",
    };
    g_autoptr(VertereProvider) provider = vertere_provider_new(&tmp, key, &err);
    if (provider == nullptr) {
        on_done(err, cb_data);
        if (cb_destroy != nullptr && cb_data != nullptr) {
            cb_destroy(cb_data);
        }
        return;
    }

    vertere_provider_check_model(provider, cancellable, on_done, cb_data, cb_destroy);
}

void vertere_app_settings_window(VertereApp *app) {
    g_autoptr(GError) err = nullptr;
    g_autoptr(VertereSettings) settings = load_settings(app, &err);
    if (settings == nullptr) {
        settings = vertere_settings_new();
    }

    VertereSettingsOps ops = {
        .save_settings = save_settings_cb,
        .test_connection = test_connection_cb,
        .user_data = app,
    };
    vertere_ui_show_settings(app->gtk_app, settings, ops);
}

static GPtrArray *history_search_cb(const char *query, GError **error, gpointer user_data) {
    VertereApp *app = user_data;
    VertereStore *store = get_store(app, error);
    if (store == nullptr) {
        return nullptr;
    }
    return vertere_store_search(store, query, 200, error);
}

static bool history_delete_cb(int64_t id, GError **error, gpointer user_data) {
    VertereApp *app = user_data;
    VertereStore *store = get_store(app, error);
    if (store == nullptr) {
        return false;
    }
    return vertere_store_delete(store, id, error);
}

void vertere_app_history_window(VertereApp *app) {
    VertereHistoryOps ops = {
        .search = history_search_cb,
        .delete_entry = history_delete_cb,
        .user_data = app,
    };
    vertere_ui_show_history(app->gtk_app, ops);
}
