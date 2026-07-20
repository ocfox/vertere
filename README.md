# vertere

Translate a screen region, the clipboard, or the selected text. The result
appears in a floating bubble next to your work.

Translation is done by a multimodal model through
[OpenRouter](https://openrouter.ai): the screenshot goes to the model as an
image, and it returns both the translation and a transcription of the source
text. There is no separate OCR step.

## Requirements

- A compositor implementing `wlr-layer-shell` and `wlr-screencopy`: sway,
  Hyprland, river, Wayfire, niri. GNOME implements neither.
- `grim`, `slurp`, `wl-clipboard`. The Nix package wraps these in.
- An OpenRouter API key, and a model that accepts image input.

## Install

NixOS, with the flake:

```nix
{
  inputs.vertere.url = "github:ocfox/vertere";

  # in your configuration
  imports = [ inputs.vertere.nixosModules.default ];

  services.vertere = {
    enable = true;
    environmentFile = "/run/secrets/vertere";  # OPENROUTER_API_KEY=...
  };
}
```

The module runs the daemon as a systemd user service for the graphical session.

Elsewhere: `cargo build --release`, then run `vertere daemon` from your
compositor's autostart.

## Configure

Run `vertere settings`, or pick Settings from the tray. Any command run before
there is a model configured opens that window instead of failing.

`Translate into` goes into the prompt as written, so name the language the way
you would to a person. `Simplified Chinese` says what `zh` leaves the model to
guess. `Unless already in it` is what to use when the source is already in that
language — set it to your other language and the direction takes care of itself.

The API key is read from `OPENROUTER_API_KEY` and is never stored in the
database.

## Use

```
bindsym $mod+t       exec vertere shot
bindsym $mod+Shift+t exec vertere clip
bindsym $mod+y       exec vertere sel
```

- `shot` — drag a region, translate the text in it
- `clip` — translate the clipboard
- `sel` — translate the selected text, no copy needed
- `settings`, `history` — the two windows, also reachable from the tray

`sel` reads the primary selection, which most GTK and Qt applications and
terminals fill on selection. Some, notably a few Electron applications, do not;
use `clip` there.

The bubble closes on Escape, or on its own after eight seconds if you never
touch it. Moving the pointer over it, dragging it, or selecting its text all
count as touching it. Drag it anywhere; it reopens where you left it.

A tray icon offers the same actions. It needs a status-notifier host to be
visible — on sway that is usually waybar with its `tray` module — so the
subcommands above remain the way in when there is none.

Everything is kept in `~/.local/share/vertere/vertere.db`: the settings, and
every translation with its source text and the model used. `vertere history`
searches it.

## License

MIT
