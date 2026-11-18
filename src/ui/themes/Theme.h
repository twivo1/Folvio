#pragma once

#include <QColor>
#include <QObject>

class QApplication;

namespace folvio {

// Design tokens. One accent colour, neutral graphite surfaces, small radii.
struct ThemeColors {
    QColor window;        // app background
    QColor surface;       // panels, cards
    QColor surfaceAlt;    // sidebar, toolbars
    QColor sunken;        // page canvas background
    QColor border;
    QColor borderStrong;
    QColor text;
    QColor textMuted;
    QColor textDisabled;
    QColor accent;
    QColor accentHover;
    QColor accentText;    // text on accent
    QColor accentSoft;    // selection / hover tint
    QColor warning;
    QColor warningSoft;
    QColor danger;
    QColor dangerSoft;
    QColor success;
    QColor info;
    QColor infoSoft;
};

class Theme : public QObject {
    Q_OBJECT
public:
    enum class Mode { System, Light, Dark };
    Q_ENUM(Mode)

    static Theme& instance();
    void apply(Mode mode);
    Mode mode() const { return m_mode; }
    bool isDark() const { return m_dark; }
    const ThemeColors& colors() const { return m_colors; }

    static QString modeToString(Mode m);
    static Mode modeFromString(const QString& s);

Q_SIGNALS:
    void changed();

private:
    Theme() = default;
    QString styleSheet() const;
    Mode m_mode = Mode::System;
    bool m_dark = false;
    ThemeColors m_colors;
};

inline const ThemeColors& tc() { return Theme::instance().colors(); }

} // namespace folvio
