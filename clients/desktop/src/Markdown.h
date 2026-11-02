#pragma once

#include <QCache>
#include <QElapsedTimer>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

/// Markdown, rendered into the HTML subset Qt's rich text actually understands.
///
/// Why not `Text.MarkdownText`: it renders five constructs and gives no control
/// over any of them. Its code spans come out unstyled, its headings ignore
/// `font-size`, and its link colour is a hard-coded blue that cannot be themed.
/// The transcript is mostly code, so the constructs it cannot style are the ones
/// that matter here.
///
/// Why the styling is inline and repetitive: Qt's rich-text subset is narrow and
/// was measured rather than assumed (see `_spike/ui-preview/md-probe*.qml`).
/// What it does honour is one `style` attribute per element with any number of
/// declarations, `background-color` on an inline `<code>`, `background-color` on
/// a block element, `font-size` on a `<p>` but NOT on an `<h1>`–`<h3>` (their own
/// sizes win), and `color` on `<a>`. So headings are emitted as sized `<p>`s, and
/// code spans as `<code style="...">` rather than `<span>` — a span's background
/// is silently dropped.
class Markdown : public QObject
{
    Q_OBJECT

public:
    static Markdown *instance();

    /// Markdown to HTML. Safe on a half-arrived string: an unterminated fence
    /// swallows the tail as code, which is what a streaming block should do, and
    /// an unclosed delimiter simply renders literally until its closer lands —
    /// except an image reference, which is a caption from the moment its `![`
    /// arrives, so a reader never sees its syntax while it streams.
    ///
    /// MEMOISED. The conversion is a function of the string and of the pictures
    /// it names: the theme is a singleton whose colours are fixed for the life of
    /// the process, but an image is resolved through the filesystem, so a path
    /// that did not exist when its row was converted stays a caption until that
    /// entry is evicted. (A growing message makes a new key on every token; a
    /// settled one is converted once.) The callers are bindings, which re-evaluate
    /// far more often than their text changes: the transcript re-runs every
    /// visible row's conversion when the window is resized, and a user bubble
    /// converts the same body twice (once for the text, once for the width
    /// measurement). A cache hit is a hash lookup; a miss is a full parse.
    Q_INVOKABLE QString toHtml(const QString &markdown) const;

    /// One decodable local picture, as the splitter found it. `source` is the
    /// fully-encoded `file:` URL a QML `Image` can load; `width`/`height` are
    /// the DISPLAY size the renderer chose (bounded by the theme's
    /// `imageMaxEdge`, aspect kept), not the file's natural size. Emitting
    /// them from the conversion is what lets a row be sized before the picture
    /// has decoded: a QML `Image` would otherwise grow the row after first
    /// layout and jump the scroll position.
    struct ImagePart
    {
        QString source;
        int width = 0;
        int height = 0;
        QString alt;
    };

    /// The body as an ordered list of runs, for a QML `Column` of `Text` and
    /// `Image` items. Every part is a QVariantMap:
    ///
    ///   { kind: "html",  html }                       — rich text to draw
    ///   { kind: "image", source, width, height, alt } — a local picture
    ///
    /// A body with no decodable local picture is ONE `html` part whose text is
    /// exactly `toHtml()`'s output, so nothing that renders today changes. A
    /// relative path, a remote URL, a missing file and a file that does not
    /// decode all stay captions inside that html run: only the pictures
    /// `imageToHtml()` could actually draw become image parts — rich text
    /// cannot draw one (Qt Quick's `Text` never loads an external `<img>`), so
    /// this is the split that lets a picture reach the screen at all.
    ///
    /// The split is a function of the same MEMOISED conversion as `toHtml()`,
    /// so a streaming row pays one parse, not two. The parts are ordered and
    /// stable per body: a token that does not add or finish an image reference
    /// only lengthens the last html run, which a `ListModel` can update in
    /// place without rebuilding the `Image` delegate underneath it.
    Q_INVOKABLE QVariantList toParts(const QString &markdown) const;

    Q_INVOKABLE QString plainHtml(const QString &text) const;

    /// Colourise one line of source with the heuristic tokenizer. Public because
    /// the same treatment is wanted for tool output.
    static QString highlight(const QString &code);

    /// How much the conversion costs, for the client log. Every conversion is
    /// timed; one that takes longer than `SlowConversionMs` is reported on its
    /// own, and the running cost is summarised once per window. An answer that
    /// streams is the case this exists for: its row is re-converted every time
    /// the text grows, so the log has to say how many conversions there were and
    /// what the worst one cost, not merely that the code "looks expensive".
    static constexpr qint64 SlowConversionMs = 8;
    static constexpr qint64 ReportWindowMs = 5000;
    /// Characters of source plus HTML held. A transcript is a few hundred KB of
    /// text in the ordinary case, so this holds all of it and still cannot grow
    /// without bound when a session is left open for days.
    static constexpr int CacheChars = 2 * 1024 * 1024;

private:
    explicit Markdown(QObject *parent = nullptr);

    /// The counters a report is built from, reset per window.
    struct Cost
    {
        QElapsedTimer window;
        qint64 conversions = 0;
        qint64 hits = 0;
        qint64 totalMs = 0;
        qint64 worstMs = 0;
        int worstChars = 0;
        bool started = false;
    };

    /// Account for one conversion and return the lines to log, if any. Called
    /// with `m_mutex` held, which is why it returns the text instead of logging
    /// it: the handler writes and flushes a file.
    QStringList account(qint64 ms, int chars, bool hit) const;

    /// One cached conversion: the HTML both entry points serve, plus the
    /// pictures found in it, in output order. Cached together so `toHtml()` and
    /// `toParts()` are one parse of one key — a row that asks for parts never
    /// re-runs the scanner, and a row that still asks for HTML (a disclosure
    /// payload, the trajectory) is unchanged.
    struct Conversion
    {
        QString html;
        QList<ImagePart> images;
    };

    /// Look up or perform the conversion, account for it, and return it. The
    /// entry point behind both `toHtml()` and `toParts()`.
    Conversion conversion(const QString &markdown) const;

    /// Guarded because a conversion is a CPU-bound call a future caller could
    /// make from a worker; the QML bindings that use it today are all on the GUI
    /// thread, where an uncontended QMutex costs nothing beside a parse.
    mutable QMutex m_mutex;
    mutable QCache<QString, Conversion> m_cache;
    mutable Cost m_cost;
    /// The document last converted, so that a *growing* one — an answer arriving
    /// token by token — supersedes its own previous entry instead of leaving a
    /// whole history of prefixes in the cache. Every one of those prefixes is
    /// unreachable the moment the next arrives, and a long answer would otherwise
    /// fill the budget with them and evict the settled rows the cache exists for.
    mutable QString m_lastDocument;
};