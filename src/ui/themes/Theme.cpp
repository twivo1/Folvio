#include "ui/themes/Theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>

namespace folvio {

Theme& Theme::instance()
{
    static Theme t;
    return t;
}

QString Theme::modeToString(Mode m)
{
    switch (m) {
    case Mode::Light: return QStringLiteral("light");
    case Mode::Dark: return QStringLiteral("dark");
    case Mode::System: break;
    }
    return QStringLiteral("system");
}

Theme::Mode Theme::modeFromString(const QString& s)
{
    if (s == "light")
        return Mode::Light;
    if (s == "dark")
        return Mode::Dark;
    return Mode::System;
}

namespace {

ThemeColors lightColors()
{
    ThemeColors c;
    c.window = QColor("#F3F4F4");
    c.surface = QColor("#FFFFFF");
    c.surfaceAlt = QColor("#E9ECEC");
    c.sunken = QColor("#DADFDF");
    c.border = QColor("#D3D8D8");
    c.borderStrong = QColor("#AEB6B8");
    c.text = QColor("#15191C");
    c.textMuted = QColor("#596468");
    c.textDisabled = QColor("#A3ABAE");
    c.accent = QColor("#11806A");
    c.accentHover = QColor("#0C6B58");
    c.accentText = QColor("#FFFFFF");
    c.accentSoft = QColor("#D5ECE6");
    c.warning = QColor("#9A5B00");
    c.warningSoft = QColor("#FCEFD8");
    c.danger = QColor("#B42318");
    c.dangerSoft = QColor("#FBE4E1");
    c.success = QColor("#11806A");
    c.info = QColor("#255C99");
    c.infoSoft = QColor("#E1ECF8");
    return c;
}

ThemeColors darkColors()
{
    ThemeColors c;
    c.window = QColor("#1B1F22");
    c.surface = QColor("#23282C");
    c.surfaceAlt = QColor("#171A1D");
    c.sunken = QColor("#121517");
    c.border = QColor("#343B40");
    c.borderStrong = QColor("#4C555B");
    c.text = QColor("#E5E9EA");
    c.textMuted = QColor("#98A3A7");
    c.textDisabled = QColor("#5C666B");
    c.accent = QColor("#22B597");
    c.accentHover = QColor("#3CC9AB");
    c.accentText = QColor("#0B1412");
    c.accentSoft = QColor("#1D3B35");
    c.warning = QColor("#E5A54B");
    c.warningSoft = QColor("#3A2E1C");
    c.danger = QColor("#F07D72");
    c.dangerSoft = QColor("#43231F");
    c.success = QColor("#22B597");
    c.info = QColor("#7DB0EA");
    c.infoSoft = QColor("#1E2D3E");
    return c;
}

} // namespace

void Theme::apply(Mode mode)
{
    m_mode = mode;
    bool dark = mode == Mode::Dark;
    if (mode == Mode::System)
        dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    m_dark = dark;
    m_colors = dark ? darkColors() : lightColors();
    const ThemeColors& c = m_colors;

    qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    p.setColor(QPalette::Window, c.window);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.surface);
    p.setColor(QPalette::AlternateBase, c.window);
    p.setColor(QPalette::ToolTipBase, c.surface);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::PlaceholderText, c.textMuted);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.surface);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::BrightText, c.danger);
    p.setColor(QPalette::Highlight, c.accent);
    p.setColor(QPalette::HighlightedText, c.accentText);
    p.setColor(QPalette::Link, c.accent);
    p.setColor(QPalette::Mid, c.border);
    p.setColor(QPalette::Midlight, c.surfaceAlt);
    p.setColor(QPalette::Dark, c.borderStrong);
    p.setColor(QPalette::Shadow, dark ? QColor(0, 0, 0) : QColor(0, 0, 0, 90));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, c.textDisabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, c.border);
    qApp->setPalette(p);
    qApp->setStyleSheet(styleSheet());
    Q_EMIT changed();
}

