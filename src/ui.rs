//! The floating translation bubble.

use std::cell::Cell;
use std::rc::Rc;
use std::time::Duration;

use anyhow::Result;
use futures_util::{Stream, StreamExt};
use gtk4::gdk;
use gtk4::glib;
use gtk4::prelude::*;
use gtk4_layer_shell::{Edge, KeyboardMode, Layer, LayerShell};

use crate::store::{DEFAULT_BASE_URL, DEFAULT_FALLBACK_LANG, DEFAULT_TARGET_LANG, Entry, Settings};
use crate::translate::Reply;
use crate::window_state::{self, Position, Screen};

const WIDTH: i32 = 420;
/// Only a starting guess: the window shrinks to its content once laid out, but
/// a position has to be picked before that is known.
const ASSUMED_HEIGHT: i32 = 200;
/// How long a finished bubble stays before dismissing itself, if untouched.
const LINGER: Duration = Duration::from_secs(8);

const CSS: &str = "
window.vertere {
  background: transparent;
}
.bubble {
  background-color: @theme_bg_color;
  border: 1px solid alpha(@theme_fg_color, 0.15);
  border-radius: 12px;
  padding: 14px 16px;
}
.translation {
  font-size: 1.1em;
}
.source {
  opacity: 0.7;
  font-size: 0.9em;
}
.status {
  opacity: 0.55;
  font-style: italic;
}
";

/// Opens a bubble and streams `deltas` into it.
///
/// Returns immediately; `on_done` runs once the reply is complete.
pub fn show<S, F>(application: &gtk4::Application, deltas: S, on_done: F)
where
    S: Stream<Item = Result<String>> + 'static,
    F: FnOnce(Reply) + 'static,
{
    ensure_css_loaded();

    build(application, deltas, on_done);
}

/// Loads the CSS once per process; repeated calls would stack duplicate
/// providers.
fn ensure_css_loaded() {
    static CSS_LOADED: std::sync::Once = std::sync::Once::new();
    CSS_LOADED.call_once(load_css);
}

fn load_css() {
    let provider = gtk4::CssProvider::new();
    provider.load_from_string(CSS);
    if let Some(display) = gdk::Display::default() {
        gtk4::style_context_add_provider_for_display(
            &display,
            &provider,
            gtk4::STYLE_PROVIDER_PRIORITY_APPLICATION,
        );
    }
}

fn build<S, F>(application: &gtk4::Application, deltas: S, on_done: F)
where
    S: Stream<Item = Result<String>> + 'static,
    F: FnOnce(Reply) + 'static,
{
    let window = gtk4::ApplicationWindow::builder()
        .application(application)
        .default_width(WIDTH)
        .resizable(false)
        .build();
    window.add_css_class("vertere");

    window.init_layer_shell();
    window.set_layer(Layer::Overlay);
    window.set_namespace(Some(crate::LAYER_NAMESPACE));
    // On-demand rather than exclusive: the bubble can take Esc when clicked,
    // without stealing the keyboard from whatever the user was doing.
    window.set_keyboard_mode(KeyboardMode::OnDemand);
    window.set_anchor(Edge::Top, true);
    window.set_anchor(Edge::Left, true);

    let translation = gtk4::Label::builder()
        .label("…")
        .wrap(true)
        .selectable(true)
        .focusable(false)
        .xalign(0.0)
        .build();
    translation.add_css_class("translation");

    let source = gtk4::Label::builder()
        .wrap(true)
        .selectable(true)
        .focusable(false)
        .xalign(0.0)
        .build();
    source.add_css_class("source");

    let source_row = gtk4::Expander::builder()
        .label("Source")
        .child(&source)
        .visible(false)
        .build();

    let status = gtk4::Label::builder().xalign(0.0).visible(false).build();
    status.add_css_class("status");

    let bubble = gtk4::Box::new(gtk4::Orientation::Vertical, 8);
    bubble.add_css_class("bubble");
    bubble.append(&translation);
    bubble.append(&source_row);
    bubble.append(&status);
    window.set_child(Some(&bubble));

    // Touching the bubble in any way means it is still wanted, so it stops
    // dismissing itself. Inferring that is more honest than asking for it.
    let touched = Rc::new(Cell::new(false));

    place(&window);
    add_drag(&window, Rc::clone(&touched));
    add_dismiss(&window, Rc::clone(&touched));

    window.present();
    let bubble = Bubble {
        translation,
        source,
        source_row,
        status,
    };
    consume(&window, deltas, bubble, touched, on_done);
}

