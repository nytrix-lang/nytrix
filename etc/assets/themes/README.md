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
