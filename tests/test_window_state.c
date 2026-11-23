#include "window_state.h"

#include <glib.h>

static constexpr VertereScreen SCREEN = {.width = 2560, .height = 1440};
static constexpr VertereScreen SIZE = {.width = 420, .height = 200};

static void test_defaults_to_right_edge_vertically_centred(void) {
    int x = 0;
    int y = 0;
    vertere_window_place(nullptr, "DP-1", SCREEN, SIZE, &x, &y);
    g_assert_cmpint(x, ==, 2560 - 420 - 24);
    g_assert_cmpint(y, ==, (1440 - 200) / 2);
}

static void test_restores_stored_position(void) {
    VerterePosition p = {.output = "DP-1", .x = 100, .y = 200};
    int x = 0;
    int y = 0;
    vertere_window_place(&p, "DP-1", SCREEN, SIZE, &x, &y);
    g_assert_cmpint(x, ==, 100);
    g_assert_cmpint(y, ==, 200);
}

static void test_ignores_other_output(void) {
    VerterePosition p = {.output = "HDMI-A-1", .x = 100, .y = 200};
    int x = 0;
    int y = 0;
    vertere_window_place(&p, "DP-1", SCREEN, SIZE, &x, &y);
    g_assert_cmpint(x, ==, 2560 - 420 - 24);
}

static void test_clamps_offscreen_and_negative(void) {
    VerterePosition off = {.output = "DP-1", .x = 3000, .y = 1300};
    int x = 0;
    int y = 0;
    vertere_window_place(&off, "DP-1", SCREEN, SIZE, &x, &y);
    g_assert_cmpint(x, ==, 2560 - 420 - 24);
    g_assert_cmpint(y, ==, 1440 - 200 - 24);

    VerterePosition neg = {.output = "DP-1", .x = -500, .y = -500};
    vertere_window_place(&neg, "DP-1", SCREEN, SIZE, &x, &y);
    g_assert_cmpint(x, ==, 24);
    g_assert_cmpint(y, ==, 24);
}

static void test_survives_tiny_screen(void) {
    VertereScreen tiny = {.width = 300, .height = 150};
    int x = -1;
    int y = -1;
    vertere_window_place(nullptr, "DP-1", tiny, SIZE, &x, &y);
    g_assert_cmpint(x, >=, 0);
    g_assert_cmpint(y, >=, 0);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/window_state/defaults", test_defaults_to_right_edge_vertically_centred);
    g_test_add_func("/window_state/restores", test_restores_stored_position);
    g_test_add_func("/window_state/other_output", test_ignores_other_output);
    g_test_add_func("/window_state/clamps", test_clamps_offscreen_and_negative);
    g_test_add_func("/window_state/tiny_screen", test_survives_tiny_screen);
    return g_test_run();
}
