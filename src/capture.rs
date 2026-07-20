//! Screen and clipboard capture, via `slurp`, `grim` and `wl-paste`.
//!
//! These are the only places that shell out. Everything downstream works on
//! plain bytes and strings, so nothing else needs to know where they came from.

use std::process::{Command, Output};

use anyhow::{Context, Result, bail};

/// Lets the user drag a region, then captures it as PNG bytes.
///
/// Returns `None` when the selection was cancelled (Esc or right click), which
/// is a normal outcome rather than an error.
pub fn screenshot() -> Result<Option<Vec<u8>>> {
    let slurp = run("slurp", &["-f", "%x,%y %wx%h"])?;
    if !slurp.status.success() {
        return Ok(None);
    }
    let region = String::from_utf8(slurp.stdout)
        .context("slurp printed invalid UTF-8")?
        .trim()
        .to_owned();

    // The region is in logical coordinates but the image comes back in physical
    // pixels, so on a scaled output it is larger than the numbers above suggest.
    let grim = run("grim", &["-g", &region, "-"])?;
    check(&grim, "grim")?;
    if grim.stdout.is_empty() {
        bail!("grim produced an empty image");
    }
    Ok(Some(grim.stdout))
}

/// Reads the clipboard as text.
pub fn clipboard() -> Result<String> {
    paste(&["--no-newline", "--type", "text"])
}

/// Reads the primary selection as text.
///
/// Merely selecting text fills this, so translating a selection needs no copy
/// step. Not every application publishes one — GTK, Qt and terminals do.
pub fn selection() -> Result<String> {
    paste(&["--primary", "--no-newline", "--type", "text"])
}

fn paste(args: &[&str]) -> Result<String> {
    let output = run("wl-paste", args)?;
    check(&output, "wl-paste")?;
    String::from_utf8(output.stdout).context("the text is not valid UTF-8")
}

fn run(program: &str, args: &[&str]) -> Result<Output> {
    Command::new(program)
        .args(args)
        .output()
        .with_context(|| format!("cannot run `{program}` — is it installed?"))
}

fn check(output: &Output, program: &str) -> Result<()> {
    if output.status.success() {
        return Ok(());
    }
    let stderr = String::from_utf8_lossy(&output.stderr);
    let stderr = stderr.trim();
    if stderr.is_empty() {
        bail!("`{program}` failed ({})", output.status);
    }
    bail!("`{program}` failed ({}): {stderr}", output.status)
}
