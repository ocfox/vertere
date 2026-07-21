//! The application database: settings and translation history.

use std::fmt;
use std::path::Path;
use std::str::FromStr;
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, bail};
use rusqlite::{Connection, OptionalExtension, params};

/// Used when the corresponding field is left empty.
///
/// The settings view shows these as placeholder text, so an empty box and the
/// value it greys out mean the same thing.
pub const DEFAULT_TARGET_LANG: &str = "Simplified Chinese";
pub const DEFAULT_FALLBACK_LANG: &str = "English";
/// OpenRouter's own endpoint, used unless a compatible one is set instead.
pub const DEFAULT_BASE_URL: &str = "https://openrouter.ai/api/v1";

/// Everything the user can change, edited through the settings view.
///
/// The API key is deliberately absent: it stays in `API_KEY`, since a secret
/// does not belong in a database that also holds translated text.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Settings {
    /// Model slug, in the vendor's own naming. Must accept image input.
    pub model: String,
    /// Language to translate into, named the way you would say it to a person.
    ///
    /// This goes straight into the prompt, so it is prose rather than a code:
    /// `Simplified Chinese` says what `zh` leaves the model to guess.
    pub target_lang: String,
    /// Language to use when the source is already in `target_lang`. Empty to
    /// always translate into `target_lang`.
    pub fallback_lang: String,
    /// The OpenAI-compatible endpoint to send requests to. Empty for
    /// OpenRouter's own.
    pub base_url: String,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            model: String::new(),
            target_lang: String::new(),
            fallback_lang: String::new(),
            base_url: String::new(),
        }
    }
}

impl Settings {
    /// Whether these are complete enough to translate with.
    ///
    /// Only the model has to be filled in: it names something the endpoint
    /// serves that cannot be guessed, while everything else has a default.
    pub fn is_usable(&self) -> bool {
        !self.model.trim().is_empty()
    }

    /// The language to translate into, defaulted.
    pub fn target(&self) -> &str {
        non_empty(&self.target_lang).unwrap_or(DEFAULT_TARGET_LANG)
    }

    /// The language to use when the source is already in [`Self::target`].
    pub fn fallback(&self) -> &str {
        non_empty(&self.fallback_lang).unwrap_or(DEFAULT_FALLBACK_LANG)
    }

    /// The endpoint to send requests to, defaulted.
    pub fn base_url(&self) -> &str {
        non_empty(&self.base_url).unwrap_or(DEFAULT_BASE_URL)
    }
}

fn non_empty(value: &str) -> Option<&str> {
    Some(value.trim()).filter(|v| !v.is_empty())
}

/// How a translation was started.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Shot,
    Clip,
    Select,
}

impl fmt::Display for Kind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Shot => "shot",
            Self::Clip => "clip",
            Self::Select => "select",
        })
    }
}

impl FromStr for Kind {
    type Err = anyhow::Error;

    fn from_str(s: &str) -> Result<Self> {
        match s {
            "shot" => Ok(Self::Shot),
            "clip" => Ok(Self::Clip),
            "select" => Ok(Self::Select),
            other => bail!("unknown kind in history: {other}"),
        }
    }
}

/// A translation about to be recorded.
#[derive(Debug)]
pub struct Record<'a> {
    pub kind: Kind,
    pub model: &'a str,
    pub target: &'a str,
    pub source: &'a str,
    pub translated: &'a str,
}

/// A translation read back out.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    pub id: i64,
    /// Seconds since the Unix epoch.
    pub created_at: i64,
    pub kind: Kind,
    pub model: String,
    pub target: String,
    pub source: String,
    pub translated: String,
}

pub struct Store {
    db: Connection,
}

