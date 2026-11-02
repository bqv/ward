#pragma once

#include <QPointer>
#include <QRect>
#include <QString>
#include <QVariantMap>
#include <QWidget>

class ClientFacade;
class QActionGroup;
class QMenu;

/// The conversation's bar, 24 px: parent seat · status · access chip · session id,
/// then model · effort · context · jobs.
///
/// This is `main.qml`'s `statusBar` (main.qml:2932), and it is the CONVERSATION's
/// bar where the header is the application's: the parent link, the live status,
/// the access level, the id, the context count, the jobs and the model are all
/// facts about the session on screen, so they sit at its edge. The reference for
/// ORDER, SPACING and ELISION is that strip; the placement differs because this
/// slice hangs it directly above the composer's hole, where the plan's chat-page
/// arithmetic puts it (header 39, content, strip 24, composer 35).
///
/// The fill is `raised` with an accent hairline on its top edge and a `border`
/// hairline under it. That is not decoration: on `panel` this strip measured
/// 1.00:1 against the header above it and was invisible (main.qml:2895 records
/// the measurement). Both hairlines are fills, never text; the accent as a word
/// would be 1.91:1 here.
///
/// Every value is read from `ClientFacade`, never from a QML context property:
/// a C++ widget cannot bind by name, and the facade is the one place that knows
/// which transport is behind the window.
class StatusStrip : public QWidget
{
    Q_OBJECT

public:
    explicit StatusStrip(ClientFacade *facade, QWidget *parent = nullptr);

    /// 24 px: one line of 12 px metadata between two 6 px insets — the QML's own
    /// `barHeight` arithmetic, and a constant because the layout around it is
    /// computed from this number before any child is laid out.
    static int stripHeight();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    class PlainCell;
    class HoverCell;
    class ChipCell;
    class SessionIdCell;
    class ContextCell;

    /// Recompute every cell from the facade. Called on construction and from each
    /// transport fact the strip draws. Eight short strings on one 24 px row: a
    /// blanket recompute is cheaper to keep honest than eight independent
    /// dependencies that can disagree about when a derived label changed.
    void refresh();

    // ---- the three chip menus (plan §5 stage 1: "QMenu for the three strip
    // chips") -------------------------------------------------------------
    //
    // One per chip that has a popover in `qml/main.qml`: the model, the effort
    // and the access chip. Each builds its QMenu on the opening, because the
    // catalog behind it is a live surface and a QMenu holds the actions it was
    // built with. The context chip has no menu in the QML — it is a ring and a
    // tooltip — and the jobs chip's roster is not this slice; neither is wired
    // here, which is why the comment on `ChipCell` names the three that are.
    void openModelMenu();
    void openEffortMenu();
    void openAccessMenu();
    void onDangerousPresetRequested(const QString &id, const QString &warning);
    int fillModelMenu();
    int fillAccessMenu();
    void chooseModel(const QString &value);

    // ---- derived values, mirroring main.qml's statusBar ---------------------
    //
    // The QML keeps its copy of these rules while both shells exist (plan §10a);
    // the values come from the same host projection. What is NOT duplicated is
    // any colour or measurement — those are tokens.
    QVariantMap modelOption() const;
    QVariantMap effortOption() const;
    QString choiceLabel(const QVariantMap &option) const;
    QString choiceDescription(const QVariantMap &option) const;
    QString modelName() const;
    QString effortLabel() const;
    QString accessLevelName() const;
    QString accessDescription() const;
    bool accessDangerous() const;
    QString jobText() const;
    bool jobsLive() const;
    int contextPercent() const;

    ClientFacade *m_facade;
    /// The line last written to the log, so the strip reports its values when they
    /// change rather than on every transport notify.
    QString m_loggedLine;
    HoverCell *m_parent;
    PlainCell *m_status;
    ChipCell *m_access;
    SessionIdCell *m_session;
    ChipCell *m_model;
    ChipCell *m_effort;
    ContextCell *m_context;
    ChipCell *m_jobs;
    QPointer<QMenu> m_modelMenu;
    QPointer<QActionGroup> m_modelGroup;
    QRect m_modelAnchor;
    QString m_modelConfigId;
    QPointer<QMenu> m_accessMenu;
    QPointer<QActionGroup> m_accessGroup;
};
