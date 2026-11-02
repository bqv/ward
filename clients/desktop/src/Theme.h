#pragma once

#include <QColor>
#include <QObject>
#include <QPalette>
#include <QString>
#include <QVariantList>

/// The design system: a NATIVE DESKTOP TOOL, not a web page in a window.
///
/// This began as a `.js` `.pragma library` module, which works but was wrong twice
/// over: a script file has no place in a client whose premise is that the web
/// stack is the problem, and the user's objection to it was unambiguous. A C++
/// singleton exposed through `qmlRegisterSingletonInstance` is the native Qt
/// mechanism for the same job — one shared token table, no script, no engine.
///
/// It also fixes a real defect the script version had: `main.qml` kept a second,
/// hand-maintained copy of the palette, so `window.bg` and the module's `bg` could
/// disagree and did drift. With a single registered instance there is one source
/// and no copy to fall out of step.
///
/// The values come from how desktop applications are built, deliberately unlike
/// the web client's own tokens:
///
///   corners   0 or 2 px. A 6-8 px radius is a web idiom; there are no rounded
///             panels in a file manager, a mail client or an IDE.
///   spacing   2/4/6/8. Dense by default, with density as a feature: this surface
///             shows tool output, and every pixel of chrome is a line of it lost.
///   type      monospaced for anything the machine produced — ids, paths, counts,
///             tool output, the transcript. Proportional only for prose the model
///             wrote, where a mono face would be harder to read, not more native.
///   colour    greys carry structure; the accent marks the one interactive focus;
///             green/amber/red mean running/waiting/failed and nothing else.
class Theme : public QObject
{
    Q_OBJECT

    // ---- surfaces -----------------------------------------------------------
    // The stack runs from the transcript UP to the chrome: the reading surface
    // is the darkest thing in the window and each band of chrome is a step above
    // it, so the page recedes and the furniture comes forward.
    //
    // That is the inverse of Adwaita (whose window is lighter than its headerbar)
    // and the inverse of how this file read until 2026-10-25; the owner asked for
    // it twice — "the chat window should be dimmer than the drawer" — so `bg` and
    // `panel` exchanged roles and their values are unchanged. Every text tier
    // reads BETTER on the new base. The tiers were then lifted the same day and
    // again when `fg` went pure white (see the `---- text` block): they now
    // measure 18.9 / 12.0 / 9.0 / 6.4 on the base, 17.0 / 10.8 / 8.1 / 5.8 on
    // the chrome and 14.5 / 9.2 / 6.9 / 5.0 on a hovered row.
    //
    // The luminance steps are small because near-black compresses: content/chrome
    // is 1.11:1, the hover lift 1.17:1 on chrome and 1.30:1 on the page, and the
    // code well only 1.06:1 below the page. Hierarchy here comes from hairlines,
    // weight and the text ladder, exactly as a dense desktop tool does it. Do
    // not "fix" a boundary by lifting a surface; the layers are one darkness with
    // edges.
    //
    // What separates page from chrome is therefore the SEAM, not the fill: a
    // 1 px `border` hairline that measures 1.35:1 against the page and 1.22:1
    // against the chrome. Before the inversion those two numbers were the same
    // pair the other way round, so the edge is exactly as legible as it was; the
    // fill step alone (1.11:1) has never been enough to read as an edge, which is
    // why the seam is drawn rather than implied.
    Q_PROPERTY(QColor bg READ bg CONSTANT)
    Q_PROPERTY(QColor panel READ panel CONSTANT)
    Q_PROPERTY(QColor raised READ raised CONSTANT)
    Q_PROPERTY(QColor border READ border CONSTANT)
    Q_PROPERTY(QColor borderStrong READ borderStrong CONSTANT)

    /// Hairlines that are an ALPHA of white rather than a fixed grey, because a
    /// hairline has to sit on whichever surface it divides. The same 6% white
    /// reads correctly on the content base and on the code card below, whereas
    /// an opaque value is right on one and invisible on the other. Measured:
    /// L1 is 1.15:1 on the page, 1.17:1 on the chrome, and 1.12:1 on the code
    /// card, where it is the only thing that draws the card's edge.
    Q_PROPERTY(QColor borderL1 READ borderL1 CONSTANT)
    Q_PROPERTY(QColor borderL2 READ borderL2 CONSTANT)

