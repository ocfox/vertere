//! Where the window was last left.

use std::fs;
use std::path::PathBuf;

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};

/// A window position in **logical** coordinates, tied to the output it was on.
///
/// Outputs come and go and their resolution or scale can change, so a position
/// is a hint to be validated on restore rather than a promise.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Position {
    /// Connector name, e.g. `DP-1`.
    pub output: String,
    pub x: i32,
    pub y: i32,
}

fn path() -> Result<PathBuf> {
    Ok(crate::xdg::state_dir()?.join("window.json"))
}

/// Reads the stored position, or `None` if there is nothing usable.
///
/// A corrupt or unreadable file is not worth failing a translation over.
pub fn load() -> Option<Position> {
    let text = fs::read_to_string(path().ok()?).ok()?;
    serde_json::from_str(&text).ok()
}

pub fn save(position: &Position) -> Result<()> {
    let path = path()?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("cannot create {}", parent.display()))?;
    }
    let text = serde_json::to_string(position)?;
    fs::write(&path, text).with_context(|| format!("cannot write {}", path.display()))
}

/// The size of an output, in logical pixels.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Screen {
    pub width: i32,
    pub height: i32,
}

/// Places a window of `size` on `screen`, honouring `stored` when it still fits.
///
/// Falls back to the right edge, vertically centred: a fixed spot the muscle
/// memory can rely on, and out of the way of most content.
pub fn place(stored: Option<&Position>, output: &str, screen: Screen, size: Screen) -> (i32, i32) {
    let margin = 24;
    let max_x = (screen.width - size.width - margin).max(margin);
    let max_y = (screen.height - size.height - margin).max(margin);

    match stored {
        Some(p) if p.output == output => (p.x.clamp(margin, max_x), p.y.clamp(margin, max_y)),
        _ => (max_x, ((screen.height - size.height) / 2).max(margin)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SCREEN: Screen = Screen {
        width: 2560,
        height: 1440,
    };
    const SIZE: Screen = Screen {
        width: 420,
        height: 200,
    };

    fn stored(output: &str, x: i32, y: i32) -> Position {
        Position {
            output: output.to_owned(),
            x,
            y,
        }
    }

    #[test]
    fn defaults_to_the_right_edge_vertically_centred() {
        let (x, y) = place(None, "DP-1", SCREEN, SIZE);
        assert_eq!(x, 2560 - 420 - 24);
        assert_eq!(y, (1440 - 200) / 2);
    }

    #[test]
    fn restores_a_stored_position() {
        let p = stored("DP-1", 100, 200);
        assert_eq!(place(Some(&p), "DP-1", SCREEN, SIZE), (100, 200));
    }

    #[test]
    fn ignores_a_position_from_another_output() {
        let p = stored("HDMI-A-1", 100, 200);
        let (x, _) = place(Some(&p), "DP-1", SCREEN, SIZE);
        assert_eq!(x, 2560 - 420 - 24, "should fall back to the default");
    }

    #[test]
    fn pulls_an_offscreen_position_back_into_view() {
        // Saved on a wider screen, restored on a narrower one.
        let p = stored("DP-1", 3000, 1300);
        let (x, y) = place(Some(&p), "DP-1", SCREEN, SIZE);
        assert_eq!(x, 2560 - 420 - 24);
        assert_eq!(y, 1440 - 200 - 24);
    }

    #[test]
    fn keeps_a_negative_position_on_screen() {
        let p = stored("DP-1", -500, -500);
        assert_eq!(place(Some(&p), "DP-1", SCREEN, SIZE), (24, 24));
    }

    #[test]
    fn survives_a_screen_smaller_than_the_window() {
        let tiny = Screen {
            width: 300,
            height: 150,
        };
        let (x, y) = place(None, "DP-1", tiny, SIZE);
        assert!(x >= 0 && y >= 0);
    }
}
