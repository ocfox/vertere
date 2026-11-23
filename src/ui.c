#include "ui.h"
#include "provider.h"
#include "vertere.h"
#include "window_state.h"

#include <gtk4-layer-shell.h>
#include <string.h>

static constexpr int BUBBLE_WIDTH = 420;
static constexpr int BUBBLE_ASSUMED_HEIGHT = 200;
static constexpr unsigned int BUBBLE_LINGER_SECS = 8;
static constexpr unsigned int HISTORY_SEARCH_DELAY_MS = 200;

static constexpr unsigned char BUBBLE_CSS_BYTES[] = {
#embed "../data/bubble.css"
    , 0
};

static GWeakRef g_settings_window;
static GWeakRef g_history_window;
static void (*g_history_reload_fn)(GtkWindow *win);

static void ensure_css_loaded(void) {
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized)) {
        g_autoptr(GtkCssProvider) provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, (const char *)BUBBLE_CSS_BYTES);
        GdkDisplay *display = gdk_display_get_default();
        if (display != nullptr) {
            gtk_style_context_add_provider_for_display(
                display,
                GTK_STYLE_PROVIDER(provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
        g_once_init_leave(&initialized, 1);
    }
}

static void show_status(GtkLabel *status, const char *text) {
    gtk_label_set_text(status, text);
    gtk_widget_set_visible(GTK_WIDGET(status), TRUE);
}

typedef struct {
    void (*before_close)(GtkWindow *win);
} EscapeCtx;

static gboolean on_escape_key(
    GtkEventControllerKey *,
    guint keyval,
    guint,
    GdkModifierType,
    gpointer user_data
) {
    GtkWidget *win = GTK_WIDGET(user_data);
    if (keyval == GDK_KEY_Escape) {
        EscapeCtx *ctx = g_object_get_data(G_OBJECT(win), "vertere-escape-ctx");
        if (ctx != nullptr && ctx->before_close != nullptr) {
            ctx->before_close(GTK_WINDOW(win));
        }
        gtk_window_close(GTK_WINDOW(win));
        return TRUE;
    }
    return FALSE;
}

static void close_on_escape_with(GtkWindow *window, void (*before_close)(GtkWindow *)) {
    EscapeCtx *ctx = g_new0(EscapeCtx, 1);
    ctx->before_close = before_close;
    g_object_set_data_full(G_OBJECT(window), "vertere-escape-ctx", ctx, g_free);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_escape_key), window);
    gtk_widget_add_controller(GTK_WIDGET(window), keys);
}

static void close_on_escape(GtkWindow *window) {
    close_on_escape_with(window, nullptr);
}

/* ------------------------------------------------------------------------- */
/* Floating translation bubble                                               */
/* ------------------------------------------------------------------------- */

struct _VertereBubbleHandle {
    gatomicrefcount ref_count;
    GWeakRef window_ref;
    GWeakRef translation_ref;
    GWeakRef source_ref;
    GWeakRef source_row_ref;
    GWeakRef status_ref;
    GCancellable *cancellable;
    VertereReply *reply;
    bool touched;
    guint linger_id;
    VertereBubbleDoneCallback on_done;
    gpointer done_data;
    GDestroyNotify done_destroy;
};

static VertereBubbleHandle *bubble_handle_ref(VertereBubbleHandle *h) {
    g_atomic_ref_count_inc(&h->ref_count);
    return h;
}

void vertere_bubble_handle_unref(VertereBubbleHandle *h) {
    if (h == nullptr) {
        return;
    }
    if (!g_atomic_ref_count_dec(&h->ref_count)) {
        return;
    }
    if (h->linger_id != 0) {
        g_source_remove(h->linger_id);
        h->linger_id = 0;
    }
    if (h->done_destroy != nullptr && h->done_data != nullptr) {
        h->done_destroy(h->done_data);
    }
    g_weak_ref_clear(&h->window_ref);
    g_weak_ref_clear(&h->translation_ref);
    g_weak_ref_clear(&h->source_ref);
    g_weak_ref_clear(&h->source_row_ref);
    g_weak_ref_clear(&h->status_ref);
    g_clear_object(&h->cancellable);
    g_clear_pointer(&h->reply, vertere_reply_free);
    g_free(h);
}

