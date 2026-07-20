//! Optional on-disk copies of captured screenshots.
//!
//! Entries reference an image by relative path. Pruning only deletes files and
//! never touches the history, so a missing file simply means "no image" — that
//! keeps the cache and the database from having to stay in step.

use std::fs;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result};

/// Writes `png` into `dir` and returns its path relative to `dir`.
pub fn save(dir: &Path, png: &[u8]) -> Result<String> {
    fs::create_dir_all(dir).with_context(|| format!("cannot create {}", dir.display()))?;

    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    let name = format!("{}-{:09}.png", now.as_secs(), now.subsec_nanos());
    let path = dir.join(&name);
    fs::write(&path, png).with_context(|| format!("cannot write {}", path.display()))?;
    Ok(name)
}

/// Deletes the oldest images until the directory fits within `limit_bytes`.
pub fn prune(dir: &Path, limit_bytes: u64) -> Result<()> {
    let mut images = Vec::new();
    let mut total = 0;

    let entries = match fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(err) => return Err(err).with_context(|| format!("cannot read {}", dir.display())),
    };

    for entry in entries {
        let entry = entry?;
        let meta = entry.metadata()?;
        if !meta.is_file() {
            continue;
        }
        total += meta.len();
        images.push((meta.modified()?, meta.len(), entry.path()));
    }

    if total <= limit_bytes {
        return Ok(());
    }

    images.sort_by_key(|(modified, _, _)| *modified);
    for (_, size, path) in images {
        if total <= limit_bytes {
            break;
        }
        fs::remove_file(&path).with_context(|| format!("cannot delete {}", path.display()))?;
        total -= size;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    struct TempDir(PathBuf);

    impl TempDir {
        fn new(tag: &str) -> Self {
            let path = std::env::temp_dir().join(format!(
                "vertere-images-{tag}-{}-{:?}",
                std::process::id(),
                std::thread::current().id()
            ));
            let _ = fs::remove_dir_all(&path);
            Self(path)
        }
    }

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn set_age(path: &Path, seconds_ago: u64) {
        let when = SystemTime::now() - std::time::Duration::from_secs(seconds_ago);
        filetime::set_file_mtime(path, filetime::FileTime::from_system_time(when)).unwrap();
    }

    #[test]
    fn saves_an_image() {
        let dir = TempDir::new("save");
        let name = save(&dir.0, b"fake png").unwrap();

        assert_eq!(fs::read(dir.0.join(&name)).unwrap(), b"fake png");
    }

    #[test]
    fn pruning_an_absent_directory_is_not_an_error() {
        let dir = TempDir::new("absent");
        assert!(prune(&dir.0, 1024).is_ok());
    }

    #[test]
    fn keeps_everything_while_under_the_limit() {
        let dir = TempDir::new("under");
        let name = save(&dir.0, &[0u8; 100]).unwrap();

        prune(&dir.0, 1024).unwrap();
        assert!(dir.0.join(&name).is_file());
    }

    #[test]
    fn deletes_the_oldest_first() {
        let dir = TempDir::new("oldest");
        let old = save(&dir.0, &[0u8; 100]).unwrap();
        let new = save(&dir.0, &[0u8; 100]).unwrap();
        set_age(&dir.0.join(&old), 60);

        prune(&dir.0, 150).unwrap();

        assert!(!dir.0.join(&old).is_file());
        assert!(dir.0.join(&new).is_file());
    }
}
