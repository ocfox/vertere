#include "capture.h"
#include "vertere.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <math.h>
#include <string.h>

static constexpr uint32_t MAX_IMAGE_DIMENSION = 2048;

#define VERTERE_CAPTURE_ERROR (vertere_capture_error_quark())
static GQuark vertere_capture_error_quark(void) {
    return g_quark_from_static_string("vertere-capture-error");
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

bool vertere_png_dimensions(
    const uint8_t *data,
    size_t len,
    uint32_t *out_width,
    uint32_t *out_height
) {
    static constexpr uint8_t PNG_SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (data == nullptr || len < 24) {
        return false;
    }
    if (memcmp(data, PNG_SIG, sizeof(PNG_SIG)) != 0 ||
        memcmp(data + 12, "IHDR", 4) != 0) {
        return false;
    }
    *out_width = read_be32(data + 16);
    *out_height = read_be32(data + 20);
    return true;
}

[[nodiscard]]
static bool run_program(
    const char *const *argv,
    GBytes **out_stdout,
    int *out_exit_code,
    GError **error
) {
    g_autoptr(GError) local_err = nullptr;
    g_autoptr(GSubprocess) proc = g_subprocess_newv(
        argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        &local_err);
    if (proc == nullptr) {
        g_set_error(error, VERTERE_CAPTURE_ERROR, 1,
                    "cannot run `%s` — is it installed? (%s)",
                    argv[0], local_err->message);
        return false;
    }

    g_autoptr(GBytes) stdout_bytes = nullptr;
    g_autoptr(GBytes) stderr_bytes = nullptr;
    if (!g_subprocess_communicate(proc, nullptr, nullptr, &stdout_bytes, &stderr_bytes, error)) {
        return false;
    }

    bool exited = g_subprocess_get_if_exited(proc);
    int status = exited ? g_subprocess_get_exit_status(proc) : -1;
    if (out_exit_code != nullptr) {
        *out_exit_code = status;
    }

    if (exited && status == 0) {
        if (out_stdout != nullptr) {
            *out_stdout = g_steal_pointer(&stdout_bytes);
        }
        return true;
    }

    if (out_exit_code != nullptr) {
        /* Caller inspects non-zero exit code (e.g. slurp cancellation). */
        return true;
    }

    gsize err_len = 0;
    const char *err_raw = stderr_bytes != nullptr
        ? g_bytes_get_data(stderr_bytes, &err_len)
        : nullptr;
    g_autofree char *err_copy = err_raw != nullptr ? g_strndup(err_raw, err_len) : g_strdup("");
    g_autofree char *err_msg = vertere_trim_dup(err_copy);
    if (*err_msg == '\0') {
        g_set_error(error, VERTERE_CAPTURE_ERROR, 2,
                    "`%s` failed (status %d)", argv[0], status);
    } else {
        g_set_error(error, VERTERE_CAPTURE_ERROR, 2,
                    "`%s` failed (status %d): %s", argv[0], status, err_msg);
    }
    return false;
}

[[nodiscard]]
static GBytes *limit_png_size(GBytes *png, GError **error) {
    gsize len = 0;
    const uint8_t *data = g_bytes_get_data(png, &len);
    uint32_t width = 0;
    uint32_t height = 0;
    if (!vertere_png_dimensions(data, len, &width, &height)) {
        return g_bytes_ref(png);
    }

    uint32_t longest = MAX(width, height);
    if (longest <= MAX_IMAGE_DIMENSION) {
        return g_bytes_ref(png);
    }

    g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new_with_type("png", error);
    if (loader == nullptr) {
        return nullptr;
    }
    if (!gdk_pixbuf_loader_write(loader, data, len, error)) {
        return nullptr;
    }
    if (!gdk_pixbuf_loader_close(loader, error)) {
        return nullptr;
    }

    GdkPixbuf *orig = gdk_pixbuf_loader_get_pixbuf(loader);
    if (orig == nullptr) {
        g_set_error(error, VERTERE_CAPTURE_ERROR, 3, "cannot decode screenshot");
        return nullptr;
    }

    double scale = (double)MAX_IMAGE_DIMENSION / (double)longest;
    int dst_w = MAX((int)llround((double)width * scale), 1);
    int dst_h = MAX((int)llround((double)height * scale), 1);

    g_autoptr(GdkPixbuf) resized = gdk_pixbuf_scale_simple(
        orig, dst_w, dst_h, GDK_INTERP_BILINEAR);
    if (resized == nullptr) {
        g_set_error(error, VERTERE_CAPTURE_ERROR, 4, "cannot resize screenshot");
        return nullptr;
    }

    g_autofree char *out_buf = nullptr;
    gsize out_len = 0;
    if (!gdk_pixbuf_save_to_buffer(resized, &out_buf, &out_len, "png", error, nullptr)) {
        return nullptr;
    }
    return g_bytes_new_take(g_steal_pointer(&out_buf), out_len);
}

GBytes *vertere_capture_screenshot(bool *out_cancelled, GError **error) {
    *out_cancelled = false;

    const char *const slurp_argv[] = {"slurp", "-f", "%x,%y %wx%h", nullptr};
    g_autoptr(GBytes) slurp_out = nullptr;
    int slurp_status = 0;
    if (!run_program(slurp_argv, &slurp_out, &slurp_status, error)) {
        return nullptr;
    }
    if (slurp_status != 0) {
        *out_cancelled = true;
        return nullptr;
    }

    gsize region_len = 0;
    const char *region_raw = g_bytes_get_data(slurp_out, &region_len);
    if (!g_utf8_validate(region_raw, (gssize)region_len, nullptr)) {
        g_set_error(error, VERTERE_CAPTURE_ERROR, 5, "slurp printed invalid UTF-8");
        return nullptr;
    }
    g_autofree char *region_copy = g_strndup(region_raw, region_len);
    g_autofree char *region = vertere_trim_dup(region_copy);

    const char *const grim_argv[] = {"grim", "-g", region, "-", nullptr};
    g_autoptr(GBytes) grim_out = nullptr;
    if (!run_program(grim_argv, &grim_out, nullptr, error)) {
        return nullptr;
    }
    if (g_bytes_get_size(grim_out) == 0) {
        g_set_error(error, VERTERE_CAPTURE_ERROR, 6, "grim produced an empty image");
        return nullptr;
    }

    return limit_png_size(grim_out, error);
}

[[nodiscard]]
static char *paste_text(const char *const *argv, GError **error) {
    g_autoptr(GBytes) out = nullptr;
    if (!run_program(argv, &out, nullptr, error)) {
        return nullptr;
    }
    gsize len = 0;
    const char *raw = g_bytes_get_data(out, &len);
    if (len > 0 && !g_utf8_validate(raw, (gssize)len, nullptr)) {
        g_set_error(error, VERTERE_CAPTURE_ERROR, 7, "the text is not valid UTF-8");
        return nullptr;
    }
    return g_strndup(raw != nullptr ? raw : "", len);
}

char *vertere_capture_clipboard(GError **error) {
    const char *const argv[] = {"wl-paste", "--no-newline", "--type", "text", nullptr};
    return paste_text(argv, error);
}

char *vertere_capture_selection(GError **error) {
    const char *const argv[] = {"wl-paste", "--primary", "--no-newline", "--type", "text", nullptr};
    return paste_text(argv, error);
}