/// The bubble's content widgets, threaded through as one value rather than
/// individually.
struct Bubble {
    translation: gtk4::Label,
    source: gtk4::Label,
    source_row: gtk4::Expander,
    status: gtk4::Label,
}

fn monitor_of(window: &gtk4::ApplicationWindow) -> Option<gdk::Monitor> {
    window.monitor().or_else(|| {
        gdk::Display::default()?
            .monitors()
            .item(0)?
            .downcast::<gdk::Monitor>()
            .ok()
    })
}

fn place(window: &gtk4::ApplicationWindow) {
    let Some(monitor) = monitor_of(window) else {
        return;
    };
    let geometry = monitor.geometry();
    let output = monitor
        .connector()
        .map(|c| c.to_string())
        .unwrap_or_default();

    let (x, y) = window_state::place(
        window_state::load().as_ref(),
        &output,
        Screen {
            width: geometry.width(),
            height: geometry.height(),
        },
        Screen {
            width: WIDTH,
            height: ASSUMED_HEIGHT,
        },
    );
    window.set_margin(Edge::Left, x);
    window.set_margin(Edge::Top, y);
}

fn remember(window: &gtk4::ApplicationWindow) {
    let Some(output) = monitor_of(window).and_then(|m| m.connector()) else {
        return;
    };
    let position = Position {
        output: output.to_string(),
        x: window.margin(Edge::Left),
        y: window.margin(Edge::Top),
    };
    if let Err(err) = window_state::save(&position) {
        eprintln!("vertere: cannot remember the window position: {err:#}");
    }
}

fn add_drag(window: &gtk4::ApplicationWindow, touched: Rc<Cell<bool>>) {
    let drag = gtk4::GestureDrag::new();
    let origin = Rc::new(Cell::new((0, 0)));

    drag.connect_drag_begin({
        let window = window.clone();
        let origin = Rc::clone(&origin);
        let touched = Rc::clone(&touched);
        move |_, _, _| {
            touched.set(true);
            origin.set((window.margin(Edge::Left), window.margin(Edge::Top)));
        }
    });
    drag.connect_drag_update({
        let window = window.clone();
        let origin = Rc::clone(&origin);
        move |_, dx, dy| {
            let (x, y) = origin.get();
            // Margins are logical pixels, as are gesture offsets, so a scaled
            // output needs no conversion here.
            window.set_margin(Edge::Left, (x + dx as i32).max(0));
            window.set_margin(Edge::Top, (y + dy as i32).max(0));
        }
    });
    drag.connect_drag_end({
        let window = window.clone();
        move |_, _, _| remember(&window)
    });
    window.add_controller(drag);
}

fn add_dismiss(window: &gtk4::ApplicationWindow, touched: Rc<Cell<bool>>) {
    close_on_escape_with(window, remember);

    // Hovering counts as interest even without a click, which covers reading a
    // long translation without touching anything.
    let motion = gtk4::EventControllerMotion::new();
    motion.connect_enter(move |_, _, _| touched.set(true));
    window.add_controller(motion);
}

fn consume<S, F>(
    window: &gtk4::ApplicationWindow,
    deltas: S,
    bubble: Bubble,
    touched: Rc<Cell<bool>>,
    on_done: F,
) where
    S: Stream<Item = Result<String>> + 'static,
    F: FnOnce(Reply) + 'static,
{
    let window = window.clone();
    glib::spawn_future_local(async move {
        let mut deltas = std::pin::pin!(deltas);
        let mut reply = Reply::new();

        while let Some(delta) = deltas.next().await {
            match delta {
                Ok(delta) => reply.push(&delta),
                Err(err) => {
                    bubble.translation.set_label("");
                    show_status(&bubble.status, &format!("Failed: {err:#}"));
                    return linger(window, touched);
                }
            }
            // Whole-text updates rather than incremental ones: at this size the
            // cost is invisible and there is no render cursor to keep in step.
            bubble.translation.set_label(reply.translation());
            if reply.has_source() && !reply.source().is_empty() {
                bubble.source.set_label(reply.source());
                bubble.source_row.set_visible(true);
            }
        }

        if reply.is_empty() {
            show_status(&bubble.status, "No translation returned");
            return linger(window, touched);
        }
        on_done(reply);
        linger(window, touched);
    });
}