impl Store {
    /// Opens the history at `path`, creating it if needed.
    pub fn open(path: &Path) -> Result<Self> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)
                .with_context(|| format!("cannot create {}", parent.display()))?;
        }
        let db = Connection::open(path)
            .with_context(|| format!("cannot open history at {}", path.display()))?;
        Self::from_connection(db)
    }

    pub fn open_in_memory() -> Result<Self> {
        Self::from_connection(Connection::open_in_memory()?)
    }

    fn from_connection(db: Connection) -> Result<Self> {
        db.pragma_update(None, "foreign_keys", true)?;
        let history = Self { db };
        history.migrate()?;
        Ok(history)
    }

    fn migrate(&self) -> Result<()> {
        let version: i64 = self
            .db
            .query_row("PRAGMA user_version", [], |row| row.get(0))
            .context("cannot read the schema version")?;
        if version < 1 {
            self.db.execute_batch(
                "BEGIN;
                 -- Key-value rather than a column per setting, so adding one later
                 -- is a write instead of a migration.
                 CREATE TABLE setting (
                   key   TEXT PRIMARY KEY,
                   value TEXT NOT NULL
                 );
                 CREATE TABLE entry (
                   id         INTEGER PRIMARY KEY,
                   created_at INTEGER NOT NULL,
                   kind       TEXT NOT NULL,
                   model      TEXT NOT NULL,
                   target     TEXT NOT NULL,
                   source     TEXT NOT NULL,
                   translated TEXT NOT NULL,
                   image_path TEXT
                 );
                 CREATE INDEX entry_created_at ON entry (created_at DESC);
                 PRAGMA user_version = 1;
                 COMMIT;",
            )?;
        }
        if version < 2 {
            // The screenshot-caching feature this backed never shipped a way to
            // turn it on, so there is nothing to migrate the data into.
            self.db.execute_batch(
                "ALTER TABLE entry DROP COLUMN image_path;
                 PRAGMA user_version = 2;",
            )?;
        }
        Ok(())
    }

    /// Reads the settings, falling back to defaults for anything unset.
    ///
    /// Never fails on missing or malformed values: an unconfigured install
    /// should open the bubble and offer to fix itself, not refuse to start.
    pub fn settings(&self) -> Result<Settings> {
        let mut stmt = self.db.prepare("SELECT key, value FROM setting")?;
        let mut settings = Settings::default();
        let rows = stmt.query_map([], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
        })?;

        for row in rows {
            let (key, value) = row?;
            match key.as_str() {
                "model" => settings.model = value,
                "target_lang" => settings.target_lang = value,
                "fallback_lang" => settings.fallback_lang = value,
                "base_url" => settings.base_url = value,
                _ => {}
            }
        }
        Ok(settings)
    }

    pub fn save_settings(&mut self, settings: &Settings) -> Result<()> {
        let tx = self.db.transaction()?;
        for (key, value) in [
            ("model", settings.model.clone()),
            ("target_lang", settings.target_lang.clone()),
            ("fallback_lang", settings.fallback_lang.clone()),
            ("base_url", settings.base_url.clone()),
        ] {
            tx.execute(
                "INSERT INTO setting (key, value) VALUES (?1, ?2)
                 ON CONFLICT (key) DO UPDATE SET value = excluded.value",
                params![key, value],
            )?;
        }
        tx.commit()?;
        Ok(())
    }

    /// Records a translation and returns its id.
    pub fn add(&mut self, record: &Record<'_>) -> Result<i64> {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock is before the Unix epoch")
            .as_secs() as i64;

        self.db.execute(
            "INSERT INTO entry (created_at, kind, model, target, source, translated)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            params![
                now,
                record.kind.to_string(),
                record.model,
                record.target,
                record.source,
                record.translated,
            ],
        )?;
        Ok(self.db.last_insert_rowid())
    }

    /// The most recent entries, newest first.
    pub fn recent(&self, limit: usize) -> Result<Vec<Entry>> {
        self.query_entries("ORDER BY created_at DESC, id DESC LIMIT ?1", [limit])
    }

    /// Substring search over both the source and the translation, newest first.
    ///
    /// A plain scan rather than an index: FTS5 tokenises on word boundaries, so
    /// it cannot find `狐狸` inside `敏捷的狐狸`, and its trigram tokeniser needs
    /// queries of three characters or more — which rules out most CJK words. At
    /// the size a personal history reaches, scanning is instant anyway.
    pub fn search(&self, query: &str, limit: usize) -> Result<Vec<Entry>> {
        let query = query.trim();
        if query.is_empty() {
            return self.recent(limit);
        }
        self.query_entries(
            "WHERE source LIKE ?1 ESCAPE '\\' OR translated LIKE ?1 ESCAPE '\\'
             ORDER BY created_at DESC, id DESC LIMIT ?2",
            params![like_contains(query), limit],
        )
    }

    /// Runs `SELECT <entry columns> FROM entry <clause>` with `params` and
    /// collects the rows, sharing the column list and row-mapping between
    /// `recent` and `search`.
    fn query_entries(&self, clause: &str, params: impl rusqlite::Params) -> Result<Vec<Entry>> {
        let mut stmt = self.db.prepare(&format!(
            "SELECT id, created_at, kind, model, target, source, translated
             FROM entry {clause}"
        ))?;
        let entries = stmt
            .query_map(params, read_entry)?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        entries.into_iter().collect()
    }

    pub fn get(&self, id: i64) -> Result<Option<Entry>> {
        let entry = self
            .db
            .query_row(
                "SELECT id, created_at, kind, model, target, source, translated
                 FROM entry WHERE id = ?1",
                [id],
                read_entry,
            )
            .optional()?;
        entry.transpose()
    }

    pub fn delete(&mut self, id: i64) -> Result<()> {
        self.db.execute("DELETE FROM entry WHERE id = ?1", [id])?;
        Ok(())
    }
}