    /// The context ring's TRACK, and the one rung of the web's hairline ladder
    /// this table did not need until the ring arrived: `border-l3` is
    /// `#ffffff29` (16% white) in `dsh-client-ui-theme`'s dark tokens, against
    /// the 6% and 12% above. It is not a divider here — it is the dial's
    /// unfilled arc, drawn on `raised`, where 16% white composites to #4b4b4b
    /// and reads as a track rather than as a second, competing ring. The FILL
    /// is `caption`, the tier the count beside it prints in, because the web's
    /// fill is `label-tertiary`: the ring and its number are one tone in both.
    Q_PROPERTY(QColor contextRingTrack READ contextRingTrack CONSTANT)

    /// The user turn's bubble: a hair above the content base (#111111) without
    /// becoming a panel, which is what makes a right-aligned turn read as the
    /// user's own words quoted back rather than as chrome. 1.17:1 over the base,
    /// and 16.1:1 for the prose inside it — both were preserved when the base
    /// moved, by re-solving the bubble against the new base rather than reusing
    /// the old value. The prose figure moved to 16.1 when `fg` went pure white
    /// (it was 15.0 at #f7f7f4); the 1.17:1 fill step is untouched.
    Q_PROPERTY(QColor bubble READ bubble CONSTANT)
    /// The scrim a picture is shown larger over: `bg` at 80% alpha, NOT a
    /// separate literal, so it follows the page if the page moves. Measured
    /// composited over `bg` (17,17,17): white text under it lands at #414141 —
    /// still readable as context, clearly recessed, and the picture above it is
    /// untouched. A scrim any lighter let the transcript compete with the
    /// picture; any darker turned the context into a black void.
    Q_PROPERTY(QColor scrim READ scrim CONSTANT)

    /// The payload/code surface. Deliberately DARKER than the window it sits in,
    /// so a tool payload reads as an inset well rather than a raised card; that
    /// is why it cannot be expressed as `raised` or `panel` — either would lift
    /// it out of the column and the card would stop looking like machine output.
    /// On this base the fill step can only be 1.06:1 — pure black would buy
    /// 1.11:1 and no more — so the `borderL1` stroke is what draws the card's
    /// edge (1.12:1 against the fill, 1.15:1 against the page). The fill step is
    /// half what it was and the stroke held, so the stroke is now the card. The
    /// fill is the well; the stroke is the line around it.
    Q_PROPERTY(QColor codeBlock READ codeBlock CONSTANT)

