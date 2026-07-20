//! Wiring the commands to the window.

use std::cell::RefCell;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, OnceLock};

use anyhow::{Context, Result, anyhow};
use futures_util::{Stream, StreamExt};
use gtk4::prelude::*;

use crate::provider::Provider;
use crate::store::{Kind, Record, Settings, Store};
use crate::translate::Reply;
use crate::{capture, images, tray, ui, xdg};

/// A provider built for one set of settings, reused until they change.
struct Session {
    settings: Settings,
    provider: Provider,
}

/// Rebuilt rather than mutated: the provider bakes in the model and the prompt,
/// so a settings change has to produce a new one.
static SESSION: Mutex<Option<Arc<Session>>> = Mutex::new(None);

thread_local! {
    static HOLD: RefCell<Option<gtk4::gio::ApplicationHoldGuard>> = const { RefCell::new(None) };
}

/// The provider client lives on tokio while the window lives on the GLib main
/// loop, so the two are bridged by a channel rather than sharing a runtime.
fn runtime() -> &'static tokio::runtime::Runtime {
    static RUNTIME: OnceLock<tokio::runtime::Runtime> = OnceLock::new();
    RUNTIME.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .worker_threads(1)
            .enable_all()
            .build()
            .expect("cannot start the tokio runtime")
    })
}

fn database() -> Result<PathBuf> {
    Ok(xdg::data_dir()?.join("vertere.db"))
}

fn open_store() -> Result<Store> {
    Store::open(&database()?).context("cannot open the database")
}

pub fn settings() -> Result<Settings> {
    open_store()?.settings()
}

pub fn save_settings(settings: &Settings) -> Result<()> {
    open_store()?.save_settings(settings)?;
    *SESSION.lock().expect("settings lock poisoned") = None;
    Ok(())
}

fn session(settings: Settings) -> Result<Arc<Session>> {
    let mut cached = SESSION.lock().expect("settings lock poisoned");
    if let Some(session) = cached.as_ref()
        && session.settings == settings
    {
        return Ok(Arc::clone(session));
    }

    let provider = Provider::new(&settings, &api_key()?)?;
    let session = Arc::new(Session { settings, provider });
    *cached = Some(Arc::clone(&session));
    Ok(session)
}

/// Environment variable holding the OpenRouter API key.
///
/// Kept out of the database: a secret does not belong next to translated text,
/// and this is the one value a service manager can supply without a UI.
pub const API_KEY_ENV: &str = "OPENROUTER_API_KEY";

fn api_key() -> Result<String> {
    let key = std::env::var(API_KEY_ENV).unwrap_or_default();
    let key = key.trim();
    if key.is_empty() {
        anyhow::bail!("${API_KEY_ENV} is not set")
    } else {
        Ok(key.to_owned())
    }
}

/// Becomes the instance that later commands are handed to.
pub fn start_daemon(application: &gtk4::Application) -> Result<()> {
    // The guard is what keeps the application alive between bubbles; dropping it
    // would let the daemon exit as soon as the last window closes.
    HOLD.with(|hold| *hold.borrow_mut() = Some(application.hold()));
    start_tray(application);

    // Warm the provider so the first translation is not also a cold start, but
    // do not refuse to run: the settings view is how an unconfigured install
    // gets configured.
    if let Ok(settings) = settings()
        && settings.is_usable()
        && let Ok(session) = session(settings)
    {
        let _ = runtime().block_on(session.provider.check_model());
    }
    Ok(())
}

pub fn shot(application: &gtk4::Application) {
    match capture::screenshot() {
        // Cancelling the selection is a decision, not a failure.
        Ok(None) => {}
        Ok(Some(png)) => run(application, Kind::Shot, Input::Image(png)),
        Err(err) => fail(application, err),
    }
}

pub fn clip(application: &gtk4::Application) {
    text(application, capture::clipboard(), "the clipboard is empty");
}

pub fn sel(application: &gtk4::Application) {
    text(application, capture::selection(), "nothing is selected");
}

pub fn settings_window(application: &gtk4::Application) {
    ui::show_settings(application, settings().unwrap_or_default());
}

pub fn history_window(application: &gtk4::Application) {
    // 200 rows is more than anyone scrolls; the search box is the way to reach
    // anything older.
    ui::show_history(application, |query| open_store()?.search(query, 200));
}

