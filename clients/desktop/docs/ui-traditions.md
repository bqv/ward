# Desktop UI traditions: beautiful, themeable, and CSS-free

An exploration, written for `dsh-native`. Everything marked **[verified]** was
checked on this box against Qt 6.11.2 or read from a file on this machine, not
recalled.

---

## 1. The objection is the tradition, not a departure from it

"CSS for native UI" feels wrong because it *is* the anomaly. The desktop themed
its widgets for thirty years before anyone reached for a stylesheet:

| Era | Mechanism | Unit of theme |
|---|---|---|
| Motif / Win95 / classic Qt | draw code + palette | C, per-widget |
| GTK1 / GTK2 | `gtkrc` + **engine modules written in C** | config + a plugin |
| Qt 2–4 | `QStyle` subclass + `QPalette` | a C++ class |
| Swing | `LookAndFeel` subclass | a Java class |
| KDE | `QStyle` plugins: Oxygen, Breeze, **Kvantum** (SVG + config) | a plugin + assets |
| GTK 3 (2011), sole route by 3.20 (2016) | CSS | a stylesheet |
| Qt 4.2+ (2006) | QSS | a stylesheet string |

Everything that made desktop applications feel like desktop applications was
built without it.

**And CSS was never what made GTK3 good.** Two properties did that:

1. **Widgets are behaviour; themes are paint.** A theme never reimplements a
   button. It keeps its keyboard activation, focus ring, mnemonic and
   accessibility role; the theme decides only what it looks like.
2. **One theme covers the whole widget set.** Consistency is structural — there
   is nowhere to put a themed button next to an unthemed scrollbar.

Both are achievable with **no stylesheet at all**, and GTK2 is the proof.

### Two quotes that settle it

