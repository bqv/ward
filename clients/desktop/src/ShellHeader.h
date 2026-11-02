#pragma once

#include <QWidget>

class ClientFacade;

/// The application's bar: the connection dot, the `DSH` brand and the three pane
/// toggles, 39 px across the top of the window.
///
/// This is `main.qml`'s `header` (main.qml:726), and it is the APPLICATION's bar
/// rather than the conversation's: a pane is a surface of the application, so its
/// way in belongs here, and everything true of the session on screen is in the
/// status strip below the content instead. The two bars have two subjects, which
/// is why they are two widgets and not one taller one.
///
/// The toggles are `QToolButton`s in behaviour — checkable, clicked, with a
/// tooltip — and custom paint in appearance. A Fusion tool button draws a bevelled
/// gradient slab; the header's toggles are flat bordered chips with no bevel, lit
/// only under the pointer or while their pane is up. That is a shape Fusion has no
/// primitive for, so `paintEvent` draws it from the token table rather than a
/// `QProxyStyle` bending a stock control into something it is not.
class ShellHeader : public QWidget
{
    Q_OBJECT

public:
    /// `facade` supplies the connection dot's state and whether the ACP toggle may
    /// be offered at all. It is borrowed and outlives the header.
    explicit ShellHeader(ClientFacade *facade, QWidget *parent = nullptr);

    /// Put the toggles' checked state in step with the page on screen. The header
    /// never decides which page is shown — the shell does — so this is written
    /// once after every switch and the toggles read as one exclusive choice.
    void setCurrentPage(const QString &page);

signals:
    /// A toggle was pressed. `page` is the pane the press was FOR ("web", "acp",
    /// "headless"); the shell turns a press on the page already showing into a
    /// return to the chat page, which is the QML toggles' put-away-and-return
    /// behaviour. Emitted as a request rather than a stack call so this widget
    /// knows nothing about the pages.
    void paneToggleRequested(const QString &page);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    class BrandMark;
    class Toggle;

    ClientFacade *m_facade;
    BrandMark *m_brand;
    Toggle *m_web;
    Toggle *m_acp;
    Toggle *m_headless;
    /// The page whose toggle is lit, so a page switch is logged once rather than
    /// on every sync.
    QString m_page;
};
