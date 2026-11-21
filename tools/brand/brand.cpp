// Generates raster/ICO/wordmark assets from the master SVG logo.
// Usage: folvio_brand <assets/branding dir>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QSvgGenerator>
#include <QSvgRenderer>
#include <QtEndian>
#include <cstdio>

static QImage renderSvg(const QString& file, QSize size, QColor bg = Qt::transparent)
{
    QSvgRenderer r(file);
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(bg);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    r.render(&p, QRectF(QPointF(0, 0), size));
    return img;
}

static bool writeIco(const QString& path, const QList<QImage>& images)
{
    QByteArray dir, data;
    const int count = int(images.size());
    int offset = 6 + 16 * count;
    for (const QImage& img : images) {
        QByteArray png;
        QBuffer b(&png);
        b.open(QIODevice::WriteOnly);
        img.save(&b, "PNG");
        char e[16] = {0};
        e[0] = char(img.width() >= 256 ? 0 : img.width());
        e[1] = char(img.height() >= 256 ? 0 : img.height());
        qToLittleEndian<quint16>(1, e + 4);   // planes
        qToLittleEndian<quint16>(32, e + 6);  // bpp
        qToLittleEndian<quint32>(quint32(png.size()), e + 8);
        qToLittleEndian<quint32>(quint32(offset), e + 12);
        dir.append(e, 16);
        data.append(png);
        offset += int(png.size());
    }
    char h[6] = {0};
    qToLittleEndian<quint16>(1, h + 2);
    qToLittleEndian<quint16>(quint16(count), h + 4);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(h, 6);
    f.write(dir);
    f.write(data);
    return true;
}

static QPainterPath wordPath(qreal px)
{
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(int(px));
    font.setWeight(QFont::DemiBold);
    font.setLetterSpacing(QFont::PercentageSpacing, 98);
    QPainterPath path;
    path.addText(0, 0, font, QStringLiteral("Folvio"));
    return path;
}

static void writeWordmark(const QString& dir, const QString& name, const QString& markSvg, QColor text)
{
    // Layout: mark (height 120) + 28 gap + text (cap height ~ 0.72 of mark)
    const qreal h = 160, markH = 128;
    QPainterPath word = wordPath(118);
    const QRectF wb = word.boundingRect();
    const qreal markW = markH * 580.0 / 640.0;
    const qreal width = 16 + markW + 26 + wb.width() + 16;
    QSvgRenderer mark(markSvg);
    auto paint = [&](QPainter& p) {
        p.setRenderHint(QPainter::Antialiasing);
        mark.render(&p, QRectF(16, (h - markH) / 2, markW, markH));
        p.translate(16 + markW + 26 - wb.left(), h / 2 - wb.center().y() + 2);
        p.fillPath(word, text);
    };
    QSvgGenerator gen;
    gen.setFileName(dir + "/" + name + ".svg");
    gen.setSize(QSize(int(width), int(h)));
    gen.setViewBox(QRectF(0, 0, width, h));
    gen.setTitle("Folvio");
    gen.setDescription("Folvio wordmark");
    {
        QPainter p(&gen);
        paint(p);
    }
    QImage img(QSize(int(width * 2), int(h * 2)), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.scale(2, 2);
        paint(p);
    }
    img.save(dir + "/" + name + ".png");
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 2) { std::fprintf(stderr, "usage: folvio_brand <branding dir>\n"); return 1; }
    const QString dir = QString::fromLocal8Bit(argv[1]);
    const QString icon = dir + "/folvio-icon.svg";
    for (int s : {1024, 512, 256, 128, 64, 48, 32, 16})
        renderSvg(icon, QSize(s, s)).save(QStringLiteral("%1/png/folvio-icon-%2.png").arg(dir).arg(s));
    QList<QImage> icoImages;
    for (int s : {256, 128, 64, 48, 40, 32, 24, 20, 16})
        icoImages << renderSvg(icon, QSize(s, s));
    if (!writeIco(dir + "/folvio.ico", icoImages)) return 2;
    for (const char* v : {"folvio-mark-light-bg", "folvio-mark-dark-bg", "folvio-mark-mono", "folvio-mark-mono-white"})
        renderSvg(dir + "/" + v + ".svg", QSize(580, 640)).save(QStringLiteral("%1/png/%2.png").arg(dir, v));
    writeWordmark(dir, "folvio-wordmark-light-bg", dir + "/folvio-mark-light-bg.svg", QColor("#15191C"));
    writeWordmark(dir, "folvio-wordmark-dark-bg", dir + "/folvio-mark-dark-bg.svg", QColor("#F2F4F3"));
    writeWordmark(dir, "folvio-wordmark-mono", dir + "/folvio-mark-mono.svg", QColor("#000000"));

    // Installer artwork (Inno Setup): large 164x314 @2x and small 55x55 @2x, 24-bit BMP.
    {
        QImage big(QSize(328, 628), QImage::Format_RGB32);
        big.fill(QColor("#15191C"));
        QPainter p(&big);
        p.setRenderHint(QPainter::Antialiasing);
        QSvgRenderer m(dir + "/folvio-mark-dark-bg.svg");
        m.render(&p, QRectF(84, 220, 160, 176));
        p.end();
        big.save(dir + "/installer-wizard.bmp", "BMP");
        QImage small = renderSvg(icon, QSize(110, 110), Qt::white).convertToFormat(QImage::Format_RGB32);
        small.save(dir + "/installer-small.bmp", "BMP");
    }
    std::printf("Brand assets written to %s\n", qPrintable(dir));
    return 0;
}
