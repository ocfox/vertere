#pragma once

#include <glib.h>

typedef struct {
    char *output;
    int x;
    int y;
} VerterePosition;

typedef struct {
    int width;
    int height;
} VertereScreen;

void vertere_position_free(VerterePosition *pos);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VerterePosition, vertere_position_free)

/**
 * Reads the stored bubble position from $XDG_STATE_HOME/vertere/window.json,
 * or returns nullptr if absent or invalid.
 */
[[nodiscard]]
VerterePosition *vertere_window_state_load(void);

/**
 * Persists @pos to $XDG_STATE_HOME/vertere/window.json.
 */
[[nodiscard]] [[gnu::nonnull(1)]]
bool vertere_window_state_save(const VerterePosition *pos, GError **error);

/**
 * Computes clamped bubble coordinates on @screen for @output, honouring @stored
 * when it belongs to the same output.
 */
[[gnu::nonnull(5, 6)]]
void vertere_window_place(
    const VerterePosition *stored,
    const char *output,
    VertereScreen screen,
    VertereScreen size,
    int *out_x,
    int *out_y
);