static GdkMonitor *monitor_of(GtkWindow *window) {
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));
    if (display == nullptr) {
        display = gdk_display_get_default();
    }
    if (display == nullptr) {
        return nullptr;
    }
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (surface != nullptr) {
        GdkMonitor *mon = gdk_display_get_monitor_at_surface(display, surface);
        if (mon != nullptr) {
            return mon;
        }
    }
    GListModel *monitors = gdk_display_get_monitors(display);
    if (monitors != nullptr && g_list_model_get_n_items(monitors) > 0) {
        gpointer item = g_list_model_get_item(monitors, 0);
        GdkMonitor *mon = GDK_MONITOR(item);
        g_object_unref(item);
        return mon;
    }
    return nullptr;
}

static void place_bubble(GtkWindow *window) {
    GdkMonitor *monitor = monitor_of(window);
    if (monitor == nullptr) {
        return;
    }

    GdkRectangle geometry = {};
    gdk_monitor_get_geometry(monitor, &geometry);
    const char *connector = gdk_monitor_get_connector(monitor);
    const char *output = connector != nullptr ? connector : "";

    g_autoptr(VerterePosition) stored = vertere_window_state_load();
    int x = 0;
    int y = 0;
    vertere_window_place(
        stored,
        output,
        (VertereScreen){.width = geometry.width, .height = geometry.height},
        (VertereScreen){.width = BUBBLE_WIDTH, .height = BUBBLE_ASSUMED_HEIGHT},
        &x,
        &y);

    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, x);
    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, y);
}

static void remember_bubble(GtkWindow *window) {
    GdkMonitor *monitor = monitor_of(window);
    if (monitor == nullptr) {
        return;
    }
    const char *connector = gdk_monitor_get_connector(monitor);
    if (connector == nullptr) {
        return;
    }

    VerterePosition pos = {
        .output = (char *)connector,
        .x = gtk_layer_get_margin(window, GTK_LAYER_SHELL_EDGE_LEFT),
        .y = gtk_layer_get_margin(window, GTK_LAYER_SHELL_EDGE_TOP),
    };
    g_autoptr(GError) err = nullptr;
    if (!vertere_window_state_save(&pos, &err)) {
        g_printerr("vertere: cannot remember the window position: %s\n", err->message);
    }
}

typedef struct {
    VertereBubbleHandle *handle;
    GtkWindow *window;
    int origin_x;
    int origin_y;
} DragCtx;

static void drag_ctx_free(gpointer data, GClosure *) {
    DragCtx *ctx = data;
    vertere_bubble_handle_unref(ctx->handle);
    g_free(ctx);
}

static void on_drag_begin(GtkGestureDrag *, double, double, gpointer user_data) {
    DragCtx *ctx = user_data;
    ctx->handle->touched = true;
    ctx->origin_x = gtk_layer_get_margin(ctx->window, GTK_LAYER_SHELL_EDGE_LEFT);
    ctx->origin_y = gtk_layer_get_margin(ctx->window, GTK_LAYER_SHELL_EDGE_TOP);
}

static void on_drag_update(GtkGestureDrag *, double dx, double dy, gpointer user_data) {
    DragCtx *ctx = user_data;
    int nx = MAX(ctx->origin_x + (int)dx, 0);
    int ny = MAX(ctx->origin_y + (int)dy, 0);
    gtk_layer_set_margin(ctx->window, GTK_LAYER_SHELL_EDGE_LEFT, nx);
    gtk_layer_set_margin(ctx->window, GTK_LAYER_SHELL_EDGE_TOP, ny);
}

static void on_drag_end(GtkGestureDrag *, double, double, gpointer user_data) {
    DragCtx *ctx = user_data;
    remember_bubble(ctx->window);
}

static void on_motion_enter(GtkEventControllerMotion *, double, double, gpointer user_data) {
    VertereBubbleHandle *h = user_data;
    h->touched = true;
}

static void handle_unref_closure(gpointer data, GClosure *) {
    vertere_bubble_handle_unref(data);
}

static char *label_selected_text(GtkLabel *label) {
    int start = 0;
    int end = 0;
    if (!gtk_label_get_selection_bounds(label, &start, &end) || start == end) {
        return nullptr;
    }
    const char *text = gtk_label_get_text(label);
    const char *s = g_utf8_offset_to_pointer(text, start);
    const char *e = g_utf8_offset_to_pointer(text, end);
    return g_strndup(s, (gsize)(e - s));
}

