#pragma once

#include "store.h"
#include "translate.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

typedef void (*VertereBubbleDoneCallback)(const VertereReply *reply, gpointer user_data);

typedef void (*VertereTestResultCallback)(const GError *error, gpointer user_data);

typedef struct {
    bool (*save_settings)(const VertereSettings *settings, GError **error, gpointer user_data);
    void (*test_connection)(
        const char *api_key,
        const char *model,
        const char *base_url,
        GCancellable *cancellable,
        VertereTestResultCallback on_done,
        gpointer cb_data,
        GDestroyNotify cb_destroy,
        gpointer user_data
    );
    gpointer user_data;
} VertereSettingsOps;

typedef struct {
    GPtrArray *(*search)(const char *query, GError **error, gpointer user_data);
    bool (*delete_entry)(int64_t id, GError **error, gpointer user_data);
    gpointer user_data;
} VertereHistoryOps;

typedef struct _VertereBubbleHandle VertereBubbleHandle;

/**
 * Opens a floating layer-shell bubble for streaming deltas.
 * Returns a handle whose widgets are updated by vertere_bubble_push/finish/fail,
 * and sets *out_cancellable to a GCancellable cancelled when the bubble closes.
 */
[[nodiscard]] [[gnu::nonnull(1)]]
VertereBubbleHandle *vertere_ui_open_bubble(
    GtkApplication *app,
    const char *known_source,
    GCancellable **out_cancellable,
    VertereBubbleDoneCallback on_done,
    gpointer done_data,
    GDestroyNotify done_destroy
);

[[gnu::nonnull(1, 2)]]
void vertere_bubble_push_delta(VertereBubbleHandle *handle, const char *delta);

[[gnu::nonnull(1)]]
void vertere_bubble_finish(VertereBubbleHandle *handle, const GError *error);

void vertere_bubble_handle_unref(VertereBubbleHandle *handle);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VertereBubbleHandle, vertere_bubble_handle_unref)

/**
 * Opens a floating bubble displaying a single error message and auto-closes.
 */
[[gnu::nonnull(1, 2)]]
void vertere_ui_show_error(GtkApplication *app, const char *message);

/**
 * Opens (or raises) the settings window.
 */
[[gnu::nonnull(1, 2)]]
void vertere_ui_show_settings(
    GtkApplication *app,
    const VertereSettings *settings,
    VertereSettingsOps ops
);

/**
 * Opens (or reloads and raises) the history window.
 */
[[gnu::nonnull(1)]]
void vertere_ui_show_history(GtkApplication *app, VertereHistoryOps ops);
