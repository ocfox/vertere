//! The status-notifier item.
//!
//! The tray runs on its own D-Bus connection and calls back from another thread,
//! so it does not touch the widgets directly: menu items send an [`Action`] down
//! a channel that the GLib main loop drains.
//!
//! It is an extra way in, never the only one. A status-notifier item is invisible
//! without a host to draw it — on sway that means something like waybar with its
//! tray module — so every action here is also a subcommand.

use anyhow::{Context, Result};
use async_channel::Sender;
use ksni::menu::StandardItem;
use ksni::{MenuItem, Tray, TrayMethods};

use crate::APP_ID;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Action {
    Shot,
    Clip,
    Selection,
    History,
    Settings,
    Quit,
}

struct Menu {
    actions: Sender<Action>,
}

impl Menu {
    fn item(&self, label: &str, action: Action) -> MenuItem<Self> {
        let actions = self.actions.clone();
        StandardItem {
            label: label.into(),
            activate: Box::new(move |_: &mut Self| {
                // The receiver only closes when the application is going away,
                // and there is nothing useful to do about it here.
                let _ = actions.send_blocking(action);
            }),
            ..Default::default()
        }
        .into()
    }
}

impl Tray for Menu {
    fn id(&self) -> String {
        APP_ID.into()
    }

    fn title(&self) -> String {
        "Vertere".into()
    }

    fn icon_name(&self) -> String {
        APP_ID.into()
    }

    // Left-clicking a tray icon has no obvious single meaning here, so it opens
    // the menu rather than guessing at one of six actions.
    const MENU_ON_ACTIVATE: bool = true;

    fn menu(&self) -> Vec<MenuItem<Self>> {
        vec![
            self.item("Translate a screen region", Action::Shot),
            self.item("Translate the clipboard", Action::Clip),
            self.item("Translate the selection", Action::Selection),
            MenuItem::Separator,
            self.item("History…", Action::History),
            self.item("Settings…", Action::Settings),
            MenuItem::Separator,
            self.item("Quit", Action::Quit),
        ]
    }
}

/// Publishes the tray item, sending menu choices to `actions`.
pub async fn spawn(actions: Sender<Action>) -> Result<()> {
    Menu { actions }
        .spawn()
        .await
        .context("cannot publish the tray item")?;
    Ok(())
}
