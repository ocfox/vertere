#include "app.h"
#include "vertere.h"

#include <gtk/gtk.h>

static constexpr char USAGE[] =
    "vertere \xe2\x80\x94 translate a screen region, the clipboard or the selection\n"
    "\n"
    "usage:\n"
    "    vertere daemon                   run in the background\n"
    "    vertere shot                     capture a screen region and translate it\n"
    "    vertere clip                     translate the clipboard contents\n"
    "    vertere select                   translate the selected text, no copy needed\n"
    "    vertere settings                 change the model and languages\n"
    "    vertere history                  browse and search past translations\n"
    "\n"
    "Commands are handed to a running daemon. Without one they do the work\n"
    "themselves and exit.\n";

static int dispatch(
    VertereApp *app,
    GApplicationCommandLine *cmdline,
    int argc,
    char **argv
) {
    if (argc < 2) {
        g_application_command_line_printerr(cmdline, "vertere: missing command\n\n%s", USAGE);
        return 1;
    }

    const char *cmd = argv[1];
    if (argc == 2 && (g_strcmp0(cmd, "-h") == 0 || g_strcmp0(cmd, "--help") == 0)) {
        g_application_command_line_print(cmdline, "%s", USAGE);
        return 0;
    }
    if (argc == 2 && g_strcmp0(cmd, "daemon") == 0) {
        g_autoptr(GError) err = nullptr;
        if (!vertere_app_start_daemon(app, &err)) {
            g_application_command_line_printerr(cmdline, "vertere: %s\n", err->message);
            return 1;
        }
        return 0;
    }
    if (argc == 2 && g_strcmp0(cmd, "settings") == 0) {
        vertere_app_settings_window(app);
        return 0;
    }
    if (argc == 2 && g_strcmp0(cmd, "history") == 0) {
        vertere_app_history_window(app);
        return 0;
    }
    if (argc == 2 && g_strcmp0(cmd, "shot") == 0) {
        vertere_app_shot(app);
        return 0;
    }
    if (argc == 2 && g_strcmp0(cmd, "clip") == 0) {
        vertere_app_clip(app);
        return 0;
    }
    if (argc == 2 && g_strcmp0(cmd, "select") == 0) {
        vertere_app_select(app);
        return 0;
    }

    g_application_command_line_printerr(
        cmdline, "vertere: unknown or malformed command `%s`\n\n%s", cmd, USAGE);
    return 1;
}

static int on_command_line(
    GApplication *,
    GApplicationCommandLine *cmdline,
    gpointer user_data
) {
    VertereApp *app = user_data;
    int argc = 0;
    g_auto(GStrv) argv = g_application_command_line_get_arguments(cmdline, &argc);
    return dispatch(app, cmdline, argc, argv);
}

#include <stdio.h>

int main(int argc, char **argv) {
    if (argc == 2 && (g_strcmp0(argv[1], "-h") == 0 || g_strcmp0(argv[1], "--help") == 0)) {
        fputs(USAGE, stdout);
        return 0;
    }

    g_autoptr(GtkApplication) gtk_app = gtk_application_new(
        VERTERE_APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_autoptr(VertereApp) app = vertere_app_new(gtk_app);

    g_signal_connect(gtk_app, "command-line", G_CALLBACK(on_command_line), app);
    return g_application_run(G_APPLICATION(gtk_app), argc, argv);
}