    // ---- text ---------------------------------------------------------------
    // Four rungs, and each one is a JOB rather than an amount of grey. The
    // measured ratio is the one that matters: every value below is checked
    // against the surfaces it is actually drawn on, at the sizes it is drawn at.
    // Three numbers per rung: the content base, the chrome, a hovered row.
    //
    //   fg        the agent's prose, and anything to be read first. Pure white.
    //             18.9:1 on the content base, 17.0:1 on the chrome,
    //             14.5:1 on a hovered row, 16.1:1 in the user's bubble.
    //   dim       secondary WORDS: row titles, labels, metadata, an unfolded
    //             thought, a tool payload. 12.0:1 / 10.8:1 / 9.2:1, and 12.7:1
    //             on the code card. 1.58:1 under `fg`, 1.33:1 over `caption`.
    //   caption   the 12 px tier: timestamps, a notice's caption, counts, the
    //             Enter-mode chip, a payload's IN/OUT gutter. 9.0:1 / 8.1:1 /
    //             6.9:1, 9.5:1 on the code card. 1.33:1 under `dim`, 1.39:1
    //             over `faint`.
    //   faint     MARKS ONLY: state dots, chevrons, the separator dot, lane
    //             strokes, and a disabled control's label. 6.4:1 / 5.8:1 /
    //             5.0:1. 1.39:1 under `caption`, still the quietest rung, and
    //             still a mark rather than a word.
    //
    // The inversion of 2026-10-25 moved these numbers between the first two
    // columns — every ratio the page gained, the chrome gave back — and no rung
    // fell below its floor anywhere. `faint` used to do both jobs, which is why
    // dimmed system messages were unreadable: a timestamp in it measured 1.8:1
    // on the old #353535 base, under every threshold there is, for a word.
    //
    // Then the ladder was LIFTED on the same day, because the owner could not
    // read the chat screen: "way too much is too dim in the chat screen,
    // brighten our text, retaining the differences". Every rung rose and the
    // steps were kept as steps — 17.6 : 11.1 : 8.4 : 6.0 on the content base,
    // before the white shift — each about a third to a half brighter than the
    // one below. That lift replaced a ladder of 17.6 : 7.9 : 6.1 : 4.0 — which
    // cleared every floor written in this comment and still failed in use, which
    // is the lesson: a ratio is a floor, not a target, and those floors were set
    // by the mark a rung has to draw rather than by someone reading a timestamp.
    // Hierarchy is carried by the INTERVALS between the rungs; pushing the
    // bottom rung down was the mistake.
    //
    // Then `fg` went to pure white — "make it pure white, and shift all the
    // other colours accordingly" — and every rung below it was re-solved UP by
    // the same factor, because a rung is a STEP below `fg` rather than a value
    // of its own. At #f7f7f4 the intervals were 1.58 / 1.33 / 1.41; they are
    // 1.58 / 1.33 / 1.39 at the new numbers, so only the ceiling moved. Had
    // only `fg` moved, the top step would have widened to 1.70:1 while the
    // bottom stayed put — three greys near the floor, no longer a ladder.
    //
    // Nothing here may put 12 px text below 4.5:1, and `faint` is not for words.
    Q_PROPERTY(QColor fg READ fg CONSTANT)
    Q_PROPERTY(QColor dim READ dim CONSTANT)
    Q_PROPERTY(QColor caption READ caption CONSTANT)
    Q_PROPERTY(QColor faint READ faint CONSTANT)

    // ---- meaning ------------------------------------------------------------
    // Only these carry semantic weight. The accent is deliberately desaturated: it
    // marks focus and the current selection, so it must not compete with content.
    Q_PROPERTY(QColor accent READ accent CONSTANT)
    /// The text that sits ON `accent`, and the reason it is a property rather
    /// than a plain accessor: it was declared as an ordinary getter, so QML could
    /// not see it and every accent fill had to pick its own foreground. A colour
    /// the theme defines but the UI cannot reach is how a palette grows a second,
    /// subtly different white. 7.6:1 on the accent. Since `fg` went pure white
    /// this is literally the same value — #ffffff on the blue (7.6:1) and #ffffff
    /// as the window's text (18.9:1 on the page, 17.0:1 on `panel`) — not two
    /// whites that happen to match: this window has one white, and it is used on
    /// two different fills.
    Q_PROPERTY(QColor accentFg READ accentFg CONSTANT)
    Q_PROPERTY(QColor accentDim READ accentDim CONSTANT)
    Q_PROPERTY(QColor ok READ ok CONSTANT)
    Q_PROPERTY(QColor warn READ warn CONSTANT)
    Q_PROPERTY(QColor danger READ danger CONSTANT)

    /// The state ladder's missing rung: a call that is still running is blue,
    /// and neither blue already in the table will do — `accent` is Adwaita's
    /// *selected background*, dark enough that an 8 px dot of it reads as a
    /// smudge rather than a signal, and `accentDim` is darker still. This is
    /// Adwaita's accent blue itself, and it means "in flight" and nothing else.
    Q_PROPERTY(QColor running READ running CONSTANT)

    // ---- metrics ------------------------------------------------------------
    // Named rather than inline so the whole tool tightens or loosens in one edit
    // instead of drifting panel by panel.
    Q_PROPERTY(int space1 READ space1 CONSTANT)
    Q_PROPERTY(int space2 READ space2 CONSTANT)
    Q_PROPERTY(int space3 READ space3 CONSTANT)
    Q_PROPERTY(int space4 READ space4 CONSTANT)
    Q_PROPERTY(int space5 READ space5 CONSTANT)
    Q_PROPERTY(int space6 READ space6 CONSTANT)

