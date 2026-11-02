#include "Theme.h"

#include <QVariantList>

// Registered once with qmlRegisterSingletonInstance, so this function is only the
// lazy construction path and not a factory the engine calls per component.
namespace {
Theme *g_theme = nullptr;
}

Theme *Theme::instance()
{
    if (!g_theme)
        g_theme = new Theme;
    return g_theme;
}

/// The Widgets shell's palette, derived from the tokens in Theme.h rather than
/// from a second table of literals: the header is the single source of colour,
/// and the QML controls and the QWidget controls are two consumers of it, not two
/// definitions (plan §5, stage 1).
///
/// Mapping notes, because the fit is not exact:
///   * `Window` is the chrome (`panel`) and `Base` is an entry's own surface
///     (`fieldFill`); a QWidget top-level is chrome, while the reading surface is
///     a pane rather than a window.
///   * `fg` is the one text rung a QPalette has, so it fills every enabled text
///     role. `caption` goes to `PlaceholderText`, `faint` to the disabled text
///     roles — the only two lower rungs with a role at all. `dim` has none and
///     stays a token custom widgets paint with.
///   * the two-tone button family is flattened to `raised` here: Fusion draws its
///     own gradient from the style, and a half-copied bevel in a palette would be
///     a third visual system rather than the same one.
QPalette Theme::palette()
{
    const Theme *tokens = instance();
    QPalette palette;

    palette.setColor(QPalette::Window, tokens->panel());
    palette.setColor(QPalette::WindowText, tokens->fg());
    palette.setColor(QPalette::Base, tokens->fieldFill());
    palette.setColor(QPalette::AlternateBase, tokens->raised());
    palette.setColor(QPalette::Text, tokens->fg());
    palette.setColor(QPalette::PlaceholderText, tokens->caption());
    palette.setColor(QPalette::Button, tokens->raised());
    palette.setColor(QPalette::ButtonText, tokens->fg());
    palette.setColor(QPalette::BrightText, tokens->accentFg());
    palette.setColor(QPalette::ToolTipBase, tokens->raised());
    palette.setColor(QPalette::ToolTipText, tokens->fg());
    palette.setColor(QPalette::Link, tokens->accent());
    palette.setColor(QPalette::LinkVisited, tokens->accentDim());
    palette.setColor(QPalette::Highlight, tokens->accent());
    palette.setColor(QPalette::HighlightedText, tokens->accentFg());
    palette.setColor(QPalette::Light, tokens->borderL1());
    palette.setColor(QPalette::Midlight, tokens->raised());
    palette.setColor(QPalette::Mid, tokens->border());
    palette.setColor(QPalette::Dark, tokens->codeBlock());
    palette.setColor(QPalette::Shadow, tokens->scrim());

    // A disabled control is a mark, not a word: `faint` is the rung the token
    // table reserves for exactly that, and the fill steps down to the disabled
    // surface rather than to a lighter grey invented here.
    palette.setColor(QPalette::Disabled, QPalette::Text, tokens->faint());
    palette.setColor(QPalette::Disabled, QPalette::WindowText, tokens->faint());
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, tokens->faint());
    palette.setColor(QPalette::Disabled, QPalette::Base, tokens->disabledFill());
    palette.setColor(QPalette::Disabled, QPalette::Button, tokens->disabledFill());
    palette.setColor(QPalette::Disabled, QPalette::Highlight, tokens->borderStrong());
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, tokens->faint());

    return palette;
}

/// ANSI 0-15, in the order the escape sequences number them. The normal half is
/// desaturated so a colour-saturated `ls` does not shout; the bright half stays
/// legible on the dark surface, which is what coloured output is for.
QVariantList Theme::ansi() const
{
    return {
        QColor(0x3b, 0x40, 0x48),   // black
        QColor(0xc0, 0x5a, 0x5a),   // red
        QColor(0x7f, 0xa6, 0x50),   // green    — the same value as ok()
        QColor(0xc9, 0xa2, 0x27),   // yellow   — the same value as warn()
        QColor(0x6f, 0x9f, 0xd8),   // blue
        QColor(0xa9, 0x7b, 0xc4),   // magenta
        QColor(0x5a, 0xa5, 0xa5),   // cyan
        QColor(0xc8, 0xcc, 0xd4),   // white
        QColor(0x5c, 0x63, 0x70),   // bright black (comments, `ls` metadata)
        QColor(0xe0, 0x73, 0x73),   // bright red
        QColor(0x9c, 0xc4, 0x6a),   // bright green
        QColor(0xe0, 0xbc, 0x4a),   // bright yellow
        QColor(0x8c, 0xb8, 0xee),   // bright blue
        QColor(0xc3, 0x96, 0xdd),   // bright magenta
        QColor(0x79, 0xc4, 0xc4),   // bright cyan
        QColor(0xf0, 0xf2, 0xf5),   // bright white
    };
}
