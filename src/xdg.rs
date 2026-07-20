//! XDG base directory lookup.
//!
//! Small enough that a dependency would cost more than it saves.

use std::env;
use std::path::PathBuf;

use anyhow::{Context, Result};

fn base(var: &str, fallback: &str) -> Result<PathBuf> {
    if let Some(dir) = env::var_os(var)
        && !dir.is_empty()
    {
        return Ok(PathBuf::from(dir));
    }
    let home = env::var_os("HOME").context("neither $HOME nor $XDG_* directories are set")?;
    Ok(PathBuf::from(home).join(fallback))
}

/// `$XDG_CONFIG_HOME/vertere`, default `~/.config/vertere`.
pub fn config_dir() -> Result<PathBuf> {
    Ok(base("XDG_CONFIG_HOME", ".config")?.join("vertere"))
}

/// `$XDG_DATA_HOME/vertere`, default `~/.local/share/vertere`.
pub fn data_dir() -> Result<PathBuf> {
    Ok(base("XDG_DATA_HOME", ".local/share")?.join("vertere"))
}

/// `$XDG_STATE_HOME/vertere`, default `~/.local/state/vertere`.
pub fn state_dir() -> Result<PathBuf> {
    Ok(base("XDG_STATE_HOME", ".local/state")?.join("vertere"))
}

/// `$XDG_CACHE_HOME/vertere`, default `~/.cache/vertere`.
pub fn cache_dir() -> Result<PathBuf> {
    Ok(base("XDG_CACHE_HOME", ".cache")?.join("vertere"))
}

/// `$XDG_RUNTIME_DIR/vertere.sock`. Falls back to `/tmp` when the compositor
/// did not provide a runtime dir.
pub fn socket_path() -> PathBuf {
    let dir = env::var_os("XDG_RUNTIME_DIR").map_or_else(|| PathBuf::from("/tmp"), PathBuf::from);
    dir.join("vertere.sock")
}