    Q_PROPERTY(int radius READ radius CONSTANT)
    Q_PROPERTY(int hairline READ hairline CONSTANT)
    Q_PROPERTY(int fontUI READ fontUI CONSTANT)
    Q_PROPERTY(int fontData READ fontData CONSTANT)
    Q_PROPERTY(int fontMono READ fontMono CONSTANT)
    Q_PROPERTY(int fontProse READ fontProse CONSTANT)
    Q_PROPERTY(int rowHeight READ rowHeight CONSTANT)
    Q_PROPERTY(int headerHeight READ headerHeight CONSTANT)
    /// The longest edge an inline picture may take in a transcript row, 30 ×
    /// `space6` = 480. A picture in a message is evidence inside a paragraph,
    /// not the row itself, and it is bounded on BOTH edges because a phone
    /// screenshot is portrait: 1080 × 2400 at a 480 px width would be 1 067 px
    /// tall and would swallow the answer it belongs to. At 480 the longest edge
    /// a landscape screenshot reads 480 × 128 and a portrait one 216 × 480,
    /// each preserving its aspect and each still legible as a picture.
    Q_PROPERTY(int imageMaxEdge READ imageMaxEdge CONSTANT)

    /// The context meter's ring, at the web's own geometry
    /// (`dsh-client-ui-conversation`, `skeleton/ContextMeter.js`: a 14 px
    /// viewBox, `RADIUS = 5.5`, `stroke-width: 2px`). A gauge is neither
    /// padding nor type, so it is its own metric rather than a spacing rung,
    /// but it is BUILT from the table — 2 × `space3` + `space1` — so it tightens
    /// with everything else instead of pinning a literal in QML. The stroke is
    /// one `space1`; the radius that falls out (5.5 px) is the web's.
    Q_PROPERTY(int contextRing READ contextRing CONSTANT)
    Q_PROPERTY(int contextRingStroke READ contextRingStroke CONSTANT)

    // ---- widget chrome ------------------------------------------------------
    // What makes a QtWidgets control read as a *widget* rather than a web button
    // is its bevel: the border is LIGHT on the top and left and DARK on the
    // bottom and right, as though lit from above, so the control appears milled
    // out of the surface. Fusion does this with a gradient plus two border
    // tones; on a dark theme the same geometry works with the bevel raised above
    // the button fill rather than below it, which is why these are their own
    // tokens and not derived from `border`.
    Q_PROPERTY(QColor buttonGradientTop READ buttonGradientTop CONSTANT)
    Q_PROPERTY(QColor buttonGradientBottom READ buttonGradientBottom CONSTANT)
    Q_PROPERTY(QColor buttonBorder READ buttonBorder CONSTANT)
    Q_PROPERTY(QColor buttonBorderBottom READ buttonBorderBottom CONSTANT)
    Q_PROPERTY(QColor buttonHover READ buttonHover CONSTANT)
    Q_PROPERTY(QColor buttonPressed READ buttonPressed CONSTANT)
    Q_PROPERTY(QColor fieldFill READ fieldFill CONSTANT)
    Q_PROPERTY(QColor disabledFill READ disabledFill CONSTANT)
    Q_PROPERTY(QColor insetHighlight READ insetHighlight CONSTANT)
    Q_PROPERTY(QColor dropShadow READ dropShadow CONSTANT)
    Q_PROPERTY(QColor textShadow READ textShadow CONSTANT)
    Q_PROPERTY(int controlHeight READ controlHeight CONSTANT)
    Q_PROPERTY(int fieldHeight READ fieldHeight CONSTANT)

    // ---- families -----------------------------------------------------------
    // Resolved through Qt's font database at the point of use, so these are
    // requests rather than promises; the terminal emulator refuses a family that cannot
    // supply a fixed pitch, because a terminal grid in a proportional face loses
    // its column alignment while every metric still looks plausible.
    Q_PROPERTY(QString mono READ mono CONSTANT)
    Q_PROPERTY(QString ui READ ui CONSTANT)

