#include "Markdown.h"

#include "Theme.h"

#include <QCache>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>
#include <QSize>
#include <QStringList>
#include <QStringView>
#include <QUrl>
#include <QVariantMap>

#include <utility>

namespace {

// The syntax palette is the web's Shiki values verbatim, which is what the
// Android app uses too (`--shiki-token-*` in design-system.md). They are not
// Adwaita colours and that is deliberate: code is the one place in the transcript
// where a second palette is expected, and re-deriving these from the greys would
// lose the hue that makes a keyword legible as a keyword.
constexpr const char *kSynKeyword = "#faa2c1";
constexpr const char *kSynString = "#69db7c";
constexpr const char *kSynComment = "#adb5bd";
constexpr const char *kSynConstant = "#4dabf7";
constexpr const char *kSynFunction = "#b197fc";
constexpr const char *kSynPunctuation = "#ced4da";

constexpr const char *kInlineCodeBg = "#292929";   // --dsw-alias-markdown-inline-code
constexpr const char *kLink = "#679efe";

QString escapeHtml(const QString &text)
{
    QString out;
    out.reserve(text.size() + 16);
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&': out += QLatin1String("&amp;"); break;
        case '<': out += QLatin1String("&lt;"); break;
        case '>': out += QLatin1String("&gt;"); break;
        case '"': out += QLatin1String("&quot;"); break;
        default: out += c; break;
        }
    }
    return out;
}

QString coloured(const char *colour, const QString &text)
{
    return QStringLiteral("<span style=\"color:%1\">%2</span>")
        .arg(QLatin1String(colour), escapeHtml(text));
}

/// Whether `token` sits at `at` in `text`.
///
/// The scanners below test for a handful of two- and seven-character openers at
/// many positions in a line — every character of it, for a plain run. Spelled
/// `text.mid(at, token.size()) == QLatin1String(…)` that comparison allocates a
/// QString per test: a 10 KB answer costs tens of thousands of allocations per
/// conversion, and a streaming answer is converted on every render tick. The
/// characters are compared in place here instead; nothing about the accepted
/// syntax changes, because this is the same left-aligned comparison with the
/// allocation taken out of it.
bool hasAt(const QString &text, int at, QLatin1String token)
{
    if (at < 0 || at + token.size() > text.size())
        return false;
    const QChar *chars = text.constData();
    const char *wanted = token.data();
    for (int i = 0; i < token.size(); ++i) {
        if (chars[at + i] != QLatin1Char(wanted[i]))
            return false;
    }
    return true;
}

bool isFence(const QString &line, QString *marker, QString *language)
{
    const QString trimmed = line.trimmed();
    if (!trimmed.startsWith(QLatin1String("```")) && !trimmed.startsWith(QLatin1String("~~~")))
        return false;
    *marker = trimmed.left(3);
    const QString info = trimmed.mid(3).trimmed();
    *language = info;
    return true;
}

bool isRule(const QString &line)
{
    const QString t = line.trimmed();
    if (t.size() < 3)
        return false;
    const QChar c = t.at(0);
    if (c != QLatin1Char('-') && c != QLatin1Char('*') && c != QLatin1Char('_'))
        return false;
    for (const QChar x : t) {
        if (x != c && x != QLatin1Char(' '))
            return false;
    }
    return true;
}

/// `#foo` is a heading in the grammar this reproduces (no space required).
int headingLevel(const QString &line, QString *text)
{
    const QString t = line.trimmed();
    int hashes = 0;
    while (hashes < t.size() && t.at(hashes) == QLatin1Char('#'))
        ++hashes;
    if (hashes == 0 || hashes > 6)
        return 0;
    *text = t.mid(hashes).trimmed();
    return hashes;
}

/// A list marker, its depth, and whether it carries a task box.
bool listItem(const QString &line, bool *ordered, QString *text, int *depth,
              int *taskState)
{
    int spaces = 0;
    while (spaces < line.size() && line.at(spaces) == QLatin1Char(' '))
        ++spaces;
    const QString t = line.mid(spaces);

    bool ord = false;
    QString rest;
    if (t.startsWith(QLatin1String("- ")) || t.startsWith(QLatin1String("* "))
        || t.startsWith(QLatin1String("+ "))) {
        rest = t.mid(2);
    } else {
        static const QRegularExpression numbered(QStringLiteral("^(\\d+)[.)]\\s+(.*)$"));
        const auto m = numbered.match(t);
        if (!m.hasMatch())
            return false;
        ord = true;
        rest = m.captured(2);
    }

    *ordered = ord;
    // Two spaces per level, which is the rule the Android parser uses.
    *depth = spaces / 2;

    const QString trimmedRest = rest.trimmed();
    if (trimmedRest.startsWith(QLatin1String("[ ]"))) {
        *taskState = 1;   // unchecked
        rest = trimmedRest.mid(3).trimmed();
    } else if (trimmedRest.startsWith(QLatin1String("[x]"))
               || trimmedRest.startsWith(QLatin1String("[X]"))) {
        *taskState = 2;   // checked
        rest = trimmedRest.mid(3).trimmed();
    } else {
        *taskState = 0;
    }

    *text = rest;
    return true;
}

/// The `|---|` row, which is the only thing that makes a pipe line a table.
bool isTableSeparator(const QString &line)
{
    const QString t = line.trimmed();
    if (t.isEmpty() || !t.contains(QLatin1Char('-')))
        return false;
    for (const QChar c : t) {
        if (c != QLatin1Char('-') && c != QLatin1Char(':') && c != QLatin1Char('|')
            && c != QLatin1Char(' ')) {
            return false;
        }
    }
    return true;
}

/// Whether the line at `i` opens a block, and so ends whatever paragraph is being
/// collected above it.
///
/// The table case needs the lookahead: a bare `|` is a shell pipeline in prose far
/// more often than it is a table header, and treating it as one tears a paragraph
/// in half at the pipe.
bool startsBlock(const QStringList &lines, int i)
{
    const QString &line = lines.at(i);
    QString a, b;
    bool ordered = false;
    int depth = 0;
    int task = 0;
    if (isFence(line, &a, &b) || isRule(line) || headingLevel(line, &a) > 0
        || listItem(line, &ordered, &a, &depth, &task)
        || line.trimmed().startsWith(QLatin1Char('>'))) {
        return true;
    }
    return line.contains(QLatin1Char('|')) && i + 1 < lines.size()
        && isTableSeparator(lines.at(i + 1));
}

}   // namespace

Markdown::Markdown(QObject *parent)
    : QObject(parent)
{
    // A character budget rather than an entry count: the cost of a document is
    // what it holds, and one large transcript would otherwise squeeze out the
    // hundreds of small rows that are hit far more often.
    m_cache.setMaxCost(CacheChars);
}

