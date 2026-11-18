#include "ui/widgets/Icons.h"

#include "ui/themes/Theme.h"

#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmapCache>
#include <QSvgRenderer>

namespace folvio {

namespace {

// Path data on a 24x24 grid, stroked (no fill) unless prefixed with "F:".
const QHash<QString, QString>& paths()
{
    static const QHash<QString, QString> p = {
        {"home", "M3 11 L12 4 L21 11 M5.5 9.5 V20 H10 V14 H14 V20 H18.5 V9.5"},
        {"convert", "M4 8 H18 M14 4 L18 8 L14 12 M20 16 H6 M10 12 L6 16 L10 20"},
        {"edit", "M6 3 H13 L17 7 V11 M6 3 V21 H11 M13 3 V7 H17 M13.5 21 L21 13.5 L18.5 11 L11 18.5 V21 Z"},
        {"rotate-left", "M4.5 9 A8 8 0 1 1 6.3 17.5 M4.5 3.5 V9 H10"},
        {"rotate-right", "M19.5 9 A8 8 0 1 0 17.7 17.5 M19.5 3.5 V9 H14"},
        {"combine", "M4 3.5 H10.5 V10 H4 Z M13.5 3.5 H20 V10 H13.5 Z M4 14 H10.5 V20.5 H4 Z M13.5 17.25 H20 M16.75 14 V20.5"},
        {"batch", "M4 7.5 L12 3.5 L20 7.5 L12 11.5 Z M4 12 L12 16 L20 12 M4 16.5 L12 20.5 L20 16.5"},
        {"recent", "M12 7.5 V12 L15 14 M12 4 A8 8 0 1 1 11.99 4 Z"},
        {"settings", "M4 7 H13 M17 7 H20 M15 5 V9 M4 17 H7 M11 17 H20 M9 15 V19"},
        {"open", "M3 6.5 V18.5 H21 V9 H11.5 L9.5 6.5 Z"},
        {"save", "M5 4 H16 L20 8 V20 H4 V4 Z M8 4 V9 H15 V4 M7.5 20 V14 H16.5 V20"},
        {"save-as", "M5 4 H14 L18 8 V11.5 M5 4 V20 H11 M8 4 V9 H14 M14 21 L21 14 L19 12 L12 19 V21 Z"},
        {"export", "M12 15 V3 M7.5 7.5 L12 3 L16.5 7.5 M5 13 V20 H19 V13"},
        {"undo", "M9 6 L4 11 L9 16 M4 11 H14 C17.3 11 20 13.7 20 17 V19"},
        {"redo", "M15 6 L20 11 L15 16 M20 11 H10 C6.7 11 4 13.7 4 17 V19"},
        {"bold", "M7 5 H13 C17.5 5 17.5 11.5 13 11.5 H7 Z M7 11.5 H14 C18.8 11.5 18.8 19 14 19 H7 Z"},
        {"italic", "M10 5 H18 M6 19 H14 M14 5 L10 19"},
        {"underline", "M7 4 V11 C7 17.5 17 17.5 17 11 V4 M5 20.5 H19"},
        {"align-left", "M4 6 H20 M4 10 H14 M4 14 H20 M4 18 H14"},
        {"align-center", "M4 6 H20 M7 10 H17 M4 14 H20 M7 18 H17"},
        {"align-right", "M4 6 H20 M10 10 H20 M4 14 H20 M10 18 H20"},
        {"align-justify", "M4 6 H20 M4 10 H20 M4 14 H20 M4 18 H20"},
        {"list-bullet", "M10 6 H20 M10 12 H20 M10 18 H20 M5 6 H5.01 M5 12 H5.01 M5 18 H5.01"},
        {"list-number", "M10 6 H20 M10 12 H20 M10 18 H20 M4 4.5 L5.5 3.8 V8.5 M3.8 8.5 H7 M4 11 C4 9.5 7 9.5 7 11 C7 12.2 4 13 4 14.5 H7"},
        {"indent", "M4 5 H20 M11 10 H20 M11 14 H20 M4 19 H20 M4 9 L7 12 L4 15"},
        {"outdent", "M4 5 H20 M11 10 H20 M11 14 H20 M4 19 H20 M7 9 L4 12 L7 15"},
        {"line-spacing", "M11 6 H20 M11 12 H20 M11 18 H20 M6 4 V20 M3.5 6.5 L6 4 L8.5 6.5 M3.5 17.5 L6 20 L8.5 17.5"},
        {"compare", "M3 5 H10 V19 H3 Z M14 5 H21 V19 H14 Z M12 2.5 V21.5"},
        {"zoom-in", "M20 20 L15.5 15.5 M11 8 V14 M8 11 H14 M11 4.5 A6.5 6.5 0 1 1 10.99 4.5 Z"},
        {"zoom-out", "M20 20 L15.5 15.5 M8 11 H14 M11 4.5 A6.5 6.5 0 1 1 10.99 4.5 Z"},
        {"fit-width", "M4 5 V19 M20 5 V19 M8 12 H16 M10.5 9.5 L8 12 L10.5 14.5 M13.5 9.5 L16 12 L13.5 14.5"},
        {"search", "M20 20 L15.5 15.5 M11 4.5 A6.5 6.5 0 1 1 10.99 4.5 Z"},
        {"replace", "M4 7 H14 M11 4 L14 7 L11 10 M20 17 H10 M13 14 L10 17 L13 20"},
        {"image", "M4 5 H20 V19 H4 Z M4 16 L9 11 L13 15 L15 13 L20 18 M15.5 7.5 A1.5 1.5 0 1 1 15.49 7.5 Z"},
        {"table", "M4 5 H20 V19 H4 Z M4 10 H20 M4 14.5 H20 M10 5 V19 M15 5 V19"},
        {"text-add", "M4 6 V4 H14 V6 M9 4 V18 M7 18 H11 M18 13 V21 M14 17 H22"},
        {"text-cursor", "M9 4 H15 M9 20 H15 M12 4 V20"},
        {"trash", "M5 7 H19 M10 7 V4 H14 V7 M7 7 L8 20 H16 L17 7 M10.5 11 V16 M13.5 11 V16"},
        {"play", "M8 5 L19 12 L8 19 Z"},
        {"cancel", "M9 9 L15 15 M15 9 L9 15 M12 4 A8 8 0 1 1 11.99 4 Z"},
        {"retry", "M19.5 12 A7.5 7.5 0 1 1 17.3 6.7 M19.5 4 V8.5 H15"},
        {"check", "M5 12.5 L10 17 L19 7"},
        {"warning", "M12 3.5 L21.5 20 H2.5 Z M12 10 V14 M12 17 H12.01"},
        {"info", "M12 11 V17 M12 7.5 H12.01 M12 4 A8 8 0 1 1 11.99 4 Z"},
        {"close", "M6 6 L18 18 M18 6 L6 18"},
        {"chevron-left", "M15 5 L8 12 L15 19"},
        {"chevron-right", "M9 5 L16 12 L9 19"},
        {"chevron-up", "M6 15 L12 9 L18 15"},
        {"chevron-down", "M6 9 L12 15 L18 9"},
        {"ocr", "M4 8 V4 H8 M16 4 H20 V8 M20 16 V20 H16 M8 20 H4 V16 M8 10 H16 M8 14 H13"},
        {"file", "M6 3 H14 L19 8 V21 H6 Z M14 3 V8 H19"},
        {"text-color", "M6.5 16 L12 4 L17.5 16 M8.6 11.5 H15.4"},
        {"plus", "M12 5 V19 M5 12 H19"},
        {"minus", "M5 12 H19"},
        {"folder", "M3 6.5 V18.5 H21 V9 H11.5 L9.5 6.5 Z"},
        {"lock", "M6 11 H18 V20 H6 Z M8.5 11 V8 C8.5 3.5 15.5 3.5 15.5 8 V11"},
        {"signature", "M3 17 C6 17 6 9 9 9 C12 9 9.5 17 12.5 17 C14.5 17 15 13 17 13 C18.5 13 18.5 16 21 16 M3 21 H21"},
        {"reflow", "M4 6 H20 M4 11 H17 C21 11 21 17 17 17 H12 M14.5 14.5 L12 17 L14.5 19.5 M4 17 H8"},
        {"page", "M6 3 H18 V21 H6 Z M9 8 H15 M9 12 H15 M9 16 H13"},
        {"dot", "F:M12 9 A3 3 0 1 1 11.99 9 Z"},
    };
    return p;
}

QColor toneColor(Icons::Tone tone, QIcon::Mode mode)
{
    const ThemeColors& c = tc();
    if (mode == QIcon::Disabled)
        return c.textDisabled;
    switch (tone) {
    case Icons::Tone::Accent: return c.accent;
    case Icons::Tone::Muted: return c.textMuted;
    case Icons::Tone::Danger: return c.danger;
    case Icons::Tone::Warning: return c.warning;
    case Icons::Tone::OnAccent: return c.accentText;
    case Icons::Tone::Normal: break;
    }
    return c.text;
}

QByteArray svgFor(const QString& name, const QColor& color)
{
    QString d = paths().value(name, paths().value("file"));
    bool fill = d.startsWith("F:");
    if (fill)
        d = d.mid(2);
    const QString attrs = fill ? QStringLiteral("fill=\"%1\" stroke=\"none\"").arg(color.name())
                               : QStringLiteral("fill=\"none\" stroke=\"%1\" stroke-width=\"1.6\" "
                                                "stroke-linecap=\"round\" stroke-linejoin=\"round\"")
                                     .arg(color.name());
    return QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\"><path d=\"%1\" %2/></svg>")
        .arg(d, attrs)
        .toUtf8();
}

class ThemedIconEngine : public QIconEngine {
public:
    ThemedIconEngine(QString name, Icons::Tone tone) : m_name(std::move(name)), m_tone(tone) {}
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        QSvgRenderer r(svgFor(m_name, toneColor(m_tone, mode)));
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        r.render(painter, QRectF(rect));
        painter->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        const QColor color = toneColor(m_tone, mode);
        const QString key = QStringLiteral("fvicon:%1:%2:%3x%4@%5").arg(m_name, color.name()).arg(size.width()).arg(size.height()).arg(scale);
        QPixmap pm;
        if (QPixmapCache::find(key, &pm))
            return pm;
        pm = QPixmap(size * scale);
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            paint(&p, QRect(QPoint(0, 0), size * scale), mode, state);
        }
        pm.setDevicePixelRatio(scale);
        QPixmapCache::insert(key, pm);
        return pm;
    }
    QIconEngine* clone() const override { return new ThemedIconEngine(m_name, m_tone); }
    QString key() const override { return QStringLiteral("folvio-themed"); }

private:
    QString m_name;
    Icons::Tone m_tone;
};

} // namespace

QIcon Icons::get(const QString& name, Tone tone)
{
    return QIcon(new ThemedIconEngine(name, tone));
}

QPixmap Icons::pixmap(const QString& name, int size, Tone tone, qreal dpr)
{
    return get(name, tone).pixmap(QSize(size, size), dpr);
}

QIcon Icons::appIcon()
{
    QIcon icon;
    for (int s : {16, 24, 32, 48, 64, 128, 256}) {
        QSvgRenderer r(QStringLiteral(":/branding/icon.svg"));
        QPixmap pm(s, s);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        r.render(&p);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

} // namespace folvio
