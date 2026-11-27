# GUI theme files

`gui.load_theme_file(path)` loads a version 1 JSON document. A document has
`format: "nytrix-ui-theme"`, a display `name`, semantic RGBA `parts`, and
optional button `states`. Colors use four normalized channels in `[0, 1]`.
The `parts` object requires `text`, `text_dim`, `text_faint`, `accent`, `ok`,
`warn`, `danger`, `surface`, `surface_hover`, `surface_active`, `window`,
`header`, `popup`, `border`, and `press`.

Button states may define `background`, `border`, and `text` colors for
`active`, `hovered`, `pressed`, and `disabled`. Invalid documents are rejected
without changing the live theme. `save_theme_file` writes the current theme;
`watch_theme_file` loads it and `begin_frame` checks for edits every 30 frames.
Use `poll_theme_file` to check immediately, or `unwatch_theme_file` to stop
watching.

The adjacent `dark.json` and `light.json` files are editable examples.

## Ported theme library

This directory also contains 23 themes ported from
[`laluxx/glemax`](https://github.com/laluxx/glemax/tree/main/etc/themes) at
commit `1c82127deb5a85ba6423ee56641496e0abbfdbef`:

`city-lights`, `dark+` (`dark-plus.json`), `dark-one`, `flatwhite`,
`kaolin-bubblegum`, `kaolin-dark`, `kaolin-eclipse`, `kaolin-galaxy`,
`kaolin-ocean`, `kaolin-temple`, `modus-operandi`,
`modus-operandi-deuteranopia`, `modus-operandi-tinted`,
`modus-operandi-tritanopia`, `modus-vivendi`,
`modus-vivendi-deuteranopia`, `modus-vivendi-tinted`,
`modus-vivendi-tritanopia`, `molokai`, `monokai-ristretto`, `nord`,
`the-matrix`, and `tomorrow-night`.

The upstream Emacs faces map to UI parts as follows: `default`
foreground/background to `text`/`window`; `mode-line` background to `header`;
`window-divider` to `border`; `region` to `surface_active`; cursor, prompt, or
highlight colors to `accent`; and `success`/`warning`/`error` to
`ok`/`warn`/`danger`. The remaining UI-only parts and button states are derived
from each palette so every port satisfies the version 1 schema.