type Row = Result<Entry>;

fn read_entry(row: &rusqlite::Row<'_>) -> rusqlite::Result<Row> {
    let kind: String = row.get(2)?;
    Ok(Ok(Entry {
        id: row.get(0)?,
        created_at: row.get(1)?,
        kind: match kind.parse() {
            Ok(kind) => kind,
            Err(err) => return Ok(Err(err)),
        },
        model: row.get(3)?,
        target: row.get(4)?,
        source: row.get(5)?,
        translated: row.get(6)?,
    }))
}

/// Builds a `LIKE` pattern matching `query` anywhere.
///
/// The wildcards are escaped so that a literal `%` in the query means a percent
/// sign rather than "anything".
fn like_contains(query: &str) -> String {
    let mut pattern = String::with_capacity(query.len() + 2);
    pattern.push('%');
    for c in query.chars() {
        if matches!(c, '%' | '_' | '\\') {
            pattern.push('\\');
        }
        pattern.push(c);
    }
    pattern.push('%');
    pattern
}

#[cfg(test)]
mod tests {
    use super::*;

    fn record<'a>(source: &'a str, translated: &'a str) -> Record<'a> {
        Record {
            kind: Kind::Clip,
            model: "vendor/model",
            target: "zh-Hans",
            source,
            translated,
        }
    }

    #[test]
    fn stores_and_reads_back_an_entry() {
        let mut history = Store::open_in_memory().unwrap();
        let id = history.add(&record("Hello", "你好")).unwrap();

        let entry = history.get(id).unwrap().unwrap();
        assert_eq!(entry.id, id);
        assert_eq!(entry.kind, Kind::Clip);
        assert_eq!(entry.source, "Hello");
        assert_eq!(entry.translated, "你好");
        assert!(entry.created_at > 0);
    }

    #[test]
    fn returns_none_for_a_missing_entry() {
        let history = Store::open_in_memory().unwrap();
        assert!(history.get(404).unwrap().is_none());
    }

    #[test]
    fn lists_recent_entries_newest_first() {
        let mut history = Store::open_in_memory().unwrap();
        let first = history.add(&record("one", "一")).unwrap();
        let second = history.add(&record("two", "二")).unwrap();

        let ids: Vec<_> = history.recent(10).unwrap().iter().map(|e| e.id).collect();
        assert_eq!(ids, [second, first]);
    }

    #[test]
    fn honours_the_recent_limit() {
        let mut history = Store::open_in_memory().unwrap();
        for text in ["one", "two", "three"] {
            history.add(&record(text, "x")).unwrap();
        }
        assert_eq!(history.recent(2).unwrap().len(), 2);
    }

