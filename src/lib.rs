/// The D-Bus name this program owns, which is also the basename of its desktop
/// entry, D-Bus activation file and icon. Changing it means changing all four.
///
/// Reverse-DNS of a domain we control, as the D-Bus namespace is shared by
/// everything on the session bus.
pub const APP_ID: &str = "me.ocfox.Vertere";

/// The layer-shell namespace, which is what sway matches window rules against.
///
/// Deliberately not [`APP_ID`]: this one gets typed into config files by hand.
pub const LAYER_NAMESPACE: &str = "vertere";

pub mod app;
pub mod capture;
pub mod images;
pub mod provider;
pub mod store;
pub mod translate;
pub mod tray;
pub mod ui;
pub mod window_state;
pub mod xdg;
