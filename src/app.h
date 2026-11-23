#pragma once

#include <gtk/gtk.h>

typedef struct _VertereApp VertereApp;

[[nodiscard]]
VertereApp *vertere_app_new(GtkApplication *gtk_app);

void vertere_app_free(VertereApp *app);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VertereApp, vertere_app_free)

[[nodiscard]] [[gnu::nonnull(1)]]
bool vertere_app_start_daemon(VertereApp *app, GError **error);

[[gnu::nonnull(1)]]
void vertere_app_shot(VertereApp *app);

[[gnu::nonnull(1)]]
void vertere_app_clip(VertereApp *app);

[[gnu::nonnull(1)]]
void vertere_app_select(VertereApp *app);

[[gnu::nonnull(1)]]
void vertere_app_settings_window(VertereApp *app);

[[gnu::nonnull(1)]]
void vertere_app_history_window(VertereApp *app);