static gboolean on_hide_copied_status(gpointer user_data) {
    GWeakRef *status_ref = user_data;
    g_autoptr(GtkLabel) status = g_weak_ref_get(status_ref);
    if (status != nullptr && g_strcmp0(gtk_label_get_text(status), "Copied") == 0) {
        gtk_widget_set_visible(GTK_WIDGET(status), FALSE);
    }
    return G_SOURCE_REMOVE;
}

static void weak_ref_free(gpointer data) {
    GWeakRef *ref = data;
    g_weak_ref_clear(ref);
    g_free(ref);
}

static void copy_to_clipboard(const char *text, GtkLabel *status) {
    GdkDisplay *display = gdk_display_get_default();
    if (display != nullptr) {
        gdk_clipboard_set_text(gdk_display_get_clipboard(display), text);
    }
    show_status(status, "Copied");

    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, status);
    g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 1, on_hide_copied_status, ref, weak_ref_free);
}

typedef struct {
    GtkLabel *translation;
    GtkLabel *source;
    GtkLabel *status;
} CopyCtx;

static gboolean on_bubble_copy_key(
    GtkEventControllerKey *,
    guint keyval,
    guint,
    GdkModifierType state,
    gpointer user_data
) {
    CopyCtx *ctx = user_data;
    GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask();
    if ((keyval == GDK_KEY_c || keyval == GDK_KEY_C) && mods == GDK_CONTROL_MASK) {
        g_autofree char *selected = label_selected_text(ctx->translation);
        if (selected == nullptr) {
            selected = label_selected_text(ctx->source);
        }
        const char *text = selected != nullptr
            ? selected
            : gtk_label_get_text(ctx->translation);
        copy_to_clipboard(text, ctx->status);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_linger_timeout(gpointer user_data) {
    VertereBubbleHandle *h = user_data;
    h->linger_id = 0;
    if (!h->touched) {
        g_autoptr(GtkWindow) win = g_weak_ref_get(&h->window_ref);
        if (win != nullptr) {
            remember_bubble(win);
            gtk_window_close(win);
        }
    }
    return G_SOURCE_REMOVE;
}

static void start_linger(VertereBubbleHandle *h) {
    if (h->linger_id != 0) {
        g_source_remove(h->linger_id);
    }
    h->linger_id = g_timeout_add_seconds_full(
        G_PRIORITY_DEFAULT,
        BUBBLE_LINGER_SECS,
        on_linger_timeout,
        bubble_handle_ref(h),
        (GDestroyNotify)vertere_bubble_handle_unref);
}

static void on_bubble_destroy(GtkWidget *, gpointer user_data) {
    VertereBubbleHandle *h = user_data;
    g_cancellable_cancel(h->cancellable);
}

VertereBubbleHandle *vertere_ui_open_bubble(
    GtkApplication *app,
    const char *known_source,
    GCancellable **out_cancellable,
    VertereBubbleDoneCallback on_done,
    gpointer done_data,
    GDestroyNotify done_destroy
) {
    ensure_css_loaded();

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(window), BUBBLE_WIDTH, -1);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_widget_add_css_class(window, "vertere");

    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(window), VERTERE_LAYER_NAMESPACE);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);

    GtkWidget *translation = gtk_label_new("\xe2\x80\xa6");
    gtk_label_set_wrap(GTK_LABEL(translation), TRUE);
    gtk_label_set_selectable(GTK_LABEL(translation), TRUE);
    gtk_widget_set_focusable(translation, FALSE);
    gtk_widget_set_hexpand(translation, TRUE);
    gtk_label_set_xalign(GTK_LABEL(translation), 0.0f);
    gtk_widget_add_css_class(translation, "translation");

    GtkWidget *source = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(source), TRUE);
    gtk_label_set_selectable(GTK_LABEL(source), TRUE);
    gtk_widget_set_focusable(source, FALSE);
    gtk_label_set_xalign(GTK_LABEL(source), 0.0f);
    gtk_widget_add_css_class(source, "source");

    GtkWidget *source_row = gtk_expander_new("Source");
    gtk_expander_set_child(GTK_EXPANDER(source_row), source);
    gtk_widget_set_visible(source_row, FALSE);

    GtkWidget *status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
    gtk_widget_set_visible(status, FALSE);
    gtk_widget_add_css_class(status, "status");

    GtkWidget *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(bubble, "bubble");
    gtk_box_append(GTK_BOX(bubble), translation);
    gtk_box_append(GTK_BOX(bubble), source_row);
    gtk_box_append(GTK_BOX(bubble), status);
    gtk_window_set_child(GTK_WINDOW(window), bubble);

    VertereBubbleHandle *h = g_new0(VertereBubbleHandle, 1);
    g_atomic_ref_count_init(&h->ref_count);
    g_weak_ref_init(&h->window_ref, window);
    g_weak_ref_init(&h->translation_ref, translation);
    g_weak_ref_init(&h->source_ref, source);
    g_weak_ref_init(&h->source_row_ref, source_row);
    g_weak_ref_init(&h->status_ref, status);
    h->cancellable = g_cancellable_new();
    h->reply = known_source != nullptr
        ? vertere_reply_new_with_source(known_source)
        : vertere_reply_new();
    h->on_done = on_done;
    h->done_data = done_data;
    h->done_destroy = done_destroy;

    place_bubble(GTK_WINDOW(window));

    GtkGesture *drag = gtk_gesture_drag_new();
    DragCtx *drag_ctx = g_new0(DragCtx, 1);
    drag_ctx->handle = bubble_handle_ref(h);
    drag_ctx->window = GTK_WINDOW(window);
    g_signal_connect_data(drag, "drag-begin", G_CALLBACK(on_drag_begin),
                          drag_ctx, drag_ctx_free, 0);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), drag_ctx);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), drag_ctx);
    gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(drag));

    close_on_escape_with(GTK_WINDOW(window), remember_bubble);

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect_data(motion, "enter", G_CALLBACK(on_motion_enter),
                          bubble_handle_ref(h), handle_unref_closure, 0);
    gtk_widget_add_controller(window, motion);

    CopyCtx *copy_ctx = g_new0(CopyCtx, 1);
    copy_ctx->translation = GTK_LABEL(translation);
    copy_ctx->source = GTK_LABEL(source);
    copy_ctx->status = GTK_LABEL(status);
    g_object_set_data_full(G_OBJECT(window), "vertere-copy-ctx", copy_ctx, g_free);
    GtkEventController *copy_keys = gtk_event_controller_key_new();
    g_signal_connect(copy_keys, "key-pressed", G_CALLBACK(on_bubble_copy_key), copy_ctx);
    gtk_widget_add_controller(window, copy_keys);

    g_signal_connect_data(window, "destroy", G_CALLBACK(on_bubble_destroy),
                          bubble_handle_ref(h), handle_unref_closure, 0);

    if (known_source != nullptr) {
        gtk_label_set_text(GTK_LABEL(source), known_source);
        gtk_widget_set_visible(source_row, TRUE);
    }

    gtk_window_present(GTK_WINDOW(window));
    if (out_cancellable != nullptr) {
        *out_cancellable = g_object_ref(h->cancellable);
    }
    return h;
}