QString Theme::styleSheet() const
{
    const ThemeColors& c = m_colors;
    QString qss = QStringLiteral(R"(
QWidget { font-family: "Segoe UI", "Segoe UI Variable Text", sans-serif; font-size: 9.5pt; }
QToolTip { color: @text; background: @surface; border: 1px solid @border; padding: 4px 6px; }

/* Sidebar ------------------------------------------------------------- */
#Sidebar { background: @surfaceAlt; border-right: 1px solid @border; }
#Sidebar QToolButton { border: none; border-radius: 4px; padding: 7px 10px; text-align: left;
                       color: @text; background: transparent; font-size: 9.5pt; }
#Sidebar QToolButton:hover { background: @hoverTint; }
#Sidebar QToolButton:checked { background: @accentSoft; color: @accentStrongText; font-weight: 600; }
#Sidebar QToolButton:focus { outline: none; border: 1px solid @accent; }
#SidebarFooter { color: @textMuted; font-size: 8pt; }

/* Page header --------------------------------------------------------- */
#PageTitle { font-size: 14pt; font-weight: 600; color: @text; }
#PageSubtitle { color: @textMuted; }
#SectionTitle { font-size: 10pt; font-weight: 600; color: @text; }
#Muted, QLabel[muted="true"] { color: @textMuted; }

/* Cards / panels -------------------------------------------------------- */
#Card, QFrame[card="true"] { background: @surface; border: 1px solid @border; border-radius: 6px; }
#Toolbar { background: @surface; border-bottom: 1px solid @border; }
#Toolbar QToolButton, QToolBar QToolButton { border: 1px solid transparent; border-radius: 4px; padding: 4px; }
#Toolbar QToolButton:hover, QToolBar QToolButton:hover { background: @hoverTint; border-color: @border; }
#Toolbar QToolButton:checked, QToolBar QToolButton:checked { background: @accentSoft; border-color: @accent; }
#Toolbar QToolButton:disabled { color: @textDisabled; }
#Toolbar QFrame[separator="true"] { background: @border; min-width: 1px; max-width: 1px; margin: 4px 4px; }
#DocTitle { font-weight: 600; font-size: 10pt; }

/* Buttons ------------------------------------------------------------- */
QPushButton { background: @surface; border: 1px solid @borderStrong; border-radius: 4px; padding: 5px 14px;
              color: @text; min-height: 18px; }
QPushButton:hover { background: @hoverTint; }
QPushButton:pressed { background: @border; }
QPushButton:disabled { color: @textDisabled; border-color: @border; }
QPushButton:focus { border: 1px solid @accent; }
QPushButton[primary="true"] { background: @accent; border: 1px solid @accent; color: @accentText; font-weight: 600; }
QPushButton[primary="true"]:hover { background: @accentHover; border-color: @accentHover; }
QPushButton[primary="true"]:disabled { background: @border; border-color: @border; color: @textDisabled; }
QPushButton[flat="true"] { border: none; background: transparent; color: @accent; padding: 4px 6px; }
QPushButton[flat="true"]:hover { text-decoration: underline; }

/* Inputs -------------------------------------------------------------- */
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: @surface; border: 1px solid @borderStrong;
              border-radius: 4px; padding: 3px 6px; min-height: 20px; selection-background-color: @accent; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: @accent; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: @surface; border: 1px solid @border; selection-background-color: @accentSoft;
              selection-color: @text; outline: none; }
QCheckBox, QRadioButton { spacing: 6px; }

/* Lists & tables ------------------------------------------------------ */
QTreeView, QListView, QTableView { background: @surface; border: 1px solid @border; border-radius: 4px;
              alternate-background-color: @window; outline: none; }
QTreeView::item, QTableView::item { padding: 3px 4px; }
QTreeView::item:selected, QListView::item:selected, QTableView::item:selected { background: @accentSoft; color: @text; }
QTreeView::item:hover, QListView::item:hover { background: @hoverTint; }
QHeaderView::section { background: @surfaceAlt; color: @textMuted; border: none; border-bottom: 1px solid @border;
              border-right: 1px solid @border; padding: 4px 6px; font-weight: 600; }