fn text(application: &gtk4::Application, read: Result<String>, when_empty: &str) {
    match read {
        Ok(text) if text.trim().is_empty() => fail(application, anyhow!("{when_empty}")),
        Ok(text) => run(application, Kind::Clip, Input::Text(text)),
        Err(err) => fail(application, err),
    }
}

enum Input {
    Image(Vec<u8>),
    Text(String),
}

fn run(application: &gtk4::Application, kind: Kind, input: Input) {
    let settings = match settings() {
        Ok(settings) => settings,
        Err(err) => return fail(application, err),
    };
    // Nothing to translate with yet, so go straight to the place that fixes it.
    if !settings.is_usable() {
        return ui::show_settings(application, settings);
    }

    let session = match session(settings) {
        Ok(session) => session,
        Err(err) => return fail(application, err),
    };

    let png = match &input {
        Input::Image(png) => Some(png.clone()),
        Input::Text(_) => None,
    };
    let deltas = translate(Arc::clone(&session), input);

    ui::show(application, deltas, move |reply| {
        report(record(&session, kind, &reply, png.as_deref()));
    });
}

/// Reports a failure in the bubble rather than on stderr.
///
/// These commands are bound to keys, so there is usually no terminal watching:
/// an error nobody sees is the same as no error at all.
fn fail(application: &gtk4::Application, err: anyhow::Error) {
    let deltas = futures_util::stream::once(async move { Err(err) }).boxed();
    ui::show(application, deltas, |_| {});
}

fn translate(session: Arc<Session>, input: Input) -> impl Stream<Item = Result<String>> {
    let (sender, receiver) = async_channel::unbounded();

    runtime().spawn(async move {
        let stream = match &input {
            Input::Image(png) => session.provider.translate_image(png).await,
            Input::Text(text) => session.provider.translate_text(text).await,
        };
        match stream {
            Ok(stream) => {
                let mut stream = std::pin::pin!(stream);
                while let Some(delta) = stream.next().await {
                    // A closed receiver means the window is gone; stop early
                    // rather than paying for a translation nobody will read.
                    if sender.send(delta).await.is_err() {
                        break;
                    }
                }
            }
            Err(err) => {
                let _ = sender.send(Err(err)).await;
            }
        }
    });

    receiver
}

fn record(session: &Session, kind: Kind, reply: &Reply, png: Option<&[u8]>) -> Result<()> {
    if reply.is_empty() {
        return Ok(());
    }

    let image_path = match png.filter(|_| session.settings.keep_images) {
        Some(png) => {
            let dir = xdg::cache_dir()?.join("images");
            let name = images::save(&dir, png)?;
            images::prune(&dir, session.settings.image_cache_mb * 1024 * 1024)?;
            Some(name)
        }
        None => None,
    };

    let mut store = open_store()?;
    store.add(&Record {
        kind,
        model: &session.settings.model,
        target: session.settings.target(),
        source: reply.source(),
        translated: reply.translation(),
        image_path: image_path.as_deref(),
    })?;
    Ok(())
}

/// The window has already closed by the time history is written, so a failure
/// has nowhere to go but the log.
fn report(result: Result<()>) {
    if let Err(err) = result {
        eprintln!("vertere: {err:#}");
    }
}

/// Publishes the tray item and drains its menu choices on the main loop.
///
/// A failure here is not fatal: without a status-notifier host there is nothing
/// to publish to, and every menu entry has a subcommand that still works.
fn start_tray(application: &gtk4::Application) {
    let (sender, receiver) = async_channel::unbounded();
    runtime().spawn(async move {
        if let Err(err) = tray::spawn(sender).await {
            eprintln!("vertere: {err:#}");
        }
    });

    let application = application.clone();
    gtk4::glib::spawn_future_local(async move {
        while let Ok(action) = receiver.recv().await {
            match action {
                tray::Action::Shot => shot(&application),
                tray::Action::Clip => clip(&application),
                tray::Action::Selection => sel(&application),
                tray::Action::History => history_window(&application),
                tray::Action::Settings => settings_window(&application),
                tray::Action::Quit => application.quit(),
            }
        }
    });
}