void vertere_bubble_push_delta(VertereBubbleHandle *h, const char *delta) {
    g_autoptr(GtkWindow) win = g_weak_ref_get(&h->window_ref);
    if (win == nullptr) {
        return;
    }

    vertere_reply_push(h->reply, delta);

    g_autoptr(GtkLabel) trans = g_weak_ref_get(&h->translation_ref);
    if (trans != nullptr) {
        gtk_label_set_text(trans, vertere_reply_translation(h->reply));
    }
    if (vertere_reply_has_source(h->reply)) {
        const char *src_text = vertere_reply_source(h->reply);
        if (*src_text != '\0') {
            g_autoptr(GtkLabel) src = g_weak_ref_get(&h->source_ref);
            if (src != nullptr) {
                gtk_label_set_text(src, src_text);
            }
            g_autoptr(GtkWidget) row = g_weak_ref_get(&h->source_row_ref);
            if (row != nullptr) {
                gtk_widget_set_visible(row, TRUE);
            }
        }
    }
}

void vertere_bubble_finish(VertereBubbleHandle *h, const GError *error) {
    g_autoptr(GtkWindow) win = g_weak_ref_get(&h->window_ref);
    if (win == nullptr) {
        return;
    }

    if (error != nullptr) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            return;
        }
        g_autoptr(GtkLabel) trans = g_weak_ref_get(&h->translation_ref);
        if (trans != nullptr) {
            gtk_label_set_text(trans, "");
        }
        g_autoptr(GtkLabel) status = g_weak_ref_get(&h->status_ref);
        if (status != nullptr) {
            g_autofree char *msg = g_strdup_printf("Failed: %s", error->message);
            show_status(status, msg);
        }
        start_linger(h);
        return;
    }

    if (vertere_reply_is_empty(h->reply)) {
        g_autoptr(GtkLabel) status = g_weak_ref_get(&h->status_ref);
        if (status != nullptr) {
            show_status(status, "No translation returned");
        }
        start_linger(h);
        return;
    }

    if (h->on_done != nullptr) {
        h->on_done(h->reply, h->done_data);
    }
    start_linger(h);
}