    // ---- terminal -----------------------------------------------------------
    Q_PROPERTY(QColor terminalFg READ terminalFg CONSTANT)
    Q_PROPERTY(QColor terminalBg READ terminalBg CONSTANT)
    Q_PROPERTY(QColor terminalCursor READ terminalCursor CONSTANT)
    Q_PROPERTY(QVariantList ansi READ ansi CONSTANT)

public:
    explicit Theme(QObject *parent = nullptr) : QObject(parent) {}

    /// One shared instance. Registered in main.cpp with
    /// qmlRegisterSingletonInstance, which is the native Qt route: no script
    /// module, no qmldir entry, and therefore nothing for a consumer to import
    /// beyond the type name itself.
    static Theme *instance();

    /// The Widgets shell's `QPalette`, derived from the tokens in this file.
    ///
    /// It exists so the shell does not need a second table of colour literals:
    /// the QML controls read the tokens through the registered singleton, and
    /// stock QWidget controls read the same tokens through this palette. The
    /// four text rungs do not all fit a `QPalette` — stock controls get `fg` for
    /// enabled text and `faint` for disabled, while custom widgets paint
    /// `dim`/`caption`/`faint` directly, which is why this is a derivation and
    /// not a replacement for the getters above.
    static QPalette palette();

    // ---- the palette, measured from Adwaita dark itself -----------------------
    // Extracted from GTK's own resource (gresource extract on libgtk-3.so.0) and
    // cross-checked with gtk_style_context_lookup_color, rather than transcribed
    // from memory — which is how the first attempt ended up with the soft blue of
    // a thousand generated web sites.
    //
    // Two of Adwaita's facts are still counter-intuitive and were worth measuring:
    //   * its WINDOW is lighter than its CHROME. Adwaita dark puts the window at
    //     #353535 and the headerbar darker still at #1e1e1e — the reverse of the
    //     web habit of a dark page under darker panels. This file kept that
    //     ORDERING until 2026-10-25; it now inverts it at the owner's request,
    //     because they read the transcript as the page and the rail as furniture.
    //     The earlier attempt at this same ordering failed for a reason that no
    //     longer applies: it was done at the Adwaita LEVEL, a 1.36:1 step with
    //     nothing drawing the seam, and the header read as a slab across the top.
    //     Near-black is different — the step is 1.11:1 and a light hairline is
    //     the edge, so the band reads as a band rather than as a slab.
    //   * its greys are neutral, not cool. Every guess here was blue-tinted.
    //
    // What is no longer Adwaita is the level, and that is deliberate. The owner
    // asked for the web client's very dark page under white text, not #eeeeec on
    // #353535. So the content base sits at #111111 and the chrome at #1c1c1c, and
    // the text ladder is rebuilt on measured ratios rather than guessed: see the
    // `---- text` block above.
    QColor bg() const { return QColor(0x11, 0x11, 0x11); }          // content base — fg 18.9:1
    QColor panel() const { return QColor(0x1c, 0x1c, 0x1c); }       // headerbar, dock, rail, popups
    QColor raised() const { return QColor(0x29, 0x29, 0x29); }      // hover / selection lift — 14.5:1
    QColor border() const { return QColor(0x2c, 0x2c, 0x2c); }      // the seam, drawn LIGHT
    QColor borderStrong() const { return QColor(0x3a, 0x3a, 0x3a); }

    /// 6% and 12% white: the web client's `border-l1` (menus, card strokes) and
    /// `border-l2` (body dividers) resolved for a dark surface. Alpha, so they
    /// followed the surfaces down without a value change.
    QColor borderL1() const { return QColor(255, 255, 255, 15); }
    QColor borderL2() const { return QColor(255, 255, 255, 31); }
    /// 16% white, the web's `border-l3`; see the token note above.
    QColor contextRingTrack() const { return QColor(255, 255, 255, 41); }

