use anyhow::{Result, bail};
use gtk4::gio;
use gtk4::gio::prelude::*;
use vertere::{APP_ID, app};

const USAGE: &str = "\
vertere — translate a screen region, the clipboard or the selection

usage:
    vertere daemon                   run in the background
    vertere shot                     capture a screen region and translate it
    vertere clip                     translate the clipboard contents
    vertere sel                      translate the selected text, no copy needed
    vertere settings                 change the model and languages
    vertere history                  browse and search past translations

Commands are handed to a running daemon. Without one they do the work
themselves and exit.
";

fn main() -> std::process::ExitCode {
    // HANDLES_COMMAND_LINE makes GIO forward the arguments to the running
    // instance over D-Bus, which is the whole of our IPC: no socket, no wire
    // format, no reconnect logic.
    let application = gtk4::Application::builder()
        .application_id(APP_ID)
        .flags(gio::ApplicationFlags::HANDLES_COMMAND_LINE)
        .build();

    application.connect_command_line(|application, command_line| {
        let arguments = command_line
            .arguments()
            .into_iter()
            .skip(1)
            .map(|a| a.to_string_lossy().into_owned())
            .collect::<Vec<_>>();

        match dispatch(application, &arguments) {
            Ok(()) => gio::glib::ExitCode::SUCCESS,
            Err(err) => {
                // Printed through the command line so it reaches the terminal
                // that invoked us, not the daemon's own stderr.
                command_line.printerr_literal(&format!("vertere: {err:#}\n"));
                gio::glib::ExitCode::FAILURE
            }
        }
    });

    // The real argv, not an empty one: under HANDLES_COMMAND_LINE these are what
    // GIO forwards to the running instance and hands to `command_line`.
    let status = application.run();
    if status == gio::glib::ExitCode::SUCCESS {
        std::process::ExitCode::SUCCESS
    } else {
        std::process::ExitCode::FAILURE
    }
}

fn dispatch(application: &gtk4::Application, arguments: &[String]) -> Result<()> {
    match arguments
        .iter()
        .map(String::as_str)
        .collect::<Vec<_>>()
        .as_slice()
    {
        ["daemon"] => app::start_daemon(application),
        ["settings"] => {
            app::settings_window(application);
            Ok(())
        }
        ["history"] => {
            app::history_window(application);
            Ok(())
        }
        ["shot"] => {
            app::shot(application);
            Ok(())
        }
        ["clip"] => {
            app::clip(application);
            Ok(())
        }
        ["sel"] => {
            app::sel(application);
            Ok(())
        }
        ["-h"] | ["--help"] => {
            print!("{USAGE}");
            Ok(())
        }
        [] => bail!("missing command\n\n{USAGE}"),
        [command, ..] => bail!("unknown or malformed command `{command}`\n\n{USAGE}"),
    }
}
