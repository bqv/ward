# The cursor set the client ships

Ten cursors, drawn here as SVG and packed into Xcursor files. Nothing is
derived from Adwaita, Breeze or any other installed theme: the paths in
`*.svg` are this repository's own artwork, so the whole directory is under the
repository's MPL-2.0 (`../../LICENSE`) and carries no third-party licence
obligation.

```
*.svg                     source, 24-unit design box, data-hotspot="x y"
rebuild, rebuild-one      regenerates themes/dsh/cursors from the SVGs
themes/default/index.theme  Inherits=dsh   <- how the theme gets selected
themes/dsh/index.theme      Inherits=Adwaita <- fallback for shapes not drawn
themes/dsh/cursors/<name>  packed 22/24/32/48/64 Xcursor files
```

## Why it exists

Nothing on this box selects a cursor theme: there is no
`/usr/share/icons/default`, no `~/.icons`, and no desktop settings daemon. The
loader therefore asks for the theme named `default`, finds nothing, and every
pointer falls through to the X core cursor **font** — the 1980s bitmap arrow.
Adwaita was installed but never in use.

## How it is selected

`selectCursorTheme()` in `src/main.cpp`, before `QGuiApplication` is
constructed, prepends this directory to `XCURSOR_PATH`. That is the only lever
that works here: Qt 6's xcb platform calls `xcb_cursor_load_cursor()` and
libxcb-cursor 0.1.6 reads `XCURSOR_PATH` and the `Xcursor.theme` **X resource**,
but **not** the `XCURSOR_THEME` environment variable — it contains no such
string. `qputenv("XCURSOR_THEME", ...)` would silently do nothing.

Because no theme *name* is set, the loader resolves `default`, which is why
`themes/default/index.theme` exists; it points at `dsh`, and `dsh` inherits
Adwaita so any shape this set does not draw still gets real artwork.

The change is per process. It does not touch the owner's other applications,
`~/.icons`, or anything under `/usr/share/icons`.

## Changing a shape

Edit the SVG, keep `data-hotspot` in the 24-unit box, run `./rebuild`. It needs
`rsvg-convert` and `xcursorgen` — development tools only; the packed files are
committed, so an install needs neither.

Add an alias by hand when you add a shape (a relative symlink in
`themes/dsh/cursors`); `rebuild-one` explains why they are not generated.

## Packaging

The ebuild installs the whole tree:

```
insinto /usr/share/dsh-native
doins -r "${S}/assets/cursors/themes"
```

producing `/usr/share/dsh-native/themes/{default,dsh}/...`, which is the third
candidate `selectCursorTheme()` looks in. No other packaging change is needed:
the files are data, and the binary already owns the environment variable.
