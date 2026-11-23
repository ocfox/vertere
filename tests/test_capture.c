#include "capture.h"

#include <glib.h>
#include <stdint.h>
#include <string.h>

static void write_be32(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)((v >> 24) & 0xff);
    dst[1] = (uint8_t)((v >> 16) & 0xff);
    dst[2] = (uint8_t)((v >> 8) & 0xff);
    dst[3] = (uint8_t)(v & 0xff);
}

static void test_reads_dimensions_from_png_header(void) {
    uint8_t png[24] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    write_be32(png + 8, 13);
    memcpy(png + 12, "IHDR", 4);
    write_be32(png + 16, 3840);
    write_be32(png + 20, 2160);

    uint32_t w = 0;
    uint32_t h = 0;
    g_assert_true(vertere_png_dimensions(png, sizeof(png), &w, &h));
    g_assert_cmpuint(w, ==, 3840);
    g_assert_cmpuint(h, ==, 2160);

    g_assert_false(vertere_png_dimensions(png, 20, &w, &h));
    const uint8_t bad[] = "not a png";
    g_assert_false(vertere_png_dimensions(bad, sizeof(bad), &w, &h));
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/capture/png_dimensions", test_reads_dimensions_from_png_header);
    return g_test_run();
}