Markdown *Markdown::instance()
{
    static Markdown markdown;
    return &markdown;
}

/// Account for one conversion, with the cache's mutex held.
///
/// Returns the lines to log rather than logging them: the handler writes and
/// flushes a file, and a slow conversion is exactly the hot path that must not
/// hold a lock across a disk write.
QStringList Markdown::account(qint64 ms, int chars, bool hit) const
{
    Cost &cost = m_cost;
    if (!cost.started) {
        cost.window.start();
        cost.started = true;
    }
    ++cost.conversions;
    if (hit)
        ++cost.hits;
    cost.totalMs += ms;
    if (ms > cost.worstMs) {
        cost.worstMs = ms;
        cost.worstChars = chars;
    }

    QStringList out;
    if (ms >= SlowConversionMs) {
        out << QStringLiteral("[markdown] conversion %1 ms for %2 chars (%3)")
                   .arg(ms).arg(chars)
                   .arg(hit ? QStringLiteral("cache hit, so the cost is other work")
                            : QStringLiteral("parsed"));
    }
    const qint64 window = cost.window.elapsed();
    if (window >= ReportWindowMs) {
        out << QStringLiteral("[markdown] %1 conversions in %2 ms: %3 cache hits,"
                              " %4 ms parsing, worst %5 ms for %6 chars")
                   .arg(cost.conversions).arg(window).arg(cost.hits)
                   .arg(cost.totalMs).arg(cost.worstMs).arg(cost.worstChars);
        cost = Cost{};
    }
    return out;
}

// ---------------------------------------------------------------------------
// the heuristic tokenizer
// ---------------------------------------------------------------------------