    QColor bubble() const { return QColor(0x21, 0x21, 0x23); }      // user turn — 1.17:1 over bg
    /// Derived rather than written: `bg` with 204/255 alpha. See the token note.
    QColor scrim() const
    {
        QColor colour = bg();
        colour.setAlpha(204);
        return colour;
    }
    QColor codeBlock() const { return QColor(0x08, 0x08, 0x0a); }   // payload / code — 1.06:1 under bg

    /// 18.9:1 on `bg` (the transcript), 17.0:1 on `panel`, 14.5:1 on a hovered
    /// row, 16.1:1 in a user bubble. Pure white at the owner's word — "make it
    /// pure white, and shift all the other colours accordingly" — and every rung
    /// below it was re-solved up with it, so the 1.58:1 step down to `dim` is
    /// the step the ladder already had (1.58:1 at #f7f7f4) rather than a wider
    /// one. It is deliberately the same value as `accentFg`: one white for the
    /// window, used on two different fills. It replaces #f7f7f4 (17.6:1 on the
    /// base), which in turn replaced #eeeeec on #353535 at 10.6:1 — a pair that
    /// read as grey on grey. This is the web client's white on near-black, now at
    /// the owner's word pure #fff.
    QColor fg() const { return QColor(0xff, 0xff, 0xff); }          // primary text
    /// Secondary text: 12.0:1 on `bg`, 10.8:1 on `panel`, 9.2:1 on a hovered
    /// row, 12.7:1 on the code card. 1.58:1 under `fg` and 1.33:1 over
    /// `caption` — the same two steps it held at 11.1:1 before `fg` went white,
    /// so the shift lifted this rung with the ceiling instead of stretching the
    /// step above it. It is also the tint of a row's icon and of the process
    /// chevron, so it still sits below `fg` everywhere — but it is where a tool
    /// payload and an unfolded thought are read, and it was too quiet at 7.9:1.
    QColor dim() const { return QColor(0xce, 0xce, 0xc9); }         // labels / metadata / payload
    /// The 12 px quiet-text tier: 9.0:1 on `bg`, 8.1:1 on `panel`, 6.9:1 on a
    /// hovered row, 9.5:1 on the code card. A timestamp, a notice's caption, a
    /// count, a preview — words, at 12 px, so the floor that matters is the one
    /// for words and this clears it by a wide margin rather than by 0.2. It is
    /// 1.33:1 under `dim` and 1.39:1 over `faint` (that lower step was 1.41:1;
    /// the white shift rounded it by two hundredths). A notice's caption belongs
    /// HERE and not on `faint`: the caption is a word, and painting words on the
    /// marks rung is what made system rows read as barely-there grey.
    QColor caption() const { return QColor(0xb3, 0xb3, 0xae); }     // quiet text
    /// MARKS ONLY: state dots, chevrons, the separator dot, lane strokes, and the
    /// label of a disabled control (which the WCAG text floor exempts). 6.4:1 on
    /// `bg`, 5.8:1 on `panel` and 5.0:1 on a hovered row — visible as a mark,
    /// still the quietest rung, and no longer invisible at a glance. The white
    /// shift lifted it with the rest (6.0 / 5.4 / 4.6 before it) and kept the
    /// 1.39:1 interval to `caption` (was 1.41:1), so it is still a step below the
    /// quietest WORDS. The rule that it carries no words is about the job, not
    /// about the number: no lift can earn it a word's place.
    QColor faint() const { return QColor(0x97, 0x97, 0x92); }       // marks