fn show_status(status: &gtk4::Label, text: &str) {
    status.set_label(text);
    status.set_visible(true);
}

/// Dismisses the window once the reply is settled, unless it was touched.
fn linger(window: gtk4::ApplicationWindow, touched: Rc<Cell<bool>>) {
    glib::timeout_add_local_once(LINGER, move || {
        if !touched.get() {
            remember(&window);
            window.close();
        }
    });
}

/// Opens the settings window.
///
/// A window of its own rather than a control on the bubble: the bubble is read
/// in a hurry and closes itself, which is the opposite of what editing needs.
pub fn show_settings(application: &gtk4::Application, settings: Settings) {
    ensure_css_loaded();

    let window = gtk4::ApplicationWindow::builder()
        .application(application)
        .title("Vertere settings")
        .default_width(460)
        .build();

    // The placeholders are the values an empty box actually uses, so what is
    // greyed out is what will happen.
    let model = entry(&settings.model, "vendor/model-name");
    let target = entry(&settings.target_lang, DEFAULT_TARGET_LANG);
    let fallback = entry(&settings.fallback_lang, DEFAULT_FALLBACK_LANG);
    let base_url = entry(&settings.base_url, DEFAULT_BASE_URL);

    let rows = gtk4::Grid::builder()
        .row_spacing(8)
        .column_spacing(12)
        .margin_top(16)
        .margin_bottom(16)
        .margin_start(16)
        .margin_end(16)
        .build();
    for (row, (label, field, hint)) in [
        (
            "Model",
            &model,
            "A model slug from the endpoint below. It must accept image input.",
        ),
        (
            "Translate into",
            &target,
            "Named as you would say it to a person, not a code.",
        ),
        (
            "Unless already in it",
            &fallback,
            "Then translate into this instead.",
        ),
        (
            "Endpoint",
            &base_url,
            "Any OpenAI-compatible API. Defaults to OpenRouter's.",
        ),
    ]
    .into_iter()
    .enumerate()
    {
        let row = row as i32 * 2;
        let caption = gtk4::Label::builder().label(label).xalign(0.0).build();
        let note = gtk4::Label::builder().label(hint).xalign(0.0).build();
        note.add_css_class("status");
        rows.attach(&caption, 0, row, 1, 1);
        rows.attach(field, 1, row, 1, 1);
        rows.attach(&note, 1, row + 1, 1, 1);
    }

    let status = gtk4::Label::builder().xalign(0.0).visible(false).build();
    status.add_css_class("status");

    let save = gtk4::Button::with_label("Save");
    save.add_css_class("suggested-action");
    save.connect_clicked({
        let window = window.clone();
        let (model, target, fallback, base_url) = (
            model.clone(),
            target.clone(),
            fallback.clone(),
            base_url.clone(),
        );
        let status = status.clone();
        // Rebuilt per click rather than mutated in place: the button hands out
        // an `Fn`, and the fields the view does not show must survive untouched.
        let base = settings.clone();
        move |_| {
            let settings = Settings {
                model: model.text().trim().to_owned(),
                target_lang: target.text().trim().to_owned(),
                fallback_lang: fallback.text().trim().to_owned(),
                base_url: base_url.text().trim().to_owned(),
                ..base.clone()
            };

            if !settings.is_usable() {
                show_status(&status, "A model is required");
                return;
            }
            match crate::app::save_settings(&settings) {
                Ok(()) => window.close(),
                Err(err) => show_status(&status, &format!("Could not save: {err:#}")),
            }
        }
    });

    let actions = gtk4::Box::new(gtk4::Orientation::Horizontal, 8);
    actions.set_halign(gtk4::Align::End);
    actions.append(&save);

    let body = gtk4::Box::new(gtk4::Orientation::Vertical, 8);
    body.append(&rows);
    body.append(&status);
    body.append(&actions);
    body.set_margin_bottom(12);
    body.set_margin_end(16);
    window.set_child(Some(&body));

    close_on_escape(&window);
    window.present();
}