GNOME's own position, on GTK3, from Tobias Bernard's
[Restyling apps at scale](https://blogs.gnome.org/tbernard/2018/10/15/restyling-apps-at-scale/):

> A fundamental misconception a lot of people have is that GTK 3 supports
> theming. This is not true, as there is no clearly defined theming API. There
> are CSS stylesheets, but they were only ever meant to be used by the platform
> and app developers.

So the "beautiful, themeable native toolkit" that GTK3 is remembered as was never
designed to be that. It looked that way because the plumbing happened to be
stable until 3.20 changed selector matching from widget type names to element
names and broke third-party themes in a point release
([Debian #832422](https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=832422)).

And Qt, on QSS, from
[Qt Style Sheets](https://doc.qt.io/qt-6/stylesheet.html):

> Style sheets are a tool for prototyping and for small, local adjustments; for
> the production look of an application, implement a QStyle or use StyleKit.

— followed by "Don't use this function to switch themes at runtime." The toolkit
that invented the closest thing to GTK CSS tells you not to ship with it. Both
positions are the opposite of what the ecosystem assumed.

One clarification for this repository: `DshButton.qml` quotes GTK's CSS in its
comments as the specification for Adwaita's numbers. That is *reading a
specification*, not adopting a mechanism, and it is the right way to get the
numbers right.

---

## 2. The GTK2 architecture — the one you actually want

GTK2 has no CSS, and yet it has every property people credit CSS with. Here is
the real structure, read from this machine's
`/usr/share/themes/Adwaita-dark/gtk-2.0/`. **[verified]**

### Layer 1 — a named colour scheme

The top of `gtkrc` is a flat token table, and nothing downstream hardcodes a
colour:

```
gtk-color-scheme = "text_color:white\nbase_color:#232729"
gtk-color-scheme = "fg_color:#eeeeec\nbg_color:#33393b"
gtk-color-scheme = "selected_fg_color:#ffffff\nselected_bg_color:#215d9c"
gtk-color-scheme = "insensitive_fg_color:#919494\ninsensitive_bg_color:#2d3234"
gtk-color-scheme = "tooltip_fg_color:#ffffff\ntooltip_bg_color:#343434"
```

Then it hands over to the widget recipes:

```
include "main.rc"
include "apps.rc"
include "hacks.rc"
```

**This is `Theme.h`.** A single token table, declared once, referenced
everywhere. GTK2 called it `gtk-color-scheme`; you call it a `Q_PROPERTY`.

### Layer 2 — per-widget recipes, with geometry and per-state colour

```
style "button" {
  xthickness = 2
  ythickness = 2
  ...
}
```

`xthickness`/`ythickness` are the widget's border and padding metrics — the
direct ancestor of `QStyle::pixelMetric()` and of `Theme.controlHeight` /
`Theme.radius` / `implicitWidth` in your controls. Geometry lived beside paint,
in the same recipe.

The colour side is an array per role, indexed by **state**:

```
fg[NORMAL]      bg[NORMAL]      base[NORMAL]      text[NORMAL]
fg[PRELIGHT]    bg[PRELIGHT]    ...               ...
fg[ACTIVE]      ...             ...               ...     # pressed
fg[SELECTED]    ...             ...               ...
fg[INSENSITIVE] ...             ...               ...
```

Five states: `NORMAL`, `PRELIGHT` (hover), `ACTIVE` (pressed), `SELECTED`,
`INSENSITIVE`. Eight roles: `fg`, `bg`, `base`, `text`, `light`, `dark`, `mid`,
`text_aa`. That is a complete state machine for widget appearance, expressed as
data.

In QML that state machine is a binding — `control.hovered`, `control.down`,
`control.enabled`, `control.visualFocus`, `control.checked`. Same machine, better
notation.

### Layer 3 — the engine: the look is a replaceable plugin

The actual drawing is a **shared object loaded at runtime**, parameterised from
the rc file. This machine has two:

```
/usr/lib64/gtk-2.0/2.10.0/engines/libadwaita.so
/usr/lib64/gtk-2.0/2.10.0/engines/libpixmap.so
```

and Adwaita-dark's button drives one of them:

```
style "button" {
  xthickness = 2
  ythickness = 2

  engine "pixmap" {
    image {
      function = BOX
      state    = NORMAL
      file     = "assets/button.png"
      border   = {4, 4, 4, 4}
      stretch  = TRUE
    }
    image { function = BOX  state = PRELIGHT  file = "assets/button-hover.png" ... }
  }
}
```

`function = BOX` with `border = {4,4,4,4}` and `stretch = TRUE` is a **nine-slice
box**: four fixed corners, four stretched edges, a stretched centre, drawn from
one PNG (or, in Kvantum's modern version, one SVG). The same technique Plasma
Style uses today, and it is why those themes look crisp at any DPI.

### And the binding: `class` and `widget_class`

```
class "GtkButton"                            style "button"
class "GtkEntry"                             style "entry"

widget_class "*<GtkTreeView>*<GtkButton>*"   style "treeview_header"
```

`class` matches a widget type; `widget_class` matches a **structural path** — "a
button anywhere inside a tree view". That is the ancestor of a CSS selector, and
it is the piece people miss most when they think CSS invented contextual styling.

### Why this matters here

GTK2 is three layers, and `dsh-native` already has two of them:

| GTK2 | `dsh-native` today |
|---|---|
| `gtk-color-scheme` token table | `Theme.h` |
| `style "button" { xthickness = 2 ... }` | `DshButton.qml` + `Theme.radius`, `Theme.controlHeight` |
| per-state colour arrays | `control.hovered ? … : control.down ? …` bindings |
| `engine "pixmap" { image { ... } }` | the drawn gradient / hairline stack in `DshButton` |
| `class "GtkButton" style "button"` | **missing** |
| `widget_class "*<GtkTreeView>*<GtkButton>*"` | **missing** |

The two missing rows are exactly what a **vendored Qt Quick Controls style**
supplies: the `qmldir` is the `class` binding, and per-file control QML is the
recipe. GTK2 did not have a stylesheet; it had *a directory of widget recipes
plus a token file plus a drawing plugin*. That is also a complete description of
a QQC2 style directory plus a theme singleton.

**So the vendored style is not a fallback for people who dislike CSS. It is the
GTK2 architecture, with QML as the engine language.** That framing is the reason
recipe 2 below is the important one.

---

## 3. Theming without a stylesheet: four idioms

| Idiom | Used by | Unit | Granularity |
|---|---|---|---|
| **Palette struct** | `GtkStyle` arrays, `QPalette`, egui `Visuals`, Slint `Palette`, QML `SystemPalette` | roles × states | coarse — no per-widget say |
| **Paint override** | `QStyle` / `QProxyStyle`, Breeze, Kvantum, GTK2 engines | code that draws each element | total, and it is code |
| **Component override** | Qt Quick Controls 2 vendored style | the widget's own QML | total, per widget |
| **Theme singleton + inherited colour sets** | `Kirigami.Theme`, **`Theme.h` here** | a token object inherited down the tree | medium |

**Paint override** is the one `Theme.h` already invokes: its own comment notes
that Fusion draws its bevel "with a gradient plus two border tones". That is
`QStyle` — a C++ class drawing every control element, with `pixelMetric()` and
`sizeFromContents()` deciding geometry. Kvantum is proof it scales to a beautiful
result.

**Component override** is the one that fits this app, and §2 is why.

**Kirigami shows how far the singleton goes.** `Kirigami.Theme` is not a flat
token list; it defines colour *sets* (`Window`, `View`, `Button`, `Selection`,
`Tooltip`, `Complementary`) and an item declares its set, with the setting
inherited through the scene graph. CSS inheritance's useful half, as attached
properties. Kirigami's spacing is likewise a real scale, not folklore: its small
and large steps are **4 px and 8 px** derived from one grid unit (commit
`6da5023b`, and per-style units at `src/styles/org.kde.desktop/Units.qml`).

**Kiragami and `qqc2-desktop-style` are *not* installed on this box** — there is
no `org/` directory in `/usr/lib64/qt6/qml/`. So the KDE route is available only
by installing KDE Frameworks.

---

## 4. The four Qt answers

| Tradition | Widget model | Unit of theme | Verdict |
|---|---|---|---|
| **Qt Widgets + `QProxyStyle`/`QStyle` + `QPalette`** | C++ retained widgets | a C++ class | The classical answer — the GTK2 engine model in Qt. Kvantum and Breeze prove the ceiling. No stylesheet. |
| **Qt Widgets + QSS** | same | a stylesheet string | The one to skip here. A limited CSS subset with no variables. |
| **QQC2 + a vendored style** | QML retained widgets (`QtQuick.Templates`) | the style directory | **This is GTK2's architecture.** Full behaviour inherited, you own every control's paint, no CSS. Verified below. |
| **QQC2 built-in style + per-control overrides** | same | loose detached files | The half-themed trap — consistent only where you remembered. Where `dsh-native` is today. |
| **Widgets built from bare `Item`** | hand-drawn `Item`s | ad hoc | Looks controlled, behaves badly. Also where `dsh-native` is today. |

If you ever wanted the opposite of a signature look — the app *inheriting* the
user's system theme — the KDE answer is `org.kde.desktop`, which renders Qt Quick
Controls **through the Qt Widgets `QStyle`**, so a QML app picks up Breeze,
Fusion or Kvantum like a native Widgets app. That is the right answer for
"respect the user's theme" and the wrong one for `dsh-native`, whose whole design
is a fixed Adwaita-dark signature. Worth knowing it exists; not worth installing
Frameworks for.

Immediate-mode toolkits (Dear ImGui, egui, Nuklear) are named only to dismiss: no
widget tree and no external theming at all.

---

## 5. What is actually on this box **[verified]**

```
Qt 6.11.2 in /usr/lib64
QtQuick.Controls styles: Basic  Fusion  Imagine  Material  Universal  FluentWinUI3
QtQuick.Templates:       present
Basic style:             70 QML files, 364 KiB
KDE QML modules:         none (no org/ in the QML tree)
GTK:                     2.24.33 and GTK3 both installed
GTK2 engines:            libadwaita.so, libpixmap.so
Desktop:                 i3, X11 (XDG_CURRENT_DESKTOP=i3, XDG_SESSION_TYPE=x11)
Qt colorScheme:          Unknown (0)
```

Three of those matter more than they look.

- **`FluentWinUI3` exists** in 6.11 — a new first-party style, easy to miss.
- **`QtQuick.Templates` is present.** The style-agnostic base widget set, and the
  key to recipe 1.
- **`colorScheme` is `Unknown`, and that is correct here.** There is no desktop
  dark-mode signal on this machine: no GNOME settings daemon, no
  `~/.config/gtk-4.0/gtk.css`, and `dconf` cannot write its user file. Any recipe
  beginning "read the OS dark-mode preference" is inapplicable on i3, and the
  hardcoded Adwaita-dark palette is the only defensible choice.

---

## 6. Where `dsh-native` sits today

`main.cpp` selects **Fusion** when the caller has not named a style:

```cpp
if (QQuickStyle::name().isEmpty())
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
```

Right instinct — Fusion is the first-party style that still believes in desktop
bevels, and its `QStyle` lineage is the paint-override tradition above. But the
app then replaces controls *individually*:

- **`DshButton` is a bare `Item`.** No `focus`, no keyboard activation, no
  `Accessible.role`, no `checked`, no `flat`, no `icon`, no `display`, and it
  cannot join a `ButtonGroup`. The template probe in recipe 1 reports
  `activeFocusOnTab = true` and `focusPolicy = 11` (`StrongFocus`) for free. A
  mouse-only button is the largest single gap between this app and a native tool.
- `DshTextField` and `DshComboBox` derive from the **styled** types in
  `QtQuick.Controls`, not from `QtQuick.Templates`. They still depend on whichever
  style is active — the coupling the design is trying to remove.
- Typography has drifted less than it did: `Theme.fontUI` is 13 and both
  `DshTextField` and `DshComboBox` now take their sizes from the theme tokens
  instead of literals, but `DshComboBox`'s popup padding is still a literal 4
  rather than `Theme.space2`.
- **Everything else is Fusion**: dialogs, menus, scroll bars, popups, tooltips,
  spin boxes, tab bars, `ItemDelegate`. That is where inconsistency shows first.

The reference render is `_spike/ui-preview/live-window.png`.

### The measured style gallery **[verified]**

Rendered offscreen through `_spike/ui-preview` (which needs no live host — its
default path uses a synthetic `DemoTransport`), once per installed style, with
`QT_QUICK_CONTROLS_STYLE` set and no recompile:

| Style | md5 of the render | Differs from Fusion in |
|---|---|---|
| `Fusion` | `62f2fa82…` | — (what `main.cpp` sets) |
| `Basic` | `62f2fa82…` | **nothing — byte-identical** |
| `Material` (dark) | `4d3c6aea…` | y 679–698, x 10–858 — the composer only |
| `Universal` (dark) | `d5e62205…` | x 16–412, y 88–701 — sidebar + composer |
| `FluentWinUI3` | `635458d6…` | y 686–704, x 20–387 — the composer only |

**Fusion and Basic produce byte-identical output.** In a 940×720 window only a
few hundred pixels change across the other three styles. That is the strongest
possible evidence for the thesis of this document: `Theme` already owns very
nearly every pixel of this application, so the underlying QQC2 style is close to
irrelevant — *until* you reach a control it does not cover.

Two things follow, and they point in opposite directions:

- **The good news** is that the Adwaita port is genuinely in control. This is not
  a Fusion app with a few overrides; it is a themed app that happens to sit on
  Fusion.
- **The bad news** is what the leak consists of. The composer's `TextArea` in
  `main.qml` overrides `background` but **not** `padding`,
  `topPadding`/`bottomPadding` or `implicitHeight`. So Material and Fluent still
  decide the field's *geometry* while `Theme` decides its *paint* — the text sits
  differently and the whole composer row shifts. **Overriding the paint of a
  styled control leaves the style in charge of its metrics.** That is the
  half-themed trap in one control, and it is the argument for recipe 1 in
  miniature: derive from `T.TextArea`, where the metrics are yours too.

The renders are in `_scratch/ui-gallery/` (`contact.png`, `composer-zoom.png`,
`sidebar-zoom.png`).

---

## 7. Examples: what to steal from whom

Split by lineage, because the advice does not transfer between them. Only QML
apps have QQC2 styles; a Qt Widgets app is a different theming world entirely.

### Qt Quick / QML — the lineage that applies here

- **MuseScore 4** — a large, genuinely polished desktop app on Qt Quick/QML with
  a C++ core ([repo](https://github.com/musescore/MuseScore),
  [framework](https://github.com/musescore/muse_framework)). Verified from the
  repositories:

  | | |
  |---|---|
  | QML files in the app | **509**, one module per feature (`src/propertiespanel/qml/MuseScore`, `src/notationscene/qml/MuseScore`, …) |
  | QML files in the framework | **242**, of which **94** are `Muse.UiComponents` |
  | Style selection | `qputenv("QT_STYLE_OVERRIDE", "Fusion")` in `src/app/main.cpp` — **no vendored QQC2 style at all** |
  | Theme | `framework/ui/api/themeapi.h` — a C++ `ThemeApi` object exposed to QML, read as `ui.theme.*` |
  | Docking | KDDockWidgets vendored under `framework/dockwindow/thirdparty/` |
  | Shadows and blur | `Qt5Compat.GraphicalEffects` |

  **The theme is the same pattern as `Theme.h`**, and it is worth diffing against:

  | `dsh-native` `Theme.h` | MuseScore `ThemeApi` |
  |---|---|
  | `CONSTANT` | `NOTIFY themeChanged` — the theme can change at runtime |
  | (dark only) | `bool isDark` as a first-class axis |
  | `bg` / `panel` / `raised` | `backgroundPrimary` / `Secondary` / `Tertiary` / `Quarternary` |
  | `buttonHover` / `buttonPressed` colours | `buttonOpacityNormal` / `Hover` / `Hit` — *opacity*, not three colours |
  | `fontUI` / `fontData` (int pixel sizes) | `bodyFont` / `bodyBold` / `largeBody` / `tab` / `header` / `titleBold` (**`QFont`**) |
  | — | `borderWidth`, `textFieldColor`, `focusColor`, `linkColor`, `popupBackgroundColor`, `borderWidth` |
  | — | `QVariantMap extra` as an escape hatch |

  Two ideas there are strictly better than the current token set: interaction
  states as **opacity multipliers on one colour** rather than three hand-picked
  greys, and typography as **`QFont` values** rather than pixel sizes.

  **The surprise:** MuseScore does *not* derive from `QtQuick.Templates`.
  `FlatButton.qml` is a `FocusScope`, and the framework provides
  `FocusableControl.qml` / `FocusableItem.qml` — a hand-rolled base that drives a
  custom `NavigationControl` system for keyboard and remote/gamepad navigation.
  It also draws its notation canvas itself instead of forcing a stock control to
  do it.

  The lesson is therefore not "derive from `T.*`" but something sharper:
  **MuseScore has one focus base type and 94 components on top of it, while
  `dsh-native` has three unrelated implementations.** The route matters less than
  the fact that there is a single base. Deriving from `T.Button` is the cheaper
  way to get that base and the accessibility role with it; MuseScore's route is
  what you would do instead if you needed bespoke navigation.
- **Kirigami and the KDE app family** (NeoChat, Tokodon, KClock, Merkuro,
  Itinerary) — the closest thing in Qt to the "widgetery" feel you want
  ([docs](https://develop.kde.org/docs/getting-started/kirigami/),
  [HIG](https://develop.kde.org/hig/layout_and_nav/)). Notably these apps ship
  **no custom style of their own**: they get coherence from a component layer
  (`AbstractCard`, `FormLayout`, drawers) plus a token system. Steal the
  **spacing discipline** and the **declare-actions-once, surface-them-in-toolbar-
  menu-and-context-menu** model.
- **Ubuntu Touch / Lomiri** — `Suru` is a real vendored QQC2 style, shipped as
  `qml-module-qtquick-controls2-suru`. Proof that a whole-distro look can live in
  one style directory.
- **[Qt Quick Controls Gallery](https://doc.qt.io/qt-6/qtquickcontrols-gallery-example.html)**
  — renders every control under every style. Cheapest way to decide "Fusion plus
  my palette" versus "I need a style of my own", and the model for a gallery app
  of your own components.

### Qt Widgets — where the CSS-free examples live, and mostly do not apply

- **Kvantum** ([repo](https://github.com/tsujan/Kvantum),
  [theme-making guide](https://sources.debian.org/data/main/q/qt5-style-kvantum/0.18.0%2Brepack-1/doc/Theme-Making.pdf))
  — the ceiling for paint-override theming: a Qt Widgets `QStyle` engine driven by
  SVG assets and a config file, the direct modern descendant of `engine "pixmap"`.
  **It cannot theme QML apps**, so it is a reference, not a route.
- **[Qt's own Styles Example](https://doc.qt.io/qtforpython-6.5/overviews/qtwidgets-widgets-styles-example.html)**
  — the canonical `QProxyStyle` walkthrough.
- **Telegram Desktop** — the strongest "beautiful Qt Widgets" case, and the
  instructive one: it reimplements its own widget vocabulary and title bar, so it
  does not look like Qt *because it does not use Qt's widget look*. In QML the
  equivalent is a vendored style, not Fusion plus tweaks.
- Counter-examples worth noting: **OBS**, **qBittorrent** and **KeePassXC** all
  reach for QSS, and KeePassXC's users fight it — user stylesheets and the app's
  own collide. **Calibre** is the same story. That is the path already rejected.

---

## 8. Recipes

### Recipe 1 — Derive from the template, never from `Item` **[verified]**

`QtQuick.Templates` is the behaviour layer; `QtQuick.Controls` is the *styled*
layer on top. Import the templates and supply only the paint:

```qml
import QtQuick
import QtQuick.Templates as T

component DshButton: T.Button {
    id: control

    implicitWidth: Math.max(64, label.implicitWidth + 16)
    implicitHeight: 28

    // The Adwaita drawing is unchanged from DshButton.qml — gradient, two-tone
    // border, inset highlight. Only the base class moved.
    background: Rectangle {
        radius: 5
        border.width: 1
        border.color: "#1b1b1b"
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.0; color: control.down ? "#282828" : "#323232" }
            GradientStop { position: 1.0; color: control.down ? "#282828" : "#353535" }
        }
    }

    contentItem: Text {
        id: label
        text: control.text
        color: control.enabled ? "#eeeeec" : "#5b5b5b"
        font.pixelSize: 12
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
```

Verified by rendering it offscreen on Qt 6.11.2:

```
activeFocusOnTab = true
focusPolicy      = 11        // StrongFocus
```

Plus keyboard activation (Space/Enter), `hovered`, `down`, `checked`, `flat`,
`highlighted`, `icon`, `display`, `ButtonGroup`, mnemonics, `padding`, `insets`
and the accessibility role — none of which a hand-drawn `Item` has.

- Use `control.visualFocus` for the accent focus ring: the template's "focused by
  keyboard, not by mouse", which is the correct predicate.
- Use `T.TextField` and `T.ComboBox` for the same reason in the other two files.

### Recipe 2 — Vendor a whole style, i.e. build GTK2's layers 2 and 3 **[verified]**

```
qml/styles/Dsh/qmldir  →  module Dsh
                          Button 1.0 Button.qml
qml/styles/Dsh/Button.qml  →  T.Button { /* paint only */ }
```

Point Qt at it before the engine loads:

```cpp
QQuickStyle::setStyle(QStringLiteral("Dsh"));
QQuickStyle::setFallbackStyle(QStringLiteral("Fusion"));   // controls not yet written
engine.addImportPath(dataDir + "/styles");                 // the PARENT of Dsh/
```

or entirely by environment while developing:

```sh
QML_IMPORT_PATH=$PWD/qml/styles \
QT_QUICK_CONTROLS_STYLE=Dsh \
./build/dsh-native --dev
```

**Verified**: with the style directory on the import path and
`QT_QUICK_CONTROLS_STYLE=Dsh`, a plain `import QtQuick.Controls; Button {}` that
never mentions the style by name loaded the vendored `Button.qml`. Without the
environment, it did not.

Add `qmldir` entries only for controls you override; everything absent falls
through to the fallback style, so a half-written style degrades to Fusion rather
than to nothing.

**The rules that bite**, from Qt's own
[customisation guide](https://doc.qt.io/qt-6/qtquickcontrols-customize.html):

- Style files must root in `QtQuick.Templates`, never `QtQuick.Controls`, or they
  derive from themselves.
- **Never `import QtQuick.Controls` inside a style file** — it breaks the QML
  compiler.
- Never give an `id` to a style's `background`/`contentItem`; it defeats the
  single-allocation optimisation and keeps an item alive for the app's lifetime.
- Restyling `ScrollBar` also requires supplying `ScrollView` — controls do not
  pick up delegates automatically.
- Global tokens (radius, elevation, density) belong in an **attached property**
  backed by a C++ type, which is the sanctioned hook for exactly the job
  `Theme.h` does.
- The macOS and Windows styles are documented as **not** suitable for
  customising; base custom work on Basic, Fusion, Imagine, Material or Universal.

**On licensing.** Qt's own `Basic/Button.qml` carries:

```
SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
```

Copying Qt's style sources pulls those terms onto those files. You almost never
need to: the Basic style's `Button.qml` is 48 lines and mostly boilerplate, and
recipe 1 produces something shorter and clearer. Copy Basic only if you want a
complete ready-made control set to edit; if you do, keep the SPDX headers and
treat those files as LGPL-3.0.

### Recipe 3 — Keep the Adwaita spec readable **[verified]**

The on-disk GTK3 `gtk.css` is a stub.
`/usr/share/themes/Adwaita-dark/gtk-3.0/gtk.css` is **80 bytes**:

```css
@import url("resource:///org/gtk/libgtk/theme/Adwaita/gtk-contained-dark.css");
```

The real stylesheet is compiled into the library, which is why the `gresource`
method recorded in `Theme.h` was necessary. The canonical command:

```sh
gresource extract /usr/lib64/libgtk-3.so.0 \
  /org/gtk/libgtk/theme/Adwaita/gtk-contained-dark.css > ~/tmp/adwaita-dark.css
```

Now the port can be *diffed* against its source:
`grep -n -A12 '^button {' ~/tmp/adwaita-dark.css`. Assets sit alongside the CSS
under the same resource prefix. The GTK2 equivalent is already plain text at
`/usr/share/themes/Adwaita-dark/gtk-2.0/main.rc` — 60 KiB of readable recipes,
useful for cross-checking a control's geometry.

Both are references for numbers, not mechanisms to adopt.

### Recipe 4 — Two declarative levers, neither of them CSS

**(a) The config file is Qt's `gtkrc`.** `qtquickcontrols2.conf` is a plain
key/value file — colours and fonts, no stylesheet — consumed by the active style.
Qt documents a palette sub-group for **every** style, plus per-style fonts:

```
[Controls]
Style=Fusion
FallbackStyle=Fusion

[Fusion]
Palette\Window=#353535
Palette\WindowText=#eeeeec
Palette\Button=#353535
Palette\Highlight=#15539e

[Basic]
Font\Family=Inter
Font\PixelSize=12
```

The keys are the roles of the `Palette` QML type. This is the closest thing Qt
has to GTK2's `gtk-color-scheme`, and it is the natural complement to `Theme.h`:
`Theme` paints the controls you own, and the conf file fixes the palette of the
controls you have not written yet — the leak the gallery below measures. Qt expects
the file to be compiled into a `.qrc`; this project deliberately avoids resources
so the UI stays live-reloadable, and `QT_QUICK_CONTROLS_CONF` points at a file
instead. `[Controls] FallbackStyle=` is also the declarative form of
`QQuickStyle::setFallbackStyle()`.

**(b) Make the token table able to carry light mode.** `Theme` exposes every
colour as `CONSTANT`. That is right for a fixed dark theme and is why the palette
cannot drift — but light mode is structurally impossible and consumers bind
without re-evaluating.

If it ever matters: drop `CONSTANT` from the colour properties, have the getters
read from a `Palette` struct, and add a setter that swaps it. No QML changes; the
bindings simply start re-evaluating. Nothing on this box will trigger the swap
automatically, so it would be a user-facing preference.

Do **not** build that on `QStyleHints::colorScheme`: it reports `Unknown` here.

### Recipe 5 — The widget vocabulary, GTK → QML

| GTK / libadwaita | Qt Quick equivalent |
|---|---|
| `GtkBox`, `GtkGrid` | `RowLayout` / `ColumnLayout` / `GridLayout` |
| `GtkHeaderBar` | `ApplicationWindow.header` + `ToolBar` |
| `GtkStack`, `GtkStackSwitcher` | `StackLayout` / `StackView`, `TabBar` |
| `GtkRevealer` | `Item` + `Behavior on implicitHeight` |
| `GtkScrolledWindow` | `ScrollView` / `Flickable` |
| `GtkListBox` | `ListView` (+ `ItemDelegate`) |
| `GtkPaned` | `SplitView` |
| `GtkNotebook` | `TabBar` + `StackLayout` |
| `GtkMenuButton` + `GtkPopover` | `ToolButton` + `Menu`, or `Popup` |
| `GtkSpinner` | `BusyIndicator` |
| `AdwActionRow` | `ItemDelegate` in a `ListView`, or `RowLayout` in a `Frame` |
| `AdwPreferencesPage` | `ScrollView` + `ColumnLayout` of rows |
| `AdwClamp` | `Item` with `Layout.maximumWidth` |
| `AdwViewSwitcher` | `TabBar` with `TabButton` |
| `AdwToast` | `Popup` + `Timer` |
| `AdwStatusPage` | centred `ColumnLayout` with an icon |

The one genuine absence is a first-party **action row** — the icon, title,
subtitle and trailing widget that carry most of libadwaita's look. It is a
20-line `ItemDelegate` and worth writing once in `qml/`.

### Recipe 6 — Window chrome that suits i3

On i3 nothing draws a title bar for you, so a client-side header bar is right —
which is what `main.qml` already does.

```qml
// Drag and resize must go through the window system, not by moving x/y.
DragHandler {
    onActiveChanged: if (active) window.startSystemMove()
}
```

`Window.startSystemMove()` and `startSystemResize(Qt.Edges)` are present in this
Qt **[verified]**. Pair them with `flags: Qt.Window | Qt.FramelessWindowHint` and
an edge resize grip.

### Recipe 7 — The polish that reads as "beautiful"

- **Motion, small and consistent.** State transitions at 100–150 ms with
  `Easing.OutCubic`; nothing above 200 ms in a tool this dense. `Behavior on
  color` for hover, `Behavior on opacity` for reveal.
- **One spacing scale, used everywhere.** `Theme.space1..6` already is one — and
  Kirigami's verified 4/8 px steps are the same shape. Finishing the job is the
  win; `DshComboBox`'s literals are the leak.
- **A focus ring everywhere, never absent.** `visualFocus` from recipe 1.
- **Typography: pick pixel sizes once.** `Theme.fontUI` / `fontData` / `fontMono`
  / `fontProse` exist; a hardcoded `font.pixelSize: 13` in one file is how a UI
  starts to look assembled.
- **Hover feedback on every interactive thing.** A control with no hover state
  reads as a picture of a control.
- **Hand-craft the focal component, keep the frame stock.** The best-looking
  examples — an itinerary's colour-coded leg cards, a clock's dial — craft one
  element and let everything around it be disciplined and ordinary. Crafting
  everything is what makes an app look assembled.

### Recipe 8 — Build a gallery app

Every component × every state × light and dark × two densities, as a separate
throwaway app. KDE ships exactly this (`kirigami-gallery`) and it is the only
reliable way to keep a themed application coherent as it grows — it catches
theme regressions far cheaper than screenshots in CI.

---

## 9. Recommendation

Keep `Theme.h`. It is GTK2's layer 1 — a token table in the Kirigami tradition,
measured rather than guessed, and it is what makes this look like a native tool.

Then two structural changes, in order:

1. **Re-base the three `Dsh*` controls on `QtQuick.Templates`.** Small, local,
   recovers keyboard and accessibility behaviour, changes no pixel. `DshButton`
   matters most.
2. **When more than a handful of controls need restyling, promote them to a
   vendored `Dsh` style** (recipe 2), with Fusion as the fallback. That is GTK2's
   `class`/`widget_class` binding and its per-widget recipes, restored — the
   point at which the app stops being "Fusion plus exceptions" and becomes a
   themed application in the GTK sense, with no stylesheet anywhere in it.

Everything else here is polish and can wait.