    #[test]
    fn searches_the_source_text() {
        let mut history = Store::open_in_memory().unwrap();
        history.add(&record("the quick fox", "敏捷的狐狸")).unwrap();
        history.add(&record("a slow turtle", "缓慢的乌龟")).unwrap();

        let found = history.search("fox", 10).unwrap();
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].source, "the quick fox");
    }

    #[test]
    fn searches_the_translation_too() {
        let mut history = Store::open_in_memory().unwrap();
        history.add(&record("the quick fox", "敏捷的狐狸")).unwrap();

        assert_eq!(history.search("狐狸", 10).unwrap().len(), 1);
    }

    #[test]
    fn an_empty_query_lists_recent_entries() {
        let mut history = Store::open_in_memory().unwrap();
        history.add(&record("one", "一")).unwrap();
        assert_eq!(history.search("   ", 10).unwrap().len(), 1);
    }

    #[test]
    fn finds_a_substring_inside_a_cjk_word() {
        let mut history = Store::open_in_memory().unwrap();
        history.add(&record("the quick fox", "敏捷的狐狸")).unwrap();

        assert_eq!(history.search("狐狸", 10).unwrap().len(), 1);
        assert_eq!(history.search("的狐", 10).unwrap().len(), 1);
    }

    #[test]
    fn treats_like_wildcards_as_ordinary_text() {
        let mut history = Store::open_in_memory().unwrap();
        history.add(&record("100% sure", "百分百")).unwrap();
        history.add(&record("nothing to see", "没什么")).unwrap();

        assert_eq!(history.search("100%", 10).unwrap().len(), 1);
        // A bare wildcard matches the literal percent sign only, not both rows.
        let found = history.search("%", 10).unwrap();
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].source, "100% sure");
        assert!(history.search("_", 10).unwrap().is_empty());
    }

    #[test]
    fn deleting_an_entry_removes_it_from_search() {
        let mut history = Store::open_in_memory().unwrap();
        let id = history.add(&record("the quick fox", "敏捷的狐狸")).unwrap();

        history.delete(id).unwrap();
        assert!(history.get(id).unwrap().is_none());
        assert!(history.search("fox", 10).unwrap().is_empty());
    }

    #[test]
    fn search_is_case_insensitive_for_ascii() {
        let mut history = Store::open_in_memory().unwrap();
        history.add(&record("The Quick Fox", "敏捷的狐狸")).unwrap();

        assert_eq!(history.search("quick", 10).unwrap().len(), 1);
    }

    #[test]
    fn reopening_keeps_the_data() {
        let dir = std::env::temp_dir().join(format!("vertere-test-{}", std::process::id()));
        let path = dir.join("history.db");
        let _ = std::fs::remove_dir_all(&dir);

        let mut history = Store::open(&path).unwrap();
        history.add(&record("Hello", "你好")).unwrap();
        drop(history);

        let history = Store::open(&path).unwrap();
        assert_eq!(history.recent(10).unwrap().len(), 1);

        std::fs::remove_dir_all(&dir).unwrap();
    }
}

#[cfg(test)]
mod setting_tests {
    use super::*;

    #[test]
    fn defaults_when_nothing_was_ever_saved() {
        let store = Store::open_in_memory().unwrap();
        let settings = store.settings().unwrap();
        assert_eq!(settings, Settings::default());
        assert!(!settings.is_usable());
    }

    #[test]
    fn round_trips_the_settings() {
        let mut store = Store::open_in_memory().unwrap();
        let settings = Settings {
            model: "vendor/model".into(),
            target_lang: "Simplified Chinese".into(),
            fallback_lang: "English".into(),
            base_url: "https://example.com/v1".into(),
        };
        store.save_settings(&settings).unwrap();
        assert_eq!(store.settings().unwrap(), settings);
    }

    #[test]
    fn saving_twice_overwrites_rather_than_duplicating() {
        let mut store = Store::open_in_memory().unwrap();
        let first = Settings {
            model: "first".into(),
            ..Settings::default()
        };
        store.save_settings(&first).unwrap();
        let second = Settings {
            model: "second".into(),
            ..first
        };
        store.save_settings(&second).unwrap();
        assert_eq!(store.settings().unwrap().model, "second");
    }

    #[test]
    fn only_the_model_has_to_be_filled_in() {
        let usable = |model: &str| {
            Settings {
                model: model.into(),
                ..Settings::default()
            }
            .is_usable()
        };
        assert!(!usable(""));
        assert!(!usable("   "));
        assert!(usable("vendor/model"));
    }

    #[test]
    fn empty_languages_fall_back_to_the_defaults() {
        let settings = Settings::default();
        assert_eq!(settings.target(), DEFAULT_TARGET_LANG);
        assert_eq!(settings.fallback(), DEFAULT_FALLBACK_LANG);
    }

    #[test]
    fn an_empty_base_url_falls_back_to_openrouter() {
        let settings = Settings::default();
        assert_eq!(settings.base_url(), DEFAULT_BASE_URL);

        let settings = Settings {
            base_url: "https://example.com/v1".into(),
            ..Settings::default()
        };
        assert_eq!(settings.base_url(), "https://example.com/v1");
    }

    #[test]
    fn a_language_of_only_whitespace_counts_as_empty() {
        let settings = Settings {
            target_lang: "   ".into(),
            fallback_lang: "\n".into(),
            ..Settings::default()
        };
        assert_eq!(settings.target(), DEFAULT_TARGET_LANG);
        assert_eq!(settings.fallback(), DEFAULT_FALLBACK_LANG);
    }

    #[test]
    fn a_set_language_wins_over_the_default() {
        let settings = Settings {
            target_lang: " Japanese ".into(),
            fallback_lang: "German".into(),
            ..Settings::default()
        };
        assert_eq!(settings.target(), "Japanese");
        assert_eq!(settings.fallback(), "German");
    }
}