void vertere_ui_show_error(GtkApplication *app, const char *message) {
    g_autoptr(VertereBubbleHandle) h = vertere_ui_open_bubble(
        app, nullptr, nullptr, nullptr, nullptr, nullptr);
    g_autoptr(GError) err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, message);
    vertere_bubble_finish(h, err);
}

/* ------------------------------------------------------------------------- */
/* Settings window                                                           */
/* ------------------------------------------------------------------------- */

typedef struct {
    GtkWindow *window;
    GtkEditable *api_key;
    GtkEntry *model;
    GtkEntry *target;
    GtkEntry *fallback;
    GtkEntry *base_url;
    GtkEntry *extra_json;
    GtkLabel *status;
    GtkButton *test_btn;
    GCancellable *test_cancel;
    VertereSettingsOps ops;
} SettingsWinCtx;

static void settings_ctx_free(gpointer data) {
    SettingsWinCtx *ctx = data;
    if (ctx->test_cancel != nullptr) {
        g_cancellable_cancel(ctx->test_cancel);
        g_clear_object(&ctx->test_cancel);
    }
    g_free(ctx);
}

static GtkWidget *make_entry(const char *value, const char *placeholder) {
    GtkWidget *e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e), value != nullptr ? value : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
    gtk_widget_set_hexpand(e, TRUE);
    return e;
}

static void on_settings_save(GtkButton *, gpointer user_data) {
    SettingsWinCtx *ctx = user_data;
    g_autofree char *api_key = vertere_trim_dup(gtk_editable_get_text(ctx->api_key));
    g_autofree char *model = vertere_trim_dup(gtk_editable_get_text(GTK_EDITABLE(ctx->model)));
    g_autofree char *target = vertere_trim_dup(gtk_editable_get_text(GTK_EDITABLE(ctx->target)));
    g_autofree char *fallback = vertere_trim_dup(gtk_editable_get_text(GTK_EDITABLE(ctx->fallback)));
    g_autofree char *base_url = vertere_trim_dup(gtk_editable_get_text(GTK_EDITABLE(ctx->base_url)));
    g_autofree char *extra_json = vertere_trim_dup(gtk_editable_get_text(GTK_EDITABLE(ctx->extra_json)));

    VertereSettings s = {
        .api_key = api_key,
        .model = model,
        .target_lang = target,
        .fallback_lang = fallback,
        .base_url = base_url,
        .extra_json = extra_json,
    };

    if (!vertere_settings_is_usable(&s)) {
        show_status(ctx->status, "A model is required");
        return;
    }

    g_autoptr(GError) err = nullptr;
    if (!vertere_provider_validate_extra_json(s.extra_json, &err)) {
        g_autofree char *msg = g_strdup_printf("Invalid Extra JSON: %s", err->message);
        show_status(ctx->status, msg);
        return;
    }

    if (!ctx->ops.save_settings(&s, &err, ctx->ops.user_data)) {
        g_autofree char *msg = g_strdup_printf("Could not save: %s", err->message);
        show_status(ctx->status, msg);
        return;
    }
    gtk_window_close(ctx->window);
}

typedef struct {
    GWeakRef win_ref;
    GtkButton *test_btn;
    GtkLabel *status;
} TestCbCtx;

static void test_cb_ctx_free(gpointer data) {
    TestCbCtx *cb = data;
    g_weak_ref_clear(&cb->win_ref);
    g_free(cb);
}