    QColor accent() const { return QColor(0x15, 0x53, 0x9e); }      // selected bg
    QColor accentDim() const { return QColor(0x0f, 0x3b, 0x71); }
    /// Green and amber are state marks first and words second: 5.4:1 and 6.9:1
    /// on `bg`, 4.8:1 and 6.2:1 on `panel`. Both clear 4.5:1 wherever they land.
    QColor ok() const { return QColor(0x4e, 0x9a, 0x06); }          // Adwaita success
    QColor warn() const { return QColor(0xf5, 0x79, 0x00); }        // Adwaita warning
    /// Adwaita's `@error_color` is #cc0000, and on a dark surface it is not
    /// readable: 3.21:1 on this base, 2.90:1 on the chrome and 2.08:1 on the old
    /// #353535. An error's own words were the least legible text in the window.
    /// This is libadwaita's `red-1` instead — the red libadwaita itself uses for
    /// error text on a dark surface — QUIETED at the owner's word: it was "quite
    /// strong, compared to the dim grey that system rows show". The loudness of
    /// #f66151 was its chroma, not its luminance, and the luminance is the one
    /// thing that cannot move: 4.5:1 on `raised` caps Y at 0.2748 against this
    /// value's 0.2874, a 4.4% dip that would not read as a dimming at all. So the
    /// colour was moved toward neutral IN LINEAR LIGHT with Y held, which leaves
    /// every WCAG ratio unchanged — 6.07:1 on `bg`, 5.48:1 on `panel`, 4.67:1 on
    /// a hovered row (a real danger-text surface: the trajectory panel's body and
    /// its hovered rows are `raised`), 6.43:1 on the code card — while OKLCH
    /// chroma falls from 0.186 to 0.110. It is also the diff's minus tint and the
    /// failure dots, so those moved with it. It now measures BELOW the quiet-word
    /// rungs (`caption` 9.0, `dim` 12.0), so the red is no longer the loudest
    /// word in the transcript, and it still clears its 4.5:1 floor on `raised`
    /// by 0.17. If `raised` ever lifts, this is the value to re-solve.
    QColor danger() const { return QColor(0xd1, 0x7a, 0x74); }      // quieted red-1
    /// Blue as a WORD is the tightest text in the palette: 5.0:1 on `bg` and
    /// 4.5:1 on `panel`, where the rail's "running" label and its counts sit.
    /// It clears 4.5:1 for a 12 px label, but by less than a tenth of a point;
    /// brighten it before brightening the chrome any further.
    QColor running() const { return QColor(0x35, 0x84, 0xe4); }     // Adwaita accent blue

    // A button is a GRADIENT, not a flat fill: `linear-gradient(to top, #323232
    // 2px, #353535)`. The first 2 px at the bottom are darker, which is what gives
    // the control a slight lift even though both stops are nearly the same grey.
    //
    // The Adwaita quote is the shape, not the value: #353535 was the old content
    // base and would now be the brightest thing in the window, brighter than the
    // transcript it sits beside. These are chrome controls, so when the panel
    // moved from #111111 to #1c1c1c the whole control family moved with it — each
    // value re-solved to hold the SAME measured step above its own panel
    // (#323232 on #1c1c1c is 1.33:1, where #2b2b2b on #111111 was also 1.33:1),
    // so only the level moved, not any control's prominence.
    QColor buttonGradientTop() const { return QColor(0x32, 0x32, 0x32); }     // 1.33:1 over the panel
    QColor buttonGradientBottom() const { return QColor(0x2f, 0x2f, 0x2f); }  // 1.27:1

    // The border is TWO-TONE: `border-color: #1b1b1b; border-bottom-color:
    // #070707`. A single-colour border is the thing that reads as flat, and this
    // is the detail the first attempt replaced with a light/dark bevel of my own
    // invention rather than the rule Adwaita actually states. Both tones moved up
    // with the panel and keep their steps: 1.10:1 over it and 1.07:1 under it.
    QColor buttonBorder() const { return QColor(0x24, 0x24, 0x24); }
    QColor buttonBorderBottom() const { return QColor(0x15, 0x15, 0x15); }

    QColor buttonHover() const { return QColor(0x3d, 0x3d, 0x3d); }  // 1.57:1 over the panel
    QColor buttonPressed() const { return QColor(0x2a, 0x2a, 0x2a); } // 1.19:1 — a press must read

    /// `background-color: #2d2d2d` on an entry, at 32 px minimum height — retuned
    /// to #2c2c2c so an entry still reads as its own surface against the #1c1c1c
    /// panel (1.22:1, the same step #242424 held against the old one). Text on it
    /// measures 13.0:1 for fg and 5.9:1 for the placeholder.
    QColor fieldFill() const { return QColor(0x2c, 0x2c, 0x2c); }
    QColor disabledFill() const { return QColor(0x28, 0x28, 0x28); } // 1.16:1 over the panel