QString Markdown::highlight(const QString &code)
{
    static const QSet<QString> keywords = {
        QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("null"),
        QStringLiteral("nil"), QStringLiteral("None"), QStringLiteral("True"),
        QStringLiteral("False"), QStringLiteral("if"), QStringLiteral("else"),
        QStringLiteral("elif"), QStringLiteral("for"), QStringLiteral("while"),
        QStringLiteral("do"), QStringLiteral("done"), QStringLiteral("then"),
        QStringLiteral("fi"), QStringLiteral("esac"), QStringLiteral("in"),
        QStringLiteral("return"), QStringLiteral("def"), QStringLiteral("fn"),
        QStringLiteral("func"), QStringLiteral("function"), QStringLiteral("class"),
        QStringLiteral("struct"), QStringLiteral("enum"), QStringLiteral("impl"),
        QStringLiteral("trait"), QStringLiteral("interface"), QStringLiteral("type"),
        QStringLiteral("const"), QStringLiteral("let"), QStringLiteral("var"),
        QStringLiteral("val"), QStringLiteral("mut"), QStringLiteral("static"),
        QStringLiteral("public"), QStringLiteral("private"), QStringLiteral("protected"),
        QStringLiteral("import"), QStringLiteral("from"), QStringLiteral("export"),
        QStringLiteral("use"), QStringLiteral("package"), QStringLiteral("module"),
        QStringLiteral("async"), QStringLiteral("await"), QStringLiteral("yield"),
        QStringLiteral("try"), QStringLiteral("catch"), QStringLiteral("except"),
        QStringLiteral("finally"), QStringLiteral("raise"), QStringLiteral("throw"),
        QStringLiteral("new"), QStringLiteral("delete"), QStringLiteral("this"),
        QStringLiteral("self"), QStringLiteral("super"), QStringLiteral("extends"),
        QStringLiteral("implements"), QStringLiteral("with"), QStringLiteral("as"),
        QStringLiteral("is"), QStringLiteral("not"), QStringLiteral("and"),
        QStringLiteral("or"), QStringLiteral("match"), QStringLiteral("case"),
        QStringLiteral("switch"), QStringLiteral("break"), QStringLiteral("continue"),
        QStringLiteral("pass"), QStringLiteral("lambda"), QStringLiteral("int"),
        QStringLiteral("float"), QStringLiteral("str"), QStringLiteral("bool"),
        QStringLiteral("void"), QStringLiteral("char"), QStringLiteral("long"),
        QStringLiteral("unsigned"), QStringLiteral("sizeof"), QStringLiteral("echo"),
        QStringLiteral("print"), QStringLiteral("local"), QStringLiteral("export"),
        QStringLiteral("source"), QStringLiteral("set"), QStringLiteral("unset"),
        QStringLiteral("printf"), QStringLiteral("read"), QStringLiteral("cd"),
        QStringLiteral("exit"), QStringLiteral("shift"), QStringLiteral("case"),
    };

    QString out;
    const QStringList lines = code.split(QLatin1Char('\n'));
    for (int l = 0; l < lines.size(); ++l) {
        if (l > 0)
            out += QLatin1Char('\n');

        const QString &line = lines.at(l);
        int i = 0;
        // Where the line's first non-space character is, which is what decides
        // whether a `#` opens a comment.
        int firstNonSpace = 0;
        while (firstNonSpace < line.size() && line.at(firstNonSpace).isSpace())
            ++firstNonSpace;

        while (i < line.size()) {
            // Comment: to end of line. Checked before everything else, so a quote
            // inside a comment stays a comment. Tested in place rather than on a
            // copy of the line's tail: that copy was taken once per token, which
            // made highlighting one line quadratic in its own length — and a
            // fenced block is re-highlighted on every conversion while the answer
            // streams.
            if (hasAt(line, i, QLatin1String("//")) || hasAt(line, i, QLatin1String("# "))
                || hasAt(line, i, QLatin1String("-- "))
                || (i == firstNonSpace && line.at(i) == QLatin1Char('#')
                    && !hasAt(line, i, QLatin1String("#!")))) {
                out += coloured(kSynComment, line.mid(i));
                break;
            }

            const QChar c = line.at(i);

            // String literal: to the matching quote, honouring a backslash escape.
            if (c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`')) {
                int end = i + 1;
                while (end < line.size()) {
                    if (line.at(end) == QLatin1Char('\\')) {
                        end += 2;
                        continue;
                    }
                    if (line.at(end) == c) {
                        ++end;
                        break;
                    }
                    ++end;
                }
                out += coloured(kSynString, line.mid(i, end - i));
                i = end;
                continue;
            }

            // Number: a run of identifier-ish characters, as long as it does not
            // begin inside a word.
            if (c.isDigit() && (i == 0 || !line.at(i - 1).isLetterOrNumber())) {
                int end = i;
                while (end < line.size()
                       && (line.at(end).isLetterOrNumber() || line.at(end) == QLatin1Char('_')
                           || line.at(end) == QLatin1Char('.'))) {
                    ++end;
                }
                out += coloured(kSynConstant, line.mid(i, end - i));
                i = end;
                continue;
            }

            if (c.isLetter() || c == QLatin1Char('_')) {
                int end = i;
                while (end < line.size()
                       && (line.at(end).isLetterOrNumber() || line.at(end) == QLatin1Char('_'))) {
                    ++end;
                }
                const QString word = line.mid(i, end - i);
                int after = end;
                while (after < line.size() && line.at(after) == QLatin1Char(' '))
                    ++after;
                if (keywords.contains(word))
                    out += coloured(kSynKeyword, word);
                else if (after < line.size() && line.at(after) == QLatin1Char('('))
                    out += coloured(kSynFunction, word);
                else
                    out += escapeHtml(word);
                i = end;
                continue;
            }

            if (c.isSpace()) {
                out += c;
                ++i;
                continue;
            }

            out += coloured(kSynPunctuation, QString(c));
            ++i;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// the inline scanner
// ---------------------------------------------------------------------------

namespace {

/// Everything inside one line: code spans, emphasis, images, links and bare
/// URLs. Recursive, so emphasis nests and a code span is literal to the end.
///
/// `images` receives one entry per picture the run actually draws, in output
/// order: the splitter (`toParts`) pairs its entries with the `<img>` tags the
/// scanner wrote. It is never null — an empty sink is a valid conversion that
/// simply discards them — and it is threaded through the recursion rather than
/// read from a global so that two conversions on two threads cannot mix.
QString inlineMarkup(const QString &text, QList<Markdown::ImagePart> *images);

// ---- a picture, or an honest caption --------------------------------------
//
// `![alt](target)` used to reach the reader as `!` glued to a link label: the
// `[` was claimed by the link case below, so an assistant that wrote an image
// got "!header after the fix" on the screen and no picture. Markdown has no
// image construct in this renderer — the only way to paint one is an `<img>` in
// the rich text the transcript already is — and Qt's importer is unforgiving
// about a bad one. Measured with a QTextDocument built from this exact HTML
// (Qt 6.11.2, 640 px column):
//
//  * a local absolute path and a `file://` URL both resolve to the image: the
//    resource is valid and the QImage decodes. A path containing a space and a
//    `#` loaded in both its raw and its percent-encoded form; this emits the
//    encoded URL, because that is the form that survives those characters;
//  * `width="480"` alone preserves the aspect ratio — a 900 × 240 source gave a
//    128 px block — but only because Qt derives the height from the picture;
//  * a MISSING file and an `https://` source both leave no resource behind and
//    give the block a height of 33 554 432 px (1 << 25), which overflows the
//    document's own height to a negative number. A dead `<img>` is not a broken
//    icon in Qt's rich text; it is a hole in the layout. So the decode happens
//    here, before any `<img>` is written, and whatever does not decode becomes
//    a caption instead.
//
// The caption is on the quiet tier at the theme's 11 px size — the probe
// measured the span's size and colour reaching the fragment — and is prefixed
// `[image]`, so a reader knows a picture was meant instead of reading a path as
// prose. It carries the alt text where there is one and the target beside it,
// elided: a path is the evidence of what failed, while a data: URI is megabytes
// of base64 that no caption should print. A remote URL is never fetched: a
// transcript row is not a network client.

/// How much of one failed reference a caption shows. Long enough for an
/// ordinary path, short enough that a data: URI cannot become the row.
constexpr int MaxCaptionChars = 120;

QString elidedText(const QString &text, int max)
{
    if (text.size() <= max)
        return text;
    return text.left(max - 1) + QChar(0x2026);
}

/// Strip the angle brackets Markdown allows around a target with spaces in it.
QString bareTarget(const QString &target)
{
    QString text = target.trimmed();
    if (text.size() >= 2 && text.startsWith(QLatin1Char('<'))
        && text.endsWith(QLatin1Char('>'))) {
        text = text.mid(1, text.size() - 2).trimmed();
    }
    return text;
}

/// The local file an image reference points at, if it points at one at all.
///
/// Only an absolute path or a `file:` URL qualifies. A relative path would need
/// a base directory a message does not carry, and an `http(s)` URL is the
/// network this client deliberately does not touch from a transcript row; both
/// are captions rather than pictures.
QString localImagePath(const QString &target)
{
    const QString text = bareTarget(target);
    if (text.isEmpty())
        return {};
    if (text.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        const QUrl url(text);
        return url.isLocalFile() ? url.toLocalFile() : QString();
    }
    if (text.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        || text.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        return {};
    }
    return QDir::isAbsolutePath(text) ? text : QString();
}

/// The decoded size of a picture this client can actually show, or an empty size
/// when it cannot. The decode is not a formality — see the block above: a bad
/// `<img>` is a 33-million-pixel hole, not a fallback glyph.
///
/// Memoised on the file's identity, because a streaming row is converted again
/// for every token and a row that already names a picture would otherwise
/// decode that picture again each time. Measured at 30–53 ms for the two test
/// screenshots, which is several frames of the GUI thread per token; the cache
/// turns every conversion after the first into a stat and a hash lookup. The
/// key carries size and modification time, so a file written over is re-read,
/// and an empty result is cached too: a file that is not an image is not worth
/// opening again either.
QSize imageSize(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable())
        return {};

    static QMutex mutex;
    static QCache<QString, QSize> cache(64);
    const QString key = path + QLatin1Char('\n') + QString::number(info.size())
        + QLatin1Char(':') + QString::number(info.lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker locker(&mutex);
        if (const QSize *hit = cache.object(key))
            return *hit;
    }

    QSize size;
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    if (reader.canRead()) {
        const QImage image = reader.read();
        if (!image.isNull())
            size = image.size();
    }

    QMutexLocker locker(&mutex);
    cache.insert(key, new QSize(size), 1);
    return size;
}

/// The target as a caption may show it: a `file:` URL becomes the path a reader
/// can act on, and a data: URI is named rather than pasted.
QString captionTarget(const QString &target)
{
    QString text = bareTarget(target);
    if (text.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        const QUrl url(text);
        // A fragment or query in a file: URL is not part of the path, and
        // replacing the URL with `toLocalFile()` would silently drop it from the
        // caption. Those are rare enough to show as the writer wrote them.
        if (url.isLocalFile() && !url.hasFragment() && !url.hasQuery())
            text = url.toLocalFile();
    } else if (text.startsWith(QLatin1String("data:"), Qt::CaseInsensitive)) {
        text = QStringLiteral("embedded image");
    }
    return elidedText(text, MaxCaptionChars);
}

QString imageCaption(const Theme *theme, const QString &alt, const QString &target)
{
    const QString altText = elidedText(alt.trimmed(), MaxCaptionChars);
    const QString targetText = captionTarget(target);

    QString text = QStringLiteral("[image]");
    if (!altText.isEmpty())
        text += QLatin1Char(' ') + altText;
    if (!targetText.isEmpty() && targetText != altText)
        text += QStringLiteral(" \u2014 ") + targetText;
    if (altText.isEmpty() && targetText.isEmpty())
        text += QStringLiteral(" (no description)");

    return QStringLiteral("<span style=\"color:%1; font-size:%2px\">%3</span>")
        .arg(theme->caption().name(), QString::number(theme->fontData()),
             escapeHtml(text));
}

/// One `![alt](target)`, as HTML a reader can understand, whatever the target.
/// A picture that decodes is also recorded in `images`, with the display size
/// this function just chose, so the splitter can hand a QML `Image` the exact
/// box the row must reserve.
QString imageToHtml(const QString &alt, const QString &target,
                    QList<Markdown::ImagePart> *images)
{
    const Theme *theme = Theme::instance();
    const QString path = localImagePath(target);
    const QSize natural = path.isEmpty() ? QSize() : imageSize(path);
    if (!natural.isEmpty()) {
        const int maxEdge = theme->imageMaxEdge();
        const QSize shown = (natural.width() > maxEdge || natural.height() > maxEdge)
                                ? natural.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio)
                                : natural;

        // Both dimensions are written from the decoded picture rather than
        // leaving Qt to infer one: the bound is on the longest edge, so a
        // portrait screenshot must lose width as well as height, and deriving
        // that inside the layout would be a second opinion about the aspect.
        const QString src = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
        Markdown::ImagePart part;
        part.source = src;
        part.width = shown.width();
        part.height = shown.height();
        part.alt = alt.trimmed();
        images->append(part);

        QString img = QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%3\"")
                          .arg(escapeHtml(src), QString::number(shown.width()),
                               QString::number(shown.height()));
        if (!alt.trimmed().isEmpty())
            img += QStringLiteral(" alt=\"%1\"").arg(escapeHtml(alt.trimmed()));
        return img + QLatin1Char('>');
    }
    return imageCaption(theme, alt, target);
}

/// Where `![alt](target)`'s target ends. Parentheses nest, because a screenshot
/// called `Screen Shot (1).png` is common and stopping at the first `)` would
/// quietly resolve a path that does not exist. -1 means the closer has not
/// arrived yet, which is a message still streaming.
int imageTargetEnd(const QString &text, int start)
{
    int depth = 0;
    for (int j = start; j < text.size(); ++j) {
        const QChar c = text.at(j);
        if (c == QLatin1Char('(')) {
            ++depth;
        } else if (c == QLatin1Char(')')) {
            if (depth == 0)
                return j;
            --depth;
        }
    }
    return -1;
}

QString linkTo(const QString &label, const QString &url)
{
    QString href = url.trimmed();
    if (href.startsWith(QLatin1String("www.")))
        href.prepend(QLatin1String("https://"));
    return QStringLiteral("<a href=\"%1\" style=\"color:%2\">%3</a>")
        .arg(escapeHtml(href), QLatin1String(kLink), label);
}

QString inlineMarkup(const QString &text, QList<Markdown::ImagePart> *images)
{
    QString out;
    int i = 0;
    const int n = text.size();

    while (i < n) {
        const QChar c = text.at(i);

        // Code span: literal to the closing backtick, and never re-scanned.
        if (c == QLatin1Char('`')) {
            const int close = text.indexOf(QLatin1Char('`'), i + 1);
            if (close > i) {
                const QString body = text.mid(i + 1, close - i - 1);
                // A span that is exactly one URL is a link, not code — the same
                // rule the Android renderer applies.
                if (body.startsWith(QLatin1String("http://"))
                    || body.startsWith(QLatin1String("https://"))) {
                    out += linkTo(escapeHtml(body), body);
                } else {
                    out += QStringLiteral("<code style=\"background-color:%1\">%2</code>")
                               .arg(QLatin1String(kInlineCodeBg), escapeHtml(body));
                }
                i = close + 1;
                continue;
            }
        }

        // Strong before emphasis, or `**x**` would be read as two italics.
        //
        // The delimiter is compared in place and the closer searched for by its
        // literal, rather than both going through `text.mid(i, 2)`: the scanner
        // reaches here once per character of every line, and the substring form
        // allocated two QStrings per test — the largest per-character cost in a
        // conversion after the parse itself.
        const bool strong = hasAt(text, i, QLatin1String("**"));
        if (strong || hasAt(text, i, QLatin1String("__"))) {
            const int close = text.indexOf(strong ? QLatin1String("**")
                                                  : QLatin1String("__"), i + 2);
            if (close > i + 2) {
                out += QStringLiteral("<b>%1</b>")
                           .arg(inlineMarkup(text.mid(i + 2, close - i - 2), images));
                i = close + 2;
                continue;
            }
        }

        if (hasAt(text, i, QLatin1String("~~"))) {
            const int close = text.indexOf(QLatin1String("~~"), i + 2);
            if (close > i + 2) {
                out += QStringLiteral("<s>%1</s>")
                           .arg(inlineMarkup(text.mid(i + 2, close - i - 2), images));
                i = close + 2;
                continue;
            }
        }

        if (c == QLatin1Char('*') || c == QLatin1Char('_')) {
            const int close = text.indexOf(c, i + 1);
            // No space just inside, which is what stops `a * b * c` becoming
            // emphasis.
            if (close > i + 1 && !text.at(i + 1).isSpace()
                && !text.at(close - 1).isSpace()) {
                out += QStringLiteral("<i>%1</i>")
                           .arg(inlineMarkup(text.mid(i + 1, close - i - 1), images));
                i = close + 1;
                continue;
            }
        }

        // `![alt](target)` — before the `[label](url)` case below, which is what
        // used to claim the bracket and print `!` plus a link. A reference still
        // arriving (no closing `)` yet) is taken to the end of the line: it is a
        // caption for the moment and a picture once the closer lands, rather
        // than showing its syntax to someone watching the answer stream in.
        if (c == QLatin1Char('!') && i + 1 < n && text.at(i + 1) == QLatin1Char('[')) {
            const int closeLabel = text.indexOf(QLatin1Char(']'), i + 2);
            if (closeLabel > i + 1) {
                const QString alt = text.mid(i + 2, closeLabel - i - 2);
                if (hasAt(text, closeLabel, QLatin1String("]("))) {
                    const int targetStart = closeLabel + 2;
                    const int targetEnd = imageTargetEnd(text, targetStart);
                    const int end = targetEnd < 0 ? n : targetEnd;
                    out += imageToHtml(alt, text.mid(targetStart, end - targetStart), images);
                    i = targetEnd < 0 ? n : targetEnd + 1;
                } else {
                    // `![alt]` with no target: a reference to a picture whose
                    // address is missing, not prose with an exclamation in it.
                    out += imageToHtml(alt, QString(), images);
                    i = closeLabel + 1;
                }
                continue;
            }
            // The alt text has not closed yet, so the rest of the line is all
            // that has arrived. A caption is readable; `![header after the` is
            // not.
            out += imageToHtml(text.mid(i + 2), QString(), images);
            i = n;
            continue;
        }

        // `[label](url)`
        if (c == QLatin1Char('[')) {
            const int closeLabel = text.indexOf(QLatin1Char(']'), i + 1);
            if (closeLabel > i && hasAt(text, closeLabel, QLatin1String("]("))) {
                const int closeUrl = text.indexOf(QLatin1Char(')'), closeLabel + 2);
                if (closeUrl > closeLabel) {
                    out += linkTo(inlineMarkup(text.mid(i + 1, closeLabel - i - 1), images),
                                  text.mid(closeLabel + 2, closeUrl - closeLabel - 2));
                    i = closeUrl + 1;
                    continue;
                }
            }
        }

        // A bare URL, but only at a word boundary — `xhttps://` is not a link.
        if ((hasAt(text, i, QLatin1String("http://")) || hasAt(text, i, QLatin1String("https://")))
            && (i == 0 || !text.at(i - 1).isLetterOrNumber())) {
            int end = i;
            while (end < n && !text.at(end).isSpace() && text.at(end) != QLatin1Char('<')
                   && text.at(end) != QLatin1Char('"') && text.at(end) != QLatin1Char('`')) {
                ++end;
            }
            // Trailing sentence punctuation belongs to the sentence.
            while (end > i && (text.at(end - 1) == QLatin1Char('.')
                               || text.at(end - 1) == QLatin1Char(',')
                               || text.at(end - 1) == QLatin1Char(';')
                               || text.at(end - 1) == QLatin1Char(':'))) {
                --end;
            }
            const QString url = text.mid(i, end - i);
            out += linkTo(escapeHtml(url), url);
            i = end;
            continue;
        }

        // Plain text runs to the next character that could start something.
        int end = i + 1;
        while (end < n && text.at(end) != QLatin1Char('`') && text.at(end) != QLatin1Char('*')
               && text.at(end) != QLatin1Char('_') && text.at(end) != QLatin1Char('[')
               && text.at(end) != QLatin1Char('~') && text.at(end) != QLatin1Char('h')
               && text.at(end) != QLatin1Char('!')) {
            ++end;
        }
        out += escapeHtml(text.mid(i, end - i));
        i = end;
    }
    return out;
}

}   // namespace

// ---------------------------------------------------------------------------
// the block parser
// ---------------------------------------------------------------------------

namespace {

/// The conversion itself, cache-free, so that the memoisation in
/// `conversion()` is visibly a wrapper and cannot be mistaken for part of the
/// parser. The picture sink is threaded through so `toParts()` gets the same
/// conversion's pictures, with the exact display size this pass chose.
QString renderToHtml(const QString &markdown, QList<Markdown::ImagePart> *images)
{
    const Theme *theme = Theme::instance();
    const QString bodyColour = theme->fg().name();
    const QString codeBg = theme->codeBlock().name();
    const QString quoteBg = theme->bubble().name();
    const QString dimColour = theme->dim().name();

    const QStringList lines = markdown.split(QLatin1Char('\n'));
    QString out;
    int i = 0;

    while (i < lines.size()) {
        const QString line = lines.at(i);

        // ---- fenced code ------------------------------------------------
        QString marker, language;
        if (isFence(line, &marker, &language)) {
            QStringList body;
            ++i;
            while (i < lines.size() && !lines.at(i).trimmed().startsWith(marker)) {
                body.append(lines.at(i));
                ++i;
            }
            if (i < lines.size())
                ++i;   // the closing fence

            // An unterminated fence swallows the tail, which is what a block still
            // streaming in should look like: code, rather than prose that
            // re-flows the moment the closer arrives.
            //
            // The card is a one-cell table, not `padding` on the `<pre>`: Qt's
            // rich text has no box model, and a CSS `padding` declaration on a
            // block is parsed and then dropped — it is applied to table cells
            // only (`QTextHtmlImporter::processBlockNode`). `cellpadding` is what
            // actually insets the cell's content, and the tint is the TABLE's
            // `bgcolor`, painted over the frame rect that contains that padding
            // (`drawFrameDecoration`). It cannot be the `<td>`'s `bgcolor`: a
            // cell's background lands on the block char format, so it would tint
            // the code lines only and leave the new inset unpainted — the text
            // would still sit flush against the tint.
            //
            // Measured, not assumed (QTextDocument probe, 640 px and 900 px
            // columns — the same importer and layout a QML `Text` uses):
            //
            //  * `cellpadding="8"` did inset 8 px, but the card was 100% of the
            //    column only while the widest code line fit inside it. `<pre>`
            //    without `white-space` is laid out NON-BREAKING, so a long line
            //    becomes the table's minimum width: at a 640 px column a
            //    120-character line made the card 952 px, 149% of the column.
            //    The tint then ended 8 px past the last glyph on every side —
            //    "the background ends at exactly the character edge". The card
            //    was not shrinking to the text in a CSS sense; the text was
            //    stretching the card, which is the same thing on screen.
            //
            //  * `white-space:pre-wrap` collapses that minimum, so `width=100%`
            //    wins and the card spans the column (measured: card 640 of a 640
            //    column, last glyph ending 16 px short of the edge). Long lines
            //    now wrap inside the card instead of overflowing the row. Only
            //    the `<pre>` wraps: inline code spans and markdown tables are
            //    untouched.
            //
            //  * 16 px (theme `space6`) measured 16 px on all four sides. 8 px
            //    was invisible; nothing smaller than this buys the band.
            //
            // `margin:0` on the `<pre>` cancels the 12 px the importer gives
            // every `<pre>`, which would otherwise sit inside the card and make
            // the vertical inset 28 px against 16 beside.
            out += QStringLiteral("<table border=\"0\" cellspacing=\"0\" cellpadding=\"%1\""
                                  " width=\"100%\" bgcolor=\"%2\">"
                                  "<tr><td><pre style=\"margin:0; white-space:pre-wrap;"
                                  " color:%3\">%4</pre>"
                                  "</td></tr></table>")
                       .arg(QString::number(theme->space6()), codeBg, bodyColour,
                            Markdown::highlight(body.join(QLatin1Char('\n'))));
            continue;
        }

        // ---- headings ----------------------------------------------------
        QString headingText;
        const int level = headingLevel(line, &headingText);
        if (level > 0) {
            // 21 / 18 / 16, and h4-h6 all collapsing to h3, as the Android app
            // does. Emitted as a sized <p> because Qt's rich text ignores
            // `font-size` on an <h1>: the tag's own size wins, every time.
            const int size = level == 1 ? 21 : (level == 2 ? 18 : 16);
            out += QStringLiteral("<p style=\"font-size:%1px\"><b>%2</b></p>")
                       .arg(QString::number(size), inlineMarkup(headingText, images));
            ++i;
            continue;
        }

        // ---- rules -------------------------------------------------------
        if (isRule(line)) {
            // The one construct Qt will not let us style. `<hr>` is drawn as its
            // own bevel and the `color` declaration is ignored, so this renders
            // heavier and brighter than the hairline the web and Android draw.
            // Everything else here is themed; this cannot be, short of leaving
            // rich text for a real QML rule between rows.
            out += QStringLiteral("<hr>");
            ++i;
            continue;
        }

        // ---- blockquote --------------------------------------------------
        if (line.trimmed().startsWith(QLatin1Char('>'))) {
            QStringList quoted;
            while (i < lines.size() && lines.at(i).trimmed().startsWith(QLatin1Char('>'))) {
                quoted.append(lines.at(i).trimmed().mid(1).trimmed());
                ++i;
            }

            // A quote is a tinted block, so it gets the same one-cell card a
            // fenced block does, and for the same reason: Qt's rich text has no
            // box model, so a block's `padding` is parsed and dropped and the
            // tint would sit flush against the letters. `cellpadding` is the
            // lever it honours, and the table's `bgcolor` is the fill that
            // covers that padding (`drawFrameDecoration` paints the frame rect,
            // padding included) — a `<td bgcolor>` would tint the text lines
            // only, which is the defect this replaces.
            //
            // Measured with a QTextDocument probe at a 640 px column, Qt 6.11.2
            // (the same importer and layout a QML `Text` uses). Before: a bare
            // `<blockquote>` painted NO background at all — every pixel around
            // the quote was the page colour — and carried only 40 px of left and
            // right margin, so the text ran from 40 to 600 of a 640 column.
            // There was no band to inset, and nothing in this file tinted. The
            // tint here is new, from `bubble()`, the token whose stated job is
            // quoted words, 1.17:1 over the page and distinct from the code
            // well's #08080a and inline code's raised #292929.
            //
            // After, same probe: tint 640 x 86 at x=0 for a two-line quote, its
            // content box 608 x 54 at (16,16) — 16 px (`space6`) of padding on
            // all four sides — and the first glyph ink at x=16, 18 px below the
            // tint's top edge (the 2 px is the line box's ascent above the
            // letters). The card is `width=100%` like the code card, so the band
            // runs to the column edge rather than to the last letter.
            out += QStringLiteral("<table border=\"0\" cellspacing=\"0\" cellpadding=\"%1\""
                                  " width=\"100%\" bgcolor=\"%2\">"
                                  "<tr><td>%3</td></tr></table>")
                       .arg(QString::number(theme->space6()), quoteBg,
                            inlineMarkup(quoted.join(QLatin1Char(' ')), images));
            continue;
        }

        // ---- tables ------------------------------------------------------
        if (line.contains(QLatin1Char('|')) && i + 1 < lines.size()
            && isTableSeparator(lines.at(i + 1))) {
            auto cells = [](const QString &row) {
                QStringList parts = row.trimmed().split(QLatin1Char('|'));
                if (!parts.isEmpty() && parts.first().trimmed().isEmpty())
                    parts.removeFirst();
                if (!parts.isEmpty() && parts.last().trimmed().isEmpty())
                    parts.removeLast();
                for (QString &p : parts)
                    p = p.trimmed();
                return parts;
            };

            const QStringList header = cells(lines.at(i));
            i += 2;   // header and separator
            QString rows;
            while (i < lines.size() && lines.at(i).contains(QLatin1Char('|'))) {
                rows += QLatin1String("<tr>");
                for (const QString &cell : cells(lines.at(i)))
                    rows += QStringLiteral("<td align=\"left\" valign=\"top\">%1</td>")
                                .arg(inlineMarkup(cell, images));
                rows += QLatin1String("</tr>");
                ++i;
            }

            // A header row with no words in it is not a header. Markdown needs
            // some row above the separator, so a writer who wants columns but
            // no column names writes `| | |` — which is how the status table
            // that exposed this was written. Qt paints a cell's `bgcolor`
            // whatever the cell contains, so that row came out as a
            // full-width dark band with no letters: the "invisible header
            // text" was a row whose text never existed, and the band was the
            // whole of it. Measured with a QTextDocument and a QQuickText
            // probe (Qt 6.11.2, sans 14 px, lineHeight 1.7): a named
            // `<th bgcolor>` puts 291 px of glyph ink inside its band, so
            // neither the tag, the background nor the inherited colour is at
            // fault; the same table with empty cells paints the band and
            // nothing else. Only a wholly blank header is dropped — one with
            // any named cell is a real header and keeps its band.
            const bool blankHeader = header.join(QString()).trimmed().isEmpty();
            if (blankHeader) {
                if (!rows.isEmpty())
                    out += QStringLiteral("<table border=\"0\" cellspacing=\"0\""
                                          " cellpadding=\"%1\" width=\"100%\">%2</table>")
                               .arg(QString::number(theme->space6()), rows);
                continue;
            }

            QString head;
            const QString headBg = theme->raised().name();
            for (const QString &cell : header)
                head += QStringLiteral("<th bgcolor=\"%2\" align=\"left\">%1</th>")
                            .arg(inlineMarkup(cell, images), headBg);

            // No grid. A border on every cell turns a three-column table into a
            // spreadsheet, and the hairline rules are what the web client uses:
            // the header carries a surface, the body is open text.
            //
            // `bgcolor` rather than a `style` rule: Qt's rich text implements the
            // HTML 3.2 attribute reliably and the CSS subset unevenly. The
            // surface is `raised`, the token for a lift above the page, rather
            // than a hard-coded grey: the old literal here had the value
            // `raised` used to carry, and a copy drifts from the theme the
            // moment the theme moves.
            //
            // `cellpadding` is `space6`, the code card's inset. At 6 px — a
            // third of it — a table read as a denser, tighter block than the
            // code above it. Measured at a 640 px column with three columns: the
            // header's letters sat 6 px from the band's left edge and 9 px below
            // its top, and the row pitch was 39 px. At 16 those are 16 and 19,
            // and the pitch is 59. The table's own structure — one row per
            // markdown row, one cell per column, `width=100%` — is unchanged,
            // and `white-space` is not touched here, so a long unbroken cell can
            // still push a column wide.
            out += QStringLiteral("<table border=\"0\" cellspacing=\"0\" cellpadding=\"%1\""
                                  " width=\"100%\"><tr>%2</tr>%3</table>")
                       .arg(QString::number(theme->space6()), head, rows);
            continue;
        }

        // ---- lists -------------------------------------------------------
        bool ordered = false, probeOrdered = false;
        QString itemText, probeText;
        int depth = 0, probeDepth = 0, task = 0, probeTask = 0;
        if (listItem(line, &ordered, &itemText, &depth, &task)) {
            QString items;
            int openDepth = 0;
            const bool listOrdered = ordered;

            while (i < lines.size()
                   && listItem(lines.at(i), &probeOrdered, &probeText, &probeDepth, &probeTask)) {
                // Nesting is expressed by opening and closing sublists rather than
                // by indenting a flat list, because Qt lays out a nested <ul>
                // itself and a hand-made indent would not wrap.
                while (openDepth < probeDepth) {
                    items += listOrdered ? QLatin1String("<ol>") : QLatin1String("<ul>");
                    ++openDepth;
                }
                while (openDepth > probeDepth) {
                    items += listOrdered ? QLatin1String("</ol>") : QLatin1String("</ul>");
                    --openDepth;
                }

                QString marker;
                if (probeTask == 2)
                    marker = QStringLiteral("<span style=\"color:%1\">&#10003;</span> ")
                                 .arg(theme->ok().name());
                else if (probeTask == 1)
                    marker = QStringLiteral("<span style=\"color:%1\">&#9675;</span> ")
                                 .arg(dimColour);

                const QString textColour = probeTask == 2 ? dimColour : bodyColour;
                items += QStringLiteral("<li><span style=\"color:%1\">%2%3</span></li>")
                             .arg(textColour, marker, inlineMarkup(probeText, images));
                ++i;
            }
            while (openDepth > 0) {
                items += listOrdered ? QLatin1String("</ol>") : QLatin1String("</ul>");
                --openDepth;
            }

            out += listOrdered ? QStringLiteral("<ol>%1</ol>").arg(items)
                               : QStringLiteral("<ul>%1</ul>").arg(items);
            continue;
        }

        // ---- blank and paragraphs ---------------------------------------
        if (line.trimmed().isEmpty()) {
            ++i;
            continue;
        }

        QStringList paragraph;
        while (i < lines.size() && !lines.at(i).trimmed().isEmpty()
               && !startsBlock(lines, i)) {
            paragraph.append(lines.at(i).trimmed());
            ++i;
        }
        if (paragraph.isEmpty()) {
            // startsBlock() said yes but no branch claimed it — a stray pipe, say.
            paragraph.append(lines.at(i).trimmed());
            ++i;
        }
        // Soft line breaks are preserved: a single newline inside a paragraph is a
        // hard break in both the web UI and the Android app.
        //
        // The lines are converted FIRST and joined after. Joining first and
        // converting the result would be wrong twice over: inlineMarkup escapes
        // what it is given, so the separator came out as the literal text `<br/>`,
        // and a construct spanning a line break could not be seen anyway.
        QStringList rendered;
        rendered.reserve(paragraph.size());
        for (const QString &para : paragraph)
            rendered.append(inlineMarkup(para, images));
        out += QStringLiteral("<p>%1</p>").arg(rendered.join(QLatin1String("<br>")));
    }

    return out;
}

}   // namespace

// ---------------------------------------------------------------------------
// the split
// ---------------------------------------------------------------------------

namespace {

/// An html run as QML reads it. Every map carries every key — the ones a kind
/// does not use are empty — so a QML `ListModel` learns its full role set from
/// the first part and a later image run cannot silently gain a role the
/// delegate has already bound against.
QVariantMap htmlPartMap(const QString &html)
{
    QVariantMap part;
    part.insert(QStringLiteral("kind"), QStringLiteral("html"));
    part.insert(QStringLiteral("html"), html);
    part.insert(QStringLiteral("source"), QString());
    part.insert(QStringLiteral("width"), 0);
    part.insert(QStringLiteral("height"), 0);
    part.insert(QStringLiteral("alt"), QString());
    return part;
}

QVariantMap imagePartMap(const Markdown::ImagePart &image)
{
    QVariantMap part;
    part.insert(QStringLiteral("kind"), QStringLiteral("image"));
    part.insert(QStringLiteral("html"), QString());
    part.insert(QStringLiteral("source"), image.source);
    part.insert(QStringLiteral("width"), image.width);
    part.insert(QStringLiteral("height"), image.height);
    part.insert(QStringLiteral("alt"), image.alt);
    return part;
}

/// The element name of one generated tag, lower-cased. The renderer's tags are
/// its own, so no case folding or attribute quoting beyond this is needed.
QString tagName(const QString &tag)
{
    int i = 1;
    if (i < tag.size() && tag.at(i) == QLatin1Char('/'))
        ++i;
    const int start = i;
    while (i < tag.size() && (tag.at(i).isLetterOrNumber() || tag.at(i) == QLatin1Char('-')))
        ++i;
    return tag.mid(start, i - start).toLower();
}

/// Elements with no closing tag. The renderer writes `br`, `hr` and `img`; the
/// rest are here so the stack is right if a future construct adds one.
bool isVoidTag(const QString &name)
{
    static const QSet<QString> voidTags = {
        QStringLiteral("area"), QStringLiteral("base"), QStringLiteral("br"),
        QStringLiteral("col"), QStringLiteral("embed"), QStringLiteral("hr"),
        QStringLiteral("img"), QStringLiteral("input"), QStringLiteral("link"),
        QStringLiteral("meta"), QStringLiteral("param"), QStringLiteral("source"),
        QStringLiteral("track"), QStringLiteral("wbr"),
    };
    return voidTags.contains(name);
}

/// Keep the stack of tags still open at the scan point. Only the split needs it:
/// it closes them at the cut and reopens them on the far side, so each part is
/// a balanced fragment rather than a half-paragraph that Qt has to guess at.
void noteTag(const QString &tag, QStringList *open)
{
    const QString name = tagName(tag);
    if (name.isEmpty())
        return;
    if (tag.startsWith(QLatin1String("</"))) {
        for (int i = open->size() - 1; i >= 0; --i) {
            if (tagName(open->at(i)) == name) {
                open->removeAt(i);
                break;
            }
        }
        return;
    }
    if (tag.endsWith(QLatin1String("/>")) || isVoidTag(name))
        return;
    open->append(tag);
}

/// Whether a run paints anything a reader would notice. A run left with only
/// the empty wrappers a picture was split out of (`<p></p>`, an empty table
/// row) is dropped: keeping it would put a blank line above and below every
/// picture. An `<hr>` is kept — it is a mark with no letters, but it is visible.
bool hasVisibleContent(const QString &run)
{
    QString text;
    text.reserve(run.size());
    int i = 0;
    while (i < run.size()) {
        if (run.at(i) == QLatin1Char('<')) {
            const int end = run.indexOf(QLatin1Char('>'), i);
            if (end < 0)
                break;
            i = end + 1;
            continue;
        }
        text += run.at(i);
        ++i;
    }
    if (!text.trimmed().isEmpty())
        return true;
    return run.contains(QLatin1String("<hr"), Qt::CaseInsensitive);
}

/// Remove paragraphs a split left empty.
///
/// A picture is usually a paragraph of its own, so the run before it ends with
/// `<p></p>` (the paragraph that held only the picture) and the run after it
/// begins with the same. `hasVisibleContent` would drop a run made only of
/// those, but a run that also carries real text keeps them, and each would draw
/// a blank line around the picture — doubling the space the Column already puts
/// there. Only `<p>` is removed: an empty `<td>`, `<th>` or `<li>` carries the
/// table's or list's structure, and a run with an `<hr>` is kept as it is.
QString tidyRun(const QString &run)
{
    static const QRegularExpression emptyParagraph(
        QStringLiteral("<p(?:\\s[^>]*)?>\\s*</p>"));
    QString tidy = run;
    tidy.remove(emptyParagraph);
    return tidy;
}

/// Split one converted body at the pictures the scanner drew.
///
/// The scanner escapes every character of prose and code, so the only `<img`
/// in its output is one `imageToHtml()` wrote; the nth such tag is the nth
/// entry in `images`, in output order (recursion appends in the same order it
/// emits). Captions never reach here — a target that does not decode is a
/// `<span>` inside the run, which is exactly the "visible behaviour unchanged"
/// case.
QVariantList splitParts(const QString &html, const QList<Markdown::ImagePart> &images)
{
    QVariantList parts;
    if (html.isEmpty())
        return parts;
    if (images.isEmpty()) {
        parts.append(htmlPartMap(html));
        return parts;
    }

    QStringList open;
    QString run;
    int from = 0;
    int i = 0;
    int next = 0;

    while (i < html.size()) {
        const int lt = html.indexOf(QLatin1Char('<'), i);
        if (lt < 0)
            break;
        const int gt = html.indexOf(QLatin1Char('>'), lt);
        if (gt < 0)
            break;
        const QString tag = html.mid(lt, gt - lt + 1);

        if (tag.startsWith(QLatin1String("<img"))) {
            if (next >= images.size()) {
                // Not a tag the scanner recorded. Leave it in the run rather
                // than inventing a part for it.
                i = gt + 1;
                continue;
            }

            run += html.mid(from, lt - from);
            // Close, outermost last, so each part is a valid document.
            for (int k = open.size() - 1; k >= 0; --k)
                run += QStringLiteral("</%1>").arg(tagName(open.at(k)));
            const QString before = tidyRun(run);
            if (hasVisibleContent(before))
                parts.append(htmlPartMap(before));

            parts.append(imagePartMap(images.at(next++)));

            run.clear();
            // Reopen, outermost first, for the text that follows the picture.
            for (const QString &opened : std::as_const(open))
                run += opened;
            from = gt + 1;
            i = from;
            continue;
        }

        noteTag(tag, &open);
        i = gt + 1;
    }

    run += html.mid(from);
    const QString tail = tidyRun(run);
    if (hasVisibleContent(tail))
        parts.append(htmlPartMap(tail));

    // Defensive: a conversion that recorded a picture but wrote no tag for it
    // is not split, and the whole body stays one run rather than going blank.
    if (next == 0) {
        QVariantList unsplit;
        unsplit.append(htmlPartMap(html));
        return unsplit;
    }
    return parts;
}

}   // namespace

/// The memoised entry point. See the header for why this is cached at all.
QString Markdown::toHtml(const QString &markdown) const
{
    return conversion(markdown).html;
}

QVariantList Markdown::toParts(const QString &markdown) const
{
    const Conversion converted = conversion(markdown);
    return splitParts(converted.html, converted.images);
}

QString Markdown::plainHtml(const QString &text) const
{
    return QStringLiteral("<p style=\"white-space:pre-wrap\">") + escapeHtml(text)
           + QStringLiteral("</p>");
}

/// Look up or build the one conversion both entry points serve.
Markdown::Conversion Markdown::conversion(const QString &markdown) const
{
    QElapsedTimer clock;
    clock.start();

    QStringList reports;
    {
        QMutexLocker locker(&m_mutex);
        if (const Conversion *hit = m_cache.object(markdown)) {
            // Copied out, because an insert from another thread may evict the
            // entry the moment this lock is released. QString and QList are
            // implicitly shared, so this is a refcount, not a body copy.
            const Conversion result = *hit;
            reports = account(clock.elapsed(), markdown.size(), true);
            for (const QString &line : reports)
                qInfo().noquote() << line;
            return result;
        }
    }

    QList<ImagePart> images;
    const QString html = renderToHtml(markdown, &images);
    Conversion result;
    result.html = html;
    result.images = images;

    {
        QMutexLocker locker(&m_mutex);
        reports = account(clock.elapsed(), markdown.size(), false);
        // A document that grew out of the one before it is an answer still
        // arriving: its previous prefix can never be asked for again, so it goes
        // rather than sitting in the budget. The test is a prefix compare, which
        // is orders of magnitude cheaper than the parse above it.
        if (!m_lastDocument.isEmpty() && markdown.size() > m_lastDocument.size()
            && QStringView(markdown).startsWith(m_lastDocument))
            m_cache.remove(m_lastDocument);
        // Cost is what the entry holds, so a transcript's worth of rows fits and
        // one enormous document cannot evict all of them: QCache refuses a cost
        // above the maximum, which is the wanted behaviour for a huge paste.
        m_cache.insert(markdown, new Conversion(result),
                       markdown.size() + html.size());
        m_lastDocument = markdown;
    }
    for (const QString &line : reports)
        qInfo().noquote() << line;
    return result;
}