static void on_test_done(const GError *error, gpointer user_data) {
    TestCbCtx *cb = user_data;
    g_autoptr(GtkWindow) win = g_weak_ref_get(&cb->win_ref);
    if (win == nullptr) {
        return;
    }
    gtk_widget_set_sensitive(GTK_WIDGET(cb->test_btn), TRUE);
    if (error == nullptr) {
        show_status(cb->status, "Looks good.");
    } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_autofree char *msg = g_strdup_printf("Test failed: %s", error->message);
        show_status(cb->status, msg);
    }
}

static void on_settings_test(GtkButton *, gpointer user_data) {
    SettingsWinCtx *ctx = user_data;
    g_autofree char *model = vertere_trim_dup(gtk_editable_get_text(GTK_EDITABLE(ctx->model)));
    if (*model == '\0') {
        show_status(ctx->status, "A model is required");
        return;
    }
    g_autofree char *api_key = vertere_trim_dup(gtk_editable_get_text(ctx->api_key));
    g_autofree char *base_url = vertere_trim_dup(gtk_editable_get_text(GTK_EDITABLE(ctx->base_url)));

    if (ctx->test_cancel != nullptr) {
        g_cancellable_cancel(ctx->test_cancel);
        g_clear_object(&ctx->test_cancel);
    }
    ctx->test_cancel = g_cancellable_new();

    show_status(ctx->status, "Testing\xe2\x80\xa6");
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->test_btn), FALSE);

    TestCbCtx *cb = g_new0(TestCbCtx, 1);
    g_weak_ref_init(&cb->win_ref, ctx->window);
    cb->test_btn = ctx->test_btn;
    cb->status = ctx->status;

    ctx->ops.test_connection(
        api_key, model, base_url, ctx->test_cancel,
        on_test_done, cb, test_cb_ctx_free, ctx->ops.user_data);
}

void vertere_ui_show_settings(
    GtkApplication *app,
    const VertereSettings *settings,
    VertereSettingsOps ops
) {
    ensure_css_loaded();

    g_autoptr(GtkWindow) existing = g_weak_ref_get(&g_settings_window);
    if (existing != nullptr) {
        gtk_window_present(existing);
        return;
    }

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Vertere settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 460, -1);

    GtkWidget *api_key = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(api_key), TRUE);
    gtk_editable_set_text(GTK_EDITABLE(api_key), settings->api_key != nullptr ? settings->api_key : "");
    g_object_set(api_key, "placeholder-text", "sk-... (or set $API_KEY)", nullptr);
    gtk_widget_set_hexpand(api_key, TRUE);

    GtkWidget *model = make_entry(settings->model, "deepseek/deepseek-flash");
    GtkWidget *target = make_entry(settings->target_lang, VERTERE_DEFAULT_TARGET_LANG);
    GtkWidget *fallback = make_entry(settings->fallback_lang, VERTERE_DEFAULT_FALLBACK_LANG);
    GtkWidget *base_url = make_entry(settings->base_url, VERTERE_DEFAULT_BASE_URL);
    GtkWidget *extra_json = make_entry(
        settings->extra_json, "{\"thinking\":{\"type\":\"disabled\"}}");

    GtkWidget *rows = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(rows), 8);
    gtk_grid_set_column_spacing(GTK_GRID(rows), 12);
    gtk_widget_set_margin_top(rows, 16);
    gtk_widget_set_margin_bottom(rows, 16);
    gtk_widget_set_margin_start(rows, 16);
    gtk_widget_set_margin_end(rows, 16);

    const struct {
        const char *label;
        GtkWidget *field;
        const char *hint;
    } fields[] = {
        {"API key", api_key, "Saved in vertere.db. Falls back to $API_KEY if empty."},
        {"Model", model, "A model slug from the endpoint below. Must accept image input for `shot`."},
        {"Translate into", target, "Named as you would say it to a person, not a code."},
        {"Unless already in it", fallback, "Then translate into this instead."},
        {"Endpoint", base_url, "Any OpenAI-compatible API. Defaults to Tencent TokenHub."},
        {"Extra JSON", extra_json, "Optional JSON object merged into requests (empty sends nothing)."},
    };

    for (size_t i = 0; i < _Countof(fields); i++) {
        int r = (int)i * 2;
        GtkWidget *caption = gtk_label_new(fields[i].label);
        gtk_label_set_xalign(GTK_LABEL(caption), 0.0f);
        GtkWidget *note = gtk_label_new(fields[i].hint);
        gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
        gtk_widget_add_css_class(note, "status");

        gtk_grid_attach(GTK_GRID(rows), caption, 0, r, 1, 1);
        gtk_grid_attach(GTK_GRID(rows), fields[i].field, 1, r, 1, 1);
        gtk_grid_attach(GTK_GRID(rows), note, 1, r + 1, 1, 1);
    }

    GtkWidget *status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
    gtk_widget_set_visible(status, FALSE);
    gtk_widget_add_css_class(status, "status");
    gtk_widget_set_margin_start(status, 16);
    gtk_widget_set_margin_end(status, 16);

    GtkWidget *save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save, "suggested-action");
    GtkWidget *test = gtk_button_new_with_label("Test");

    SettingsWinCtx *ctx = g_new0(SettingsWinCtx, 1);
    ctx->window = GTK_WINDOW(window);
    ctx->api_key = GTK_EDITABLE(api_key);
    ctx->model = GTK_ENTRY(model);
    ctx->target = GTK_ENTRY(target);
    ctx->fallback = GTK_ENTRY(fallback);
    ctx->base_url = GTK_ENTRY(base_url);
    ctx->extra_json = GTK_ENTRY(extra_json);
    ctx->status = GTK_LABEL(status);
    ctx->test_btn = GTK_BUTTON(test);
    ctx->ops = ops;
    g_object_set_data_full(G_OBJECT(window), "vertere-settings-ctx", ctx, settings_ctx_free);

    g_signal_connect(save, "clicked", G_CALLBACK(on_settings_save), ctx);
    g_signal_connect(test, "clicked", G_CALLBACK(on_settings_test), ctx);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(actions), test);
    gtk_box_append(GTK_BOX(actions), save);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(body), rows);
    gtk_box_append(GTK_BOX(body), status);
    gtk_box_append(GTK_BOX(body), actions);
    gtk_widget_set_margin_bottom(body, 12);
    gtk_widget_set_margin_end(body, 16);
    gtk_window_set_child(GTK_WINDOW(window), body);

    close_on_escape(GTK_WINDOW(window));
    gtk_window_present(GTK_WINDOW(window));
    g_weak_ref_set(&g_settings_window, window);
}