    /// `box-shadow: inset 0 1px rgba(255,255,255,0.02)` — a hairline highlight on
    /// the top inner edge, and the reason an Adwaita button looks lit from above.
    QColor insetHighlight() const { return QColor(255, 255, 255, 5); }
    /// `box-shadow: 0 1px 2px rgba(0,0,0,0.07)`.
    QColor dropShadow() const { return QColor(0, 0, 0, 18); }
    /// `text-shadow: 0 -1px rgba(0,0,0,0.83)` on button labels.
    QColor textShadow() const { return QColor(0, 0, 0, 212); }

    QColor accentFg() const { return QColor(0xff, 0xff, 0xff); }    // on accent — 7.6:1

    QColor terminalFg() const { return fg(); }
    /// The terminal is an inset well inside the dock, and it has to stay DARKER
    /// than the content base it sits in: #1e1e1e on the old #353535 read as a
    /// well, on #1c1c1c it would read as a slightly LIGHTER panel. Now the base
    /// is #111111, so the well has to go below that — and near-black compresses,
    /// so the deepest step still available is 1.07:1 (#000000 would only buy
    /// 1.11:1). The 1 px `border` ring around the grid is what draws the well's
    /// edge at 1.45:1; the fill is deliberately near-void so every ANSI colour
    /// gains contrast against it.
    QColor terminalBg() const { return QColor(0x06, 0x06, 0x08); }
    QColor terminalCursor() const { return QColor(0xff, 0xff, 0xff); }

        // ---- metrics ------------------------------------------------------------
    // Named rather than inline so the whole tool tightens or loosens in one edit
    // instead of drifting panel by panel.
    //
    // The type scale was raised by 2 px across the board for readability at a
    // distance, and then one point was given back, so every rung now sits one
    // point above where it started. Every height whose job is framing one of
    // those lines took the same +2 then -1: the scale and the boxes that frame it
    // move together or the air under every line changes with the type. Measured
    // rather than guessed — QFontMetrics on this box gives the 13 px line 14.5 px
    // of height, so the 12 px line it interpolates to is 13.5 — and a 27 px row
    // gives the 12 px line the 13.5 px of air a 26 px row gave an 11 px one. The
    // rows are fixed heights on purpose: a row's height must be a leaf, never the
    // measurement of a child it also lays out.
    int space1() const { return 2; }
    int space2() const { return 4; }
    int space3() const { return 6; }
    int space4() const { return 8; }
    int space5() const { return 10; }
    int space6() const { return 16; }
    int fieldHeight() const { return 35; }       // entry: 32 px min-height + borders, +1 for the type

    int radius() const { return 5; }        // buttons and entries both state 5px
    int hairline() const { return 1; }
    int fontUI() const { return 13; }
    int fontData() const { return 12; }
    int fontMono() const { return 13; }
    int fontProse() const { return 14; }
    int rowHeight() const { return 27; }
    int headerHeight() const { return 39; }    // Adwaita's headerbar, not a 27 px toolbar
    int imageMaxEdge() const { return 30 * space6(); }   // 480 px; see the token above
    int contextRing() const { return 2 * space3() + space1(); }   // 14 px; see the token above
    int contextRingStroke() const { return space1(); }            // 2 px
    int controlHeight() const { return 29; }   // Adwaita's 24 px min-height, plus borders and air

    // ---- families -----------------------------------------------------------
    // Resolved through Qt's font database at the point of use, so these are
    // requests rather than promises; the terminal emulator refuses a family that cannot
    // supply a fixed pitch, because a terminal grid in a proportional face loses
    // its column alignment while every metric still looks plausible.
    QString mono() const { return QStringLiteral("monospace"); }
    QString ui() const { return QStringLiteral("sans-serif"); }

    /// ANSI 0-15, in the order the escape sequences number them.
    QVariantList ansi() const;
};
