//! Screen and clipboard capture, via `slurp`, `grim` and `wl-paste`.
//!
//! These are the only places that shell out. Everything downstream works on
//! plain bytes and strings, so nothing else needs to know where they came from.

use std::io::Cursor;
use std::process::{Command, Output};

use anyhow::{Context, Result, bail};
use gdk_pixbuf::{InterpType, Pixbuf};

const MAX_IMAGE_DIMENSION: u32 = 2048;

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
    Ok(Some(limit_png_size(grim.stdout)?))
}

/// Shrinks the image if either side is over [`MAX_IMAGE_DIMENSION`], so a
/// full-screen grab on a scaled output does not become a multi-megabyte upload.
fn limit_png_size(png: Vec<u8>) -> Result<Vec<u8>> {
    // Sizing off the header rather than a decode: a capture that already fits —
    // the usual case — should not pay to be unpacked into a pixbuf and copied.
    let Some((width, height)) = png_dimensions(&png) else {
        return Ok(png);
    };
    let longest = width.max(height);
    if longest <= MAX_IMAGE_DIMENSION {
        return Ok(png);
    }

    let scale = f64::from(MAX_IMAGE_DIMENSION) / f64::from(longest);
    // At an extreme aspect ratio the short side rounds to zero, which no image
    // format accepts.
    let scaled = |side: u32| ((f64::from(side) * scale).round() as i32).max(1);
    let resized = Pixbuf::from_read(Cursor::new(png))
        .context("cannot decode screenshot")?
        .scale_simple(scaled(width), scaled(height), InterpType::Bilinear)
        .context("cannot resize screenshot")?;
    resized
        .save_to_bufferv("png", &[])
        .context("cannot encode resized screenshot")
}

/// Reads the width and height out of a PNG's IHDR chunk, which the format puts
/// first and at a fixed offset.
///
/// `None` means the bytes are not a PNG we recognise; callers pass them through
/// rather than fail, since a capture is worth more than a size check.
fn png_dimensions(png: &[u8]) -> Option<(u32, u32)> {
    const SIGNATURE: &[u8] = b"\x89PNG\r\n\x1a\n";

    if !png.starts_with(SIGNATURE) || png.get(12..16)? != b"IHDR" {
        return None;
    }
    let ihdr = png.get(16..24)?;
    let width = u32::from_be_bytes(ihdr[..4].try_into().ok()?);
    let height = u32::from_be_bytes(ihdr[4..].try_into().ok()?);
    Some((width, height))
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

#[cfg(test)]
mod tests {
    use super::png_dimensions;

    #[test]
    fn reads_dimensions_from_a_png_header() {
        let mut png = b"\x89PNG\r\n\x1a\n".to_vec();
        png.extend(13u32.to_be_bytes());
        png.extend(b"IHDR");
        png.extend(3840u32.to_be_bytes());
        png.extend(2160u32.to_be_bytes());

        assert_eq!(png_dimensions(&png), Some((3840, 2160)));
        assert_eq!(png_dimensions(&png[..20]), None);
        assert_eq!(png_dimensions(b"not a png"), None);
    }
}