fn entry(value: &str, placeholder: &str) -> gtk4::Entry {
    gtk4::Entry::builder()
        .text(value)
        .placeholder_text(placeholder)
        .hexpand(true)
        .build()
}

fn close_on_escape(window: &gtk4::ApplicationWindow) {
    close_on_escape_with(window, |_| {});
}

/// Closes `window` on Escape, running `before_close` first.
fn close_on_escape_with(
    window: &gtk4::ApplicationWindow,
    before_close: impl Fn(&gtk4::ApplicationWindow) + 'static,
) {
    let keys = gtk4::EventControllerKey::new();
    keys.connect_key_pressed({
        let window = window.clone();
        move |_, key, _, _| {
            if key == gdk::Key::Escape {
                before_close(&window);
                window.close();
                return glib::Propagation::Stop;
            }
            glib::Propagation::Proceed
        }
    });
    window.add_controller(keys);
}

/// Opens the history window.
///
/// `search` is handed in rather than the rows themselves, so typing re-queries
/// the database instead of filtering a snapshot that goes stale on every new
/// translation.
pub fn show_history(
    application: &gtk4::Application,
    search: impl Fn(&str) -> Result<Vec<Entry>> + 'static,
) {
    ensure_css_loaded();

    let window = gtk4::ApplicationWindow::builder()
        .application(application)
        .title("Vertere history")
        .default_width(560)
        .default_height(600)
        .build();

    let query = gtk4::SearchEntry::builder()
        .placeholder_text("Search the source text and the translations")
        .build();

    let list = gtk4::ListBox::builder()
        .selection_mode(gtk4::SelectionMode::None)
        .build();
    list.add_css_class("boxed-list");

    let status = gtk4::Label::builder().xalign(0.0).visible(false).build();
    status.add_css_class("status");

    // Shared rather than cloned: the closure owns `search`, which is only
    // required to be callable, not duplicable.
    let refresh = Rc::new({
        let list = list.clone();
        let status = status.clone();
        move |text: &str| {
            while let Some(child) = list.first_child() {
                list.remove(&child);
            }
            match search(text) {
                Ok(entries) if entries.is_empty() => {
                    show_status(&status, "Nothing here yet");
                }
                Ok(entries) => {
                    status.set_visible(false);
                    for entry in entries {
                        list.append(&history_row(&entry));
                    }
                }
                Err(err) => show_status(&status, &format!("Could not read: {err:#}")),
            }
        }
    });
    refresh("");

    query.connect_search_changed({
        let refresh = Rc::clone(&refresh);
        move |query| refresh(query.text().as_str())
    });

    let scroller = gtk4::ScrolledWindow::builder()
        .child(&list)
        .vexpand(true)
        .build();

    let body = gtk4::Box::new(gtk4::Orientation::Vertical, 8);
    body.set_margin_top(12);
    body.set_margin_bottom(12);
    body.set_margin_start(12);
    body.set_margin_end(12);
    body.append(&query);
    body.append(&status);
    body.append(&scroller);
    window.set_child(Some(&body));

    close_on_escape(&window);
    window.present();
}

fn history_row(entry: &Entry) -> gtk4::Widget {
    let translated = gtk4::Label::builder()
        .label(&entry.translated)
        .wrap(true)
        .selectable(true)
        .focusable(false)
        .xalign(0.0)
        .build();

    let source = gtk4::Label::builder()
        .label(&entry.source)
        .wrap(true)
        .selectable(true)
        .focusable(false)
        .xalign(0.0)
        .build();
    source.add_css_class("source");

    let meta = gtk4::Label::builder()
        .label(format!("{} · {}", entry.kind, entry.model))
        .xalign(0.0)
        .build();
    meta.add_css_class("status");

    let row = gtk4::Box::new(gtk4::Orientation::Vertical, 4);
    row.set_margin_top(10);
    row.set_margin_bottom(10);
    row.set_margin_start(12);
    row.set_margin_end(12);
    row.append(&translated);
    if !entry.source.is_empty() {
        row.append(&source);
    }
    row.append(&meta);
    row.upcast()
}
