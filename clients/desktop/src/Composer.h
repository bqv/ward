#pragma once

#include <QHash>
#include <QString>
#include <QWidget>
#include <Qt>

class ClientFacade;
class MentionPopup;

/// The composer: one field, the controls that trail it, and the rules that turn a
/// keystroke into a prompt.
///
/// This is `main.qml`'s footer composer (main.qml:1540-1855) together with the
/// functions behind it — `submit()` (2158), `commandLine()` (2058),
/// `primaryMode()`/`alternateMode()` (2220), `adoptDraft()` (462) — ported one
/// branch at a time. It is the one place in the window where a keystroke becomes a
/// prompt, so every rule below names the QML line it mirrors rather than being
/// re-derived.
///
/// Values come from `ClientFacade`, never from a QML context property: a C++
/// widget cannot bind by name, and the facade is the one object that knows which
/// transport the window is showing.
class Composer : public QWidget
{
    Q_OBJECT

public:
    explicit Composer(ClientFacade *facade, QWidget *parent = nullptr);

    /// Put the caret in the field, at the end of whatever it holds — the shell's
    /// focus gesture, and main.qml's `input.forceActiveFocus()` equivalent.
    void focusEntry();

    /// The height the footer must give the composer right now: the field, its
    /// margins and, while a notice is up, the notice's line. Arithmetic rather
    /// than a measurement of laid-out children, the same choice main.qml's dock
    /// makes with `dockRows` (main.qml:973): the footer decides the space the
    /// children get, so it must not ask them how much they want.
    int composerHeight() const;

signals:
    /// The composer grew or shrank. The shell's footer follows the height itself;
    /// the signal exists so the page can log the arithmetic it is laying out.
    void heightChanged();

protected:
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    class Entry;
    class Field;
    class Notice;
    class PushButton;
    class Strip;
    class TextButton;

    // ---- the keyboard, exactly as main.qml resolves it --------------------
    //
    // Enter and Ctrl+Enter submit; Shift+Enter is a newline and is left to the
    // field (main.qml:1745-1756). Escape stops a running turn (1764-1772), and
    // Ctrl+V offers the clipboard image without taking the key from an ordinary
    // paste (1757-1763).
    void onEnterPressed(Qt::KeyboardModifiers modifiers);
    bool onEscapePressed();
    void onPasteRequested();

    /// main.qml's `submit(mode)` (2158): empty text is nothing, a single-line `/`
    /// is a command and never prose, and a command is never queued or steered.
    void submitWithMode(const QString &mode);

    /// The command branch of `submit()` (2178-2193), including which drafts are
    /// cleared and which refusals keep the text.
    void runCommandLine(const QString &line);

    /// The `/` list, without a popup: the vocabulary the client can drive
    /// (`slashCommands()`), written into the notice line.
    void showVocabulary();
    void showNotice(const QString &text, bool error);
    void clearNotice();

    // ---- the controls ------------------------------------------------------
    void refreshEnabledState();
    void refreshPlaceholder();
    void refreshModeChip();
    void refreshQueueChip();
    void refreshSendButton();
    void refreshAttachments();
    void clearAttachments();
    void relayoutField();

    /// main.qml's `adoptDraft()` (462): a draft belongs to the session it was
    /// typed in, and is put back when that session returns.
    void adoptDraft(const QString &sessionId);

    /// main.qml's `primaryMode()`/`alternateMode()` (2220-2227), which are the
    /// facade's `promptMode()`/`alternatePromptMode()`.
    QString primaryMode() const;
    QString alternateMode() const;

    /// main.qml's `commandLine()` (2058) and `commandWord()` (2070).
    static QString commandLine(const QString &text);
    static QString commandWord(const QString &line);

    /// The queue row's Steer, compressed to a count while the dock is empty
    /// (main.qml:1509-1523 has the per-item control this stands in for).
    void steerQueuedHead();

    /// main.qml's `composerAttach` (1825) and the `FileDialog` it opens (5944).
    void attachRequested();

    bool onMentionKey(int key, Qt::KeyboardModifiers modifiers);
    void refreshMention();
    void showMention();
    void closeMention(bool clearCandidates);
    void insertMention(const QString &path);
    QString mentionToken() const;

    ClientFacade *m_facade;
    Field *m_field = nullptr;
    Entry *m_entry = nullptr;
    Notice *m_notice = nullptr;
    Strip *m_strip = nullptr;
    TextButton *m_modeChip = nullptr;
    TextButton *m_queueChip = nullptr;
    PushButton *m_attach = nullptr;
    PushButton *m_send = nullptr;

    /// One draft per session id, IN MEMORY ONLY.
    ///
    /// main.qml:441 says why it is not persisted: "a draft is the thought you had
    /// a minute ago, not a file, and a client that restores week-old
    /// half-sentences on launch is a client you have to clean up after." So this
    /// is the same store in the same sense — a map for the window's lifetime and
    /// nothing on disk — rather than a new QSettings key.
    QHash<QString, QString> m_drafts;
    QString m_draftSession;
    MentionPopup *m_mention = nullptr;
    QString m_mentionToken;
    bool m_mentionDismissed = false;
    bool m_insertingMention = false;
    QWidget *m_watchedWindow = nullptr;
    int m_loggedFieldHeight = -1;
    /// The footer height last announced, so a keystroke that does not grow the
    /// field does not make the shell relayout and log.
    int m_announcedHeight = -1;

    /// Guards the relayout against re-entry: a height change inside a resize can
    /// bring the resize handler back around.
    bool m_relayingOut = false;
};
