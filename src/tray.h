#pragma once

#include <gio/gio.h>
#include <stdint.h>

typedef enum : uint8_t {
    VERTERE_TRAY_SHOT = 1,
    VERTERE_TRAY_CLIP = 2,
    VERTERE_TRAY_SELECTION = 3,
    VERTERE_TRAY_HISTORY = 5,
    VERTERE_TRAY_SETTINGS = 6,
    VERTERE_TRAY_QUIT = 8,
} VertereTrayAction;

typedef void (*VertereTrayCallback)(VertereTrayAction action, gpointer user_data);

typedef struct _VertereTray VertereTray;

/**
 * Publishes the StatusNotifierItem and DBusMenu on the session bus.
 * Non-fatal if no StatusNotifierWatcher is running yet; automatically
 * re-registers whenever a watcher appears.
 */
[[nodiscard]] [[gnu::nonnull(1)]]
VertereTray *vertere_tray_start(
    VertereTrayCallback on_action,
    gpointer user_data,
    GDestroyNotify destroy_data
);

void vertere_tray_stop(VertereTray *tray);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VertereTray, vertere_tray_stop)