/* Scroll bars: thin, neutral ------------------------------------------ */
QScrollBar:vertical { background: transparent; width: 11px; margin: 0; }
QScrollBar::handle:vertical { background: @scroll; border-radius: 4px; min-height: 28px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 0; }
QScrollBar::handle:horizontal { background: @scroll; border-radius: 4px; min-width: 28px; margin: 2px; }
QScrollBar::handle:hover { background: @borderStrong; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* Progress ------------------------------------------------------------ */
QProgressBar { background: @surfaceAlt; border: 1px solid @border; border-radius: 3px; text-align: center;
               color: @text; max-height: 14px; font-size: 8pt; }
QProgressBar::chunk { background: @accent; border-radius: 2px; }

/* Status bar ---------------------------------------------------------- */
QStatusBar { background: @surfaceAlt; border-top: 1px solid @border; color: @textMuted; }
QStatusBar QLabel { color: @textMuted; padding: 0 8px; }
QStatusBar::item { border: none; }

/* Tabs ---------------------------------------------------------------- */
QTabWidget::pane { border: 1px solid @border; border-radius: 4px; top: -1px; background: @surface; }
QTabBar::tab { background: transparent; border: none; padding: 6px 12px; color: @textMuted; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; font-weight: 600; }
QTabBar::tab:hover { color: @text; }

QMenu { background: @surface; border: 1px solid @border; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 24px; border-radius: 3px; }
QMenu::item:selected { background: @accentSoft; color: @text; }
QMenu::separator { height: 1px; background: @border; margin: 4px 6px; }
QSplitter::handle { background: @border; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

/* Banners ------------------------------------------------------------- */
#Banner[kind="info"] { background: @infoSoft; border: 1px solid @info; border-radius: 4px; }
#Banner[kind="warning"] { background: @warningSoft; border: 1px solid @warning; border-radius: 4px; }
#Banner[kind="error"] { background: @dangerSoft; border: 1px solid @danger; border-radius: 4px; }
#Banner[kind="success"] { background: @accentSoft; border: 1px solid @accent; border-radius: 4px; }
#Banner QLabel { color: @text; }

#FormatChip { border: 1px solid @borderStrong; border-radius: 4px; padding: 6px 10px; background: @surface;
              font-weight: 600; min-width: 54px; }
#FormatChip:hover { border-color: @accent; }
#FormatChip:checked { background: @accentSoft; border-color: @accent; color: @accentStrongText; }
#FormatGroupLabel { color: @textMuted; font-size: 8pt; font-weight: 600; text-transform: uppercase; }
)");
    const QColor hover = m_dark ? QColor(255, 255, 255, 18) : QColor(0, 0, 0, 14);
    const QColor scroll = m_dark ? QColor("#3A4247") : QColor("#C3CACC");
    const QColor accentStrongText = m_dark ? c.accentHover : c.accentHover;
    auto rgba = [](const QColor& col) {
        return QStringLiteral("rgba(%1,%2,%3,%4)").arg(col.red()).arg(col.green()).arg(col.blue()).arg(col.alpha());
    };
    const QList<std::pair<QString, QString>> tokens = {
        {"@hoverTint", rgba(hover)}, {"@accentStrongText", accentStrongText.name()}, {"@accentSoft", c.accentSoft.name()},
        {"@accentHover", c.accentHover.name()}, {"@accentText", c.accentText.name()}, {"@accent", c.accent.name()},
        {"@surfaceAlt", c.surfaceAlt.name()}, {"@surface", c.surface.name()}, {"@window", c.window.name()},
        {"@borderStrong", c.borderStrong.name()}, {"@border", c.border.name()}, {"@textMuted", c.textMuted.name()},
        {"@textDisabled", c.textDisabled.name()}, {"@text", c.text.name()}, {"@scroll", scroll.name()},
        {"@warningSoft", c.warningSoft.name()}, {"@warning", c.warning.name()}, {"@dangerSoft", c.dangerSoft.name()},
        {"@danger", c.danger.name()}, {"@infoSoft", c.infoSoft.name()}, {"@info", c.info.name()},
    };
    for (const auto& [k, v] : tokens)
        qss.replace(k, v);
    return qss;
}

} // namespace folvio
