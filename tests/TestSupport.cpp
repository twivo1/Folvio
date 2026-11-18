#include "TestSupport.h"

#include "core/deps/DependencyManager.h"
#include "core/process/ProcessRunner.h"
#include "engines/pdf/PdfDocument.h"

#include <QFont>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QStandardPaths>

namespace folvio::test {

bool makeTextPdf(const QString& path, const QList<QStringList>& pages, const QString& family)
{
    QPdfWriter w(path);
    w.setPageSize(QPageSize(QPageSize::A4));
    w.setResolution(72); // 1 device unit == 1 pt
    w.setPageMargins(QMarginsF(0, 0, 0, 0));
    w.setCreator("Folvio tests");
    QPainter p;
    if (!p.begin(&w))
        return false;
    QFont f(family);
    f.setPointSizeF(14);
    p.setFont(f);
    for (int i = 0; i < pages.size(); ++i) {
        if (i > 0)
            w.newPage();
        int y = 100;
        for (const QString& line : pages[i]) {
            p.drawText(72, y, line);
            y += 28;
        }
    }
    return p.end();
}

bool makeScannedPdf(const QString& path, const QString& text)
{
    QImage img(1240, 1754, QImage::Format_RGB32); // A4 @150 dpi
    img.fill(Qt::white);
    {
        QPainter ip(&img);
        QFont f("Arial");
        f.setPixelSize(40);
        ip.setFont(f);
        ip.setPen(Qt::black);
        int y = 200;
        for (const QString& line : text.split('\n')) {
            ip.drawText(150, y, line);
            y += 70;
        }
    }
    QPdfWriter w(path);
    w.setPageSize(QPageSize(QPageSize::A4));
    w.setResolution(150);
    w.setPageMargins(QMarginsF(0, 0, 0, 0));
    QPainter p;
    if (!p.begin(&w))
        return false;
    p.drawImage(QRect(0, 0, w.width(), w.height()), img);
    return p.end();
}

bool makeTablePdf(const QString& path)
{
    QPdfWriter w(path);
    w.setPageSize(QPageSize(QPageSize::A4));
    w.setResolution(72);
    w.setPageMargins(QMarginsF(0, 0, 0, 0));
    QPainter p;
    if (!p.begin(&w))
        return false;
    QFont f("Arial");
    f.setPointSizeF(11);
    p.setFont(f);
    p.drawText(72, 80, "Quarterly report");
    for (int r = 0; r <= 4; ++r)
        p.drawLine(72, 100 + r * 24, 472, 100 + r * 24);
    for (int c = 0; c <= 3; ++c)
        p.drawLine(72 + c * 133, 100, 72 + c * 133, 196);
    const char* cells[4][3] = {{"Item", "Qty", "Price"}, {"Paper", "10", "4.50"}, {"Ink", "2", "19.99"}, {"Pens", "5", "1.20"}};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            p.drawText(80 + c * 133, 117 + r * 24, QString::fromLatin1(cells[r][c]));
    QImage img(120, 80, QImage::Format_RGB32);
    img.fill(QColor("#17A589"));
    p.drawImage(QRect(72, 240, 120, 80), img);
    p.drawText(72, 360, "Image caption");
    return p.end();
}

bool makePng(const QString& path, int w, int h, bool alpha)
{
    QImage img(w, h, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    img.fill(alpha ? QColor(0, 0, 0, 0) : QColor(Qt::white));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor("#17A589"));
    p.drawEllipse(QRect(w / 4, h / 4, w / 2, h / 2));
    p.end();
    return img.save(path, "PNG");
}

bool runFfmpeg(const QStringList& args)
{
    const QString ffmpeg = DependencyManager::instance().path("ffmpeg");
    if (ffmpeg.isEmpty())
        return false;
    ProcessSpec spec;
    spec.program = ffmpeg;
    spec.arguments = QStringList{"-hide_banner", "-loglevel", "error", "-y"} + args;
    spec.timeoutMs = 120000;
    return ProcessRunner::run(spec).succeeded();
}

static QString pythonExe()
{
    // "python3" first: on Linux "python" is often missing; on Windows python3 may be the Store stub,
    // so "python" is preferred there.
#ifdef Q_OS_WIN
    const QStringList names = {"python", "python3"};
#else
    const QStringList names = {"python3", "python"};
#endif
    for (const QString& n : names) {
        const QString p = QStandardPaths::findExecutable(n);
        if (!p.isEmpty())
            return p;
    }
    return {};
}

bool havePython()
{
    if (pythonExe().isEmpty())
        return false;
    ProcessSpec spec;
    spec.program = pythonExe();
    spec.arguments = {"-I", "-c", "import pypdf"};
    return ProcessRunner::run(spec).succeeded();
}

bool runPython(const QString& script, const QStringList& args)
{
    ProcessSpec spec;
    spec.program = pythonExe();
    spec.arguments = QStringList{"-I", "-c", script} + args;
    spec.timeoutMs = 60000;
    return ProcessRunner::run(spec).succeeded();
}

QString pdfText(const QString& path, const QByteArray& password)
{
    auto doc = PdfDocument::openFile(path, password);
    return doc ? doc->allText() : QString();
}

double pixelDifference(const QImage& a, const QImage& b, const QRect& ignore)
{
    if (a.size() != b.size())
        return 1.0;
    const QImage x = a.convertToFormat(QImage::Format_RGB32), y = b.convertToFormat(QImage::Format_RGB32);
    qint64 diff = 0, total = 0;
    for (int j = 0; j < x.height(); ++j) {
        const QRgb* ra = reinterpret_cast<const QRgb*>(x.constScanLine(j));
        const QRgb* rb = reinterpret_cast<const QRgb*>(y.constScanLine(j));
        for (int i = 0; i < x.width(); ++i) {
            if (!ignore.isNull() && ignore.contains(i, j))
                continue;
            ++total;
            const int d = std::abs(qRed(ra[i]) - qRed(rb[i])) + std::abs(qGreen(ra[i]) - qGreen(rb[i]))
                          + std::abs(qBlue(ra[i]) - qBlue(rb[i]));
            if (d > 24)
                ++diff;
        }
    }
    return total ? double(diff) / double(total) : 0.0;
}

} // namespace folvio::test