/* ------------------------------------------------------------------------- */
/* History window                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    GtkSearchEntry *query;
    GtkListBox *list;
    GtkLabel *status;
    VertereHistoryOps ops;
} HistoryWinCtx;

typedef struct {
    HistoryWinCtx *win_ctx;
    int64_t id;
} DeleteRowCtx;

static void on_delete_clicked(GtkButton *button, gpointer user_data) {
    DeleteRowCtx *d = user_data;
    HistoryWinCtx *ctx = d->win_ctx;

    g_autoptr(GError) err = nullptr;
    if (!ctx->ops.delete_entry(d->id, &err, ctx->ops.user_data)) {
        g_autofree char *msg = g_strdup_printf("Could not delete: %s", err->message);
        show_status(ctx->status, msg);
        return;
    }

    gtk_widget_grab_focus(GTK_WIDGET(ctx->query));
    GtkWidget *row = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_LIST_BOX_ROW);
    if (row != nullptr) {
        gtk_list_box_remove(ctx->list, row);
    }
}

static GtkWidget *make_history_row(const VertereEntry *entry, HistoryWinCtx *ctx) {
    GtkWidget *translated = gtk_label_new(entry->translated);
    gtk_label_set_wrap(GTK_LABEL(translated), TRUE);
    gtk_label_set_selectable(GTK_LABEL(translated), TRUE);
    gtk_widget_set_focusable(translated, FALSE);
    gtk_label_set_xalign(GTK_LABEL(translated), 0.0f);

    GtkWidget *source = gtk_label_new(entry->source);
    gtk_label_set_wrap(GTK_LABEL(source), TRUE);
    gtk_label_set_selectable(GTK_LABEL(source), TRUE);
    gtk_widget_set_focusable(source, FALSE);
    gtk_label_set_xalign(GTK_LABEL(source), 0.0f);
    gtk_widget_add_css_class(source, "source");

    g_autofree char *meta_str = g_strdup_printf(
        "%s \xc2\xb7 %s", vertere_kind_to_str(entry->kind), entry->model);
    GtkWidget *meta = gtk_label_new(meta_str);
    gtk_widget_set_hexpand(meta, TRUE);
    gtk_label_set_xalign(GTK_LABEL(meta), 0.0f);
    gtk_widget_add_css_class(meta, "status");

    GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(remove, "flat");
    gtk_widget_set_tooltip_text(remove, "Delete");

    DeleteRowCtx *d = g_new0(DeleteRowCtx, 1);
    d->win_ctx = ctx;
    d->id = entry->id;
    g_object_set_data_full(G_OBJECT(remove), "vertere-delete-ctx", d, g_free);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_delete_clicked), d);

    GtkWidget *meta_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(meta_row), meta);
    gtk_box_append(GTK_BOX(meta_row), remove);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(row, 10);
    gtk_widget_set_margin_bottom(row, 10);
    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 12);
    gtk_box_append(GTK_BOX(row), translated);
    if (entry->source != nullptr && *entry->source != '\0') {
        gtk_box_append(GTK_BOX(row), source);
    }
    gtk_box_append(GTK_BOX(row), meta_row);
    return row;
}

static void refresh_history(HistoryWinCtx *ctx, const char *text) {
    for (GtkWidget *child = nullptr;
         (child = gtk_widget_get_first_child(GTK_WIDGET(ctx->list))) != nullptr;) {
        gtk_list_box_remove(ctx->list, child);
    }

    g_autoptr(GError) err = nullptr;
    g_autoptr(GPtrArray) entries = ctx->ops.search(text, &err, ctx->ops.user_data);
    if (entries == nullptr) {
        g_autofree char *msg = g_strdup_printf("Could not read: %s", err->message);
        show_status(ctx->status, msg);
        return;
    }

    if (entries->len == 0) {
        show_status(ctx->status, "Nothing here yet");
        return;
    }

    gtk_widget_set_visible(GTK_WIDGET(ctx->status), FALSE);
    for (guint i = 0; i < entries->len; i++) {
        const VertereEntry *entry = g_ptr_array_index(entries, i);
        gtk_list_box_append(ctx->list, make_history_row(entry, ctx));
    }
}

static void reload_history_window(GtkWindow *win) {
    HistoryWinCtx *ctx = g_object_get_data(G_OBJECT(win), "vertere-history-ctx");
    if (ctx != nullptr) {
        refresh_history(ctx, gtk_editable_get_text(GTK_EDITABLE(ctx->query)));
    }
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    HistoryWinCtx *ctx = user_data;
    refresh_history(ctx, gtk_editable_get_text(GTK_EDITABLE(entry)));
}

void vertere_ui_show_history(GtkApplication *app, VertereHistoryOps ops) {
    ensure_css_loaded();

    g_autoptr(GtkWindow) existing = g_weak_ref_get(&g_history_window);
    if (existing != nullptr) {
        if (g_history_reload_fn != nullptr) {
            g_history_reload_fn(existing);
        }
        gtk_window_present(existing);
        return;
    }

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Vertere history");
    gtk_window_set_default_size(GTK_WINDOW(window), 560, 600);

    GtkWidget *query = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(
        GTK_SEARCH_ENTRY(query), "Search the source text and the translations");
    gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(query), HISTORY_SEARCH_DELAY_MS);

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");

    GtkWidget *status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
    gtk_widget_set_visible(status, FALSE);
    gtk_widget_add_css_class(status, "status");

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
    gtk_widget_set_vexpand(scroller, TRUE);

    HistoryWinCtx *ctx = g_new0(HistoryWinCtx, 1);
    ctx->query = GTK_SEARCH_ENTRY(query);
    ctx->list = GTK_LIST_BOX(list);
    ctx->status = GTK_LABEL(status);
    ctx->ops = ops;
    g_object_set_data_full(G_OBJECT(window), "vertere-history-ctx", ctx, g_free);

    refresh_history(ctx, "");
    g_signal_connect(query, "search-changed", G_CALLBACK(on_search_changed), ctx);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(body, 12);
    gtk_widget_set_margin_bottom(body, 12);
    gtk_widget_set_margin_start(body, 12);
    gtk_widget_set_margin_end(body, 12);
    gtk_box_append(GTK_BOX(body), query);
    gtk_box_append(GTK_BOX(body), status);
    gtk_box_append(GTK_BOX(body), scroller);
    gtk_window_set_child(GTK_WINDOW(window), body);

    close_on_escape(GTK_WINDOW(window));
    gtk_window_present(GTK_WINDOW(window));
    g_history_reload_fn = reload_history_window;
    g_weak_ref_set(&g_history_window, window);
}
