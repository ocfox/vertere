#pragma once

#include <glib.h>
#include <stdint.h>

/**
 * Extracts width and height from a PNG IHDR chunk.
 * Returns false if @data is not a valid PNG with an IHDR header.
 */
[[nodiscard]] [[gnu::nonnull(3, 4)]]
bool vertere_png_dimensions(
    const uint8_t *data,
    size_t len,
    uint32_t *out_width,
    uint32_t *out_height
);

/**
 * Lets the user drag a region via `slurp`, captures it via `grim`, and shrinks
 * oversized images (> 2048px on either side).
 *
 * If the user cancels selection, sets *out_cancelled = true and returns nullptr
 * with @error unset.
 */
[[nodiscard]] [[gnu::nonnull(1)]]
GBytes *vertere_capture_screenshot(bool *out_cancelled, GError **error);

/**
 * Reads plain text from the Wayland clipboard via `wl-paste`.
 */
[[nodiscard]]
char *vertere_capture_clipboard(GError **error);

/**
 * Reads plain text from the Wayland primary selection via `wl-paste --primary`.
 */
[[nodiscard]]
char *vertere_capture_selection(GError **error);
