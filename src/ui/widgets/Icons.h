#pragma once

#include <QIcon>
#include <QString>

namespace folvio {

// Consistent 24x24 stroke icon set drawn from inline SVG. Icons are coloured
// at paint time from the active Theme, so they follow light/dark switches.
class Icons {
public:
    enum class Tone { Normal, Accent, Muted, Danger, Warning, OnAccent };
    static QIcon get(const QString& name, Tone tone = Tone::Normal);
    static QPixmap pixmap(const QString& name, int size, Tone tone = Tone::Normal, qreal dpr = 1.0);
    static QIcon appIcon();
};

} // namespace folvio
